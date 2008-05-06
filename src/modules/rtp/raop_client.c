/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2008 Colin Guthrie

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

/* TODO: Replace OpenSSL with NSS */
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/aes.h>
#include <openssl/rsa.h>
#include <openssl/engine.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/socket-util.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/random.h>
#include <pulsecore/poll.h>

#include "raop_client.h"
#include "rtsp.h"
#include "base64.h"

#define AES_CHUNKSIZE 16

#define JACK_STATUS_DISCONNECTED 0
#define JACK_STATUS_CONNECTED 1

#define JACK_TYPE_ANALOG 0
#define JACK_TYPE_DIGITAL 1

#define VOLUME_DEF -30
#define VOLUME_MIN -144
#define VOLUME_MAX 0


struct pa_raop_client {
    pa_rtsp_context *rtsp;
    pa_socket_client *sc;
    const char *host;
    char *sid;

    uint8_t jack_type;
    uint8_t jack_status;

    /* Encryption Related bits */
    AES_KEY aes;
    uint8_t aes_iv[AES_CHUNKSIZE]; /* initialization vector for aes-cbc */
    uint8_t aes_nv[AES_CHUNKSIZE]; /* next vector for aes-cbc */
    uint8_t aes_key[AES_CHUNKSIZE]; /* key for aes-cbc */

    pa_iochannel *io;
    pa_iochannel_cb_t callback;
    void* userdata;
};

static int rsa_encrypt(uint8_t *text, int len, uint8_t *res) {
    char n[] =
        "59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUtwC"
        "5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDR"
        "KSKv6kDqnw4UwPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuB"
        "OitnZ/bDzPHrTOZz0Dew0uowxf/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJ"
        "Q+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/UAaHqn9JdsBWLUEpVviYnh"
        "imNVvYFZeCXg/IdTQ+x4IRdiXNv5hEew==";
    char e[] = "AQAB";
    uint8_t modules[256];
    uint8_t exponent[8];
    int size;
    RSA *rsa;

    rsa = RSA_new();
    size = pa_base64_decode(n, modules);
    rsa->n = BN_bin2bn(modules, size, NULL);
    size = pa_base64_decode(e, exponent);
    rsa->e = BN_bin2bn(exponent, size, NULL);

    size = RSA_public_encrypt(len, text, res, rsa, RSA_PKCS1_OAEP_PADDING);
    RSA_free(rsa);
    return size;
}

static int aes_encrypt(pa_raop_client* c, uint8_t *data, int size)
{
    uint8_t *buf;
    int i=0, j;

    pa_assert(c);

    memcpy(c->aes_nv, c->aes_iv, AES_CHUNKSIZE);
    while (i+AES_CHUNKSIZE <= size) {
        buf = data + i;
        for (j=0; j<AES_CHUNKSIZE; ++j)
            buf[j] ^= c->aes_nv[j];

        AES_encrypt(buf, buf, &c->aes);
        memcpy(c->aes_nv, buf, AES_CHUNKSIZE);
        i += AES_CHUNKSIZE;
    }
    return i;
}

pa_raop_client* pa_raop_client_new(void)
{
    pa_raop_client* c = pa_xnew0(pa_raop_client, 1);
    return c;
}

void pa_raop_client_free(pa_raop_client* c)
{
    pa_assert(c);
    pa_xfree(c);
}

static int remove_char_from_string(char *str, char rc)
{
  int i=0, j=0, len;
  int num = 0;
  len = strlen(str);
  while (i<len) {
      if (str[i] == rc) {
          for (j=i; j<len; j++)
              str[j] = str[j+1];
          len--;
          num++;
      } else {
          i++;
      }
  }
  return num;
}

static void rtsp_cb(pa_rtsp_context *rtsp, pa_rtsp_state state, pa_headerlist* headers, void *userdata)
{
    pa_raop_client* c = userdata;
    pa_assert(c);
    pa_assert(rtsp);
    pa_assert(rtsp == c->rtsp);

    switch (state) {
        case STATE_CONNECT: {
            int i;
            uint8_t rsakey[512];
            char *key, *iv, *sac, *sdp;
            uint16_t rand_data;
            const char *ip;
            char *url;

            ip = pa_rtsp_localip(c->rtsp);
            /* First of all set the url properly */
            url = pa_sprintf_malloc("rtsp://%s/%s", ip, c->sid);
            pa_rtsp_set_url(c->rtsp, url);
            pa_xfree(url);

            /* Now encrypt our aes_public key to send to the device */
            i = rsa_encrypt(c->aes_key, AES_CHUNKSIZE, rsakey);
            pa_base64_encode(rsakey, i, &key);
            remove_char_from_string(key, '=');
            pa_base64_encode(c->aes_iv, AES_CHUNKSIZE, &iv);
            remove_char_from_string(iv, '=');

            pa_random(&rand_data, sizeof(rand_data));
            pa_base64_encode(&rand_data, AES_CHUNKSIZE, &sac);
            remove_char_from_string(sac, '=');
            pa_rtsp_add_header(c->rtsp, "Apple-Challenge", sac);
            sdp = pa_sprintf_malloc(
                "v=0\r\n"
                "o=iTunes %s 0 IN IP4 %s\r\n"
                "s=iTunes\r\n"
                "c=IN IP4 %s\r\n"
                "t=0 0\r\n"
                "m=audio 0 RTP/AVP 96\r\n"
                "a=rtpmap:96 AppleLossless\r\n"
                "a=fmtp:96 4096 0 16 40 10 14 2 255 0 0 44100\r\n"
                "a=rsaaeskey:%s\r\n"
                "a=aesiv:%s\r\n",
                c->sid, ip, c->host, key, iv);
            pa_rtsp_announce(c->rtsp, sdp);
            pa_xfree(key);
            pa_xfree(iv);
            pa_xfree(sac);
            pa_xfree(sdp);
            break;
        }

        case STATE_ANNOUNCE:
            pa_rtsp_remove_header(c->rtsp, "Apple-Challenge");
            pa_rtsp_setup(c->rtsp);
            break;

        case STATE_SETUP: {
            char *aj = pa_xstrdup(pa_headerlist_gets(headers, "Audio-Jack-Status"));
            if (aj) {
                char *token, *pc;
                char delimiters[] = ";";
                const char* token_state = NULL;
                c->jack_type = JACK_TYPE_ANALOG;
                c->jack_status = JACK_STATUS_DISCONNECTED;

                while ((token = pa_split(aj, delimiters, &token_state))) {
                    if ((pc = strstr(token, "="))) {
                      *pc = 0;
                      if (!strcmp(token, "type") && !strcmp(pc+1, "digital")) {
                          c->jack_type = JACK_TYPE_DIGITAL;
                      }
                    } else {
                        if (!strcmp(token,"connected"))
                            c->jack_status = JACK_STATUS_CONNECTED;
                    }
                    pa_xfree(token);
                }
                pa_xfree(aj);
                pa_rtsp_record(c->rtsp);
            } else {
                pa_log("Audio Jack Status missing");
            }
            break;
        }

        case STATE_RECORD:
            /* Connect to the actual stream ;) */
            /* if(raopcl_stream_connect(raopcld)) goto erexit; */
            break;

        case STATE_TEARDOWN:
        case STATE_SET_PARAMETER:
        case STATE_FLUSH:
            break;
    }
}

int pa_raop_client_connect(pa_raop_client* c, pa_mainloop_api *mainloop, const char* host)
{
    char *sci;
    struct {
        uint32_t a;
        uint32_t b;
        uint32_t c;
    } rand_data;

    pa_assert(c);
    pa_assert(host);

    c->host = host;
    c->rtsp = pa_rtsp_context_new("iTunes/4.6 (Macintosh; U; PPC Mac OS X 10.3)");

    /* Initialise the AES encryption system */
    pa_random_seed();
    pa_random(c->aes_iv, sizeof(c->aes_iv));
    pa_random(c->aes_key, sizeof(c->aes_key));
    memcpy(c->aes_nv, c->aes_iv, sizeof(c->aes_nv));
    AES_set_encrypt_key(c->aes_key, 128, &c->aes);

    /* Generate random instance id */
    pa_random(&rand_data, sizeof(rand_data));
    c->sid = pa_sprintf_malloc("%u", rand_data.a);
    sci = pa_sprintf_malloc("%08x%08x",rand_data.b, rand_data.c);
    pa_rtsp_add_header(c->rtsp, "Client-Instance", sci);
    pa_rtsp_set_callback(c->rtsp, rtsp_cb, c);
    return pa_rtsp_connect(c->rtsp, mainloop, host, 5000);
}

void pa_raop_client_disconnect(pa_raop_client* c)
{

}

void pa_raop_client_send_sample(pa_raop_client* c, const uint8_t* buffer, unsigned int count)
{

}
