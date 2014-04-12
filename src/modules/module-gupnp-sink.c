/***
  This file is part of PulseAudio.

  Copyright 2014 - Daniel Mack

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libgupnp/gupnp.h>
#include <libgupnp-av/gupnp-av.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/glib-utils.h>

#include "module-gupnp-sink-symdef.h"

PA_MODULE_AUTHOR("Daniel Mack");
PA_MODULE_DESCRIPTION("UPnp/AV sink");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);

#define PA_UPNP_RENDER_CONTROL_URN "urn:schemas-upnp-org:service:RenderingControl:1"

static const char * const valid_modargs[] = {
    "udn",
};

/* Messages from GLib context */
enum {
    GUPNP_SINK_CONTEXT_AVAILABLE = PA_SINK_MESSAGE_MAX,
    GUPNP_SINK_VOLUME_CHANGED,
    GUPNP_SINK_MUTE_CHANGED,
};

/* Messages to GLib context */
enum {
    GUPNP_SINK_SET_VOLUME,
    GUPNP_SINK_GET_VOLUME,
    GUPNP_SINK_SET_MUTED,
    GUPNP_SINK_GET_MUTED,
};

typedef struct pa_upnp_device pa_upnp_device;
typedef struct pa_gupnp_sink_msg pa_gupnp_sink_msg;
struct userdata;

struct pa_gupnp_sink_msg {
    pa_msgobject parent;
    struct userdata *userdata;
};

PA_DEFINE_PRIVATE_CLASS(pa_gupnp_sink_msg, pa_msgobject);
#define PA_GUPNP_SINK_MSG(o) (pa_gupnp_sink_msg_cast(o))

struct userdata {
    pa_core *core;
    pa_sink *sink;
    pa_module *module;
    const char *udn;
    pa_gupnp_sink_msg *msg_to_glib;
    pa_gupnp_sink_msg *msg_from_glib;
    GUPnPDeviceInfo *device_info;
    GUPnPServiceInfo *render_control_service_info;

    GMainLoop *loop;
    pa_thread *manage_thread;
    pa_thread *sink_thread;
    pa_thread_mq manage_thread_mq;
    pa_thread_mq sink_thread_mq;
    pa_rtpoll *manage_rtpoll;
    pa_rtpoll *sink_rtpoll;
};

struct pa_gupnp_sink_init_data {
    pa_volume_t volume;
    bool muted;
};

static unsigned int volume_to_percent(unsigned int v)
{
    return (v * 100) / 65536;
}

static unsigned int volume_from_percent(unsigned int v)
{
    return (v * 65536) / 100;
}

static void
last_change_cb(GUPnPServiceProxy *proxy,
               const char        *variable,
               GValue            *value,
               gpointer           userdata)
{
    GUPnPLastChangeParser *parser = gupnp_last_change_parser_new();
    struct userdata *u = userdata;
    guint volume = G_MAXUINT;
    gboolean muted = -1;
    GError *error = NULL;
    bool success;

    success = gupnp_last_change_parser_parse_last_change(parser, 0, g_value_get_string(value), &error,
                                                         "Volume",  G_TYPE_UINT,    &volume,
                                                         "Mute",    G_TYPE_BOOLEAN, &muted,
                                                         NULL);
    if (!success)
        return;

    if (volume != G_MAXUINT) {
        pa_volume_t v = volume_from_percent(volume);
        pa_assert_se(pa_asyncmsgq_send(u->manage_thread_mq.outq, PA_MSGOBJECT(u->msg_from_glib),
                                       GUPNP_SINK_VOLUME_CHANGED, &v, 0, NULL) == 0);
    }

    if (muted != -1) {
        pa_assert_se(pa_asyncmsgq_send(u->manage_thread_mq.outq, PA_MSGOBJECT(u->msg_from_glib),
                                       GUPNP_SINK_MUTE_CHANGED, &muted, 0, NULL) == 0);
    }

    g_object_unref(parser);
}

static void
device_proxy_available_cb(GUPnPControlPoint *cp,
                          GUPnPDeviceProxy *proxy,
                          gpointer data) {

    struct userdata *u = data;
    struct pa_gupnp_sink_init_data init_data;
    GError *error = NULL;
    GUPnPServiceProxy *service_proxy;

    u->device_info = GUPNP_DEVICE_INFO(proxy);

    u->render_control_service_info =
        gupnp_device_info_get_service(u->device_info, PA_UPNP_RENDER_CONTROL_URN);

    if (!u->render_control_service_info) {
        pa_log("Device with udn %s does not have a rendering control service\n", u->udn);
        return;
    }

    service_proxy = GUPNP_SERVICE_PROXY(u->render_control_service_info);

    gupnp_service_proxy_send_action(service_proxy,
                                    "GetVolume", &error,
                                    NULL,
                                    "CurrentVolume", G_TYPE_UINT, &init_data.volume,
                                    NULL);
    init_data.volume = volume_from_percent(init_data.volume);

    gupnp_service_proxy_send_action(service_proxy,
                                    "GetMute", &error,
                                    NULL,
                                    "CurrentMute", G_TYPE_UINT, &init_data.muted,
                                    NULL);

    gupnp_service_proxy_set_subscribed(service_proxy, true);
    gupnp_service_proxy_add_notify(service_proxy,
                                   "LastChange", G_TYPE_STRING,
                                   last_change_cb, u);

    pa_assert_se(pa_asyncmsgq_send(u->manage_thread_mq.outq, PA_MSGOBJECT(u->msg_from_glib),
                                   GUPNP_SINK_CONTEXT_AVAILABLE, &init_data, 0, NULL) == 0);
}

static void
context_available_cb(GUPnPContextManager *context_manager,
                     GUPnPContext *context,
                     gpointer data) {

    GUPnPControlPoint *cp;
    struct userdata *u = data;

    cp = gupnp_control_point_new(context, u->udn);

    g_signal_connect(cp, "device-proxy-available",
                     G_CALLBACK(device_proxy_available_cb), u);
    gssdp_resource_browser_set_active(GSSDP_RESOURCE_BROWSER(cp), true);
    gupnp_context_manager_manage_control_point(context_manager, cp);
    g_object_unref(cp);
}

static int process_msg_to_glib(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    pa_gupnp_sink_msg *msg_from_glib = PA_GUPNP_SINK_MSG(o);
    struct userdata *u = msg_from_glib->userdata;
    GError *error = NULL;

    switch (code) {
        case GUPNP_SINK_SET_VOLUME: {
            const pa_cvolume *cvol = data;
            pa_volume_t v = pa_cvolume_avg(cvol);

            v = volume_to_percent(v);
            gupnp_service_proxy_send_action(GUPNP_SERVICE_PROXY(u->render_control_service_info),
                                            "SetVolume", &error,
                                            "DesiredVolume", G_TYPE_UINT, v,
                                            NULL,
                                            NULL);
            break;
        }

        case GUPNP_SINK_GET_VOLUME: {
            pa_cvolume *cvol = data;
            pa_volume_t v;

            gupnp_service_proxy_send_action(GUPNP_SERVICE_PROXY(u->render_control_service_info),
                                            "GetVolume", &error,
                                            NULL,
                                            "CurrentVolume", G_TYPE_UINT, &v,
                                            NULL);
            pa_cvolume_set(cvol, 2, volume_from_percent(v));
            break;
        }

        case GUPNP_SINK_SET_MUTED: {
            const unsigned int *muted = data;
            gupnp_service_proxy_send_action(GUPNP_SERVICE_PROXY(u->render_control_service_info),
                                            "SetMute", &error,
                                            "DesiredMute", G_TYPE_UINT, *muted,
                                            NULL,
                                            NULL);
            break;
        }

        case GUPNP_SINK_GET_MUTED: {
            unsigned int *muted = data;
            gupnp_service_proxy_send_action(GUPNP_SERVICE_PROXY(u->render_control_service_info),
                                            "SetMute", &error,
                                            NULL,
                                            "CurrentMute", G_TYPE_UINT, muted,
                                            NULL);
            break;
        }

        default:
            pa_assert_not_reached();
    }

    return 0;
}

static void manage_thread_func(void *data) {

    struct userdata *u = data;
    GUPnPContextManager *context_manager;
    GMainContext *context;
    GSource *source;

    context = g_main_context_new();
    g_main_context_push_thread_default(context);
    u->loop = g_main_loop_new(context, false);

    source = pa_gsource_for_thread_mq(&u->manage_thread_mq, u->loop);
    g_source_attach(source, context);

    context_manager = gupnp_context_manager_new(NULL, 0);
    g_signal_connect(context_manager, "context-available",
                     G_CALLBACK(context_available_cb), u);

    g_main_loop_run(u->loop);
}

static void sink_thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);
    pa_assert(u->module);
    pa_assert(u->module->core);

    pa_log_debug("Thread starting up");

    if (u->module->core->realtime_scheduling)
        pa_make_realtime(u->module->core->realtime_priority);

    pa_thread_mq_install(&u->sink_thread_mq);

    for (;;) {
        int ret;

//        if (u->sink && PA_UNLIKELY(u->sink->thread_info.rewind_requested))
//            pa_sink_process_rewind(u->sink, 0);

        ret = pa_rtpoll_run(u->sink_rtpoll, true);

        if (ret < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->sink_thread_mq.outq, PA_MSGOBJECT(u->module->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->sink_thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

static int gupnp_sink_set_state(pa_sink *s, pa_sink_state_t state) {
    struct userdata *u = s->userdata;

    switch (state) {
        case PA_SINK_SUSPENDED:
        case PA_SINK_IDLE:
            break;

        case PA_SINK_RUNNING:
            break;

        case PA_SINK_UNLINKED:
        case PA_SINK_INIT:
        case PA_SINK_INVALID_STATE:
            break;
    }

    return 0;
}

static int gupnp_sink_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    switch (code) {
        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            *((pa_usec_t *) data) = 1000;
            return 0;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

static void sink_set_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;

    if (!PA_SINK_IS_LINKED(pa_sink_get_state(s)))
        return;

    pa_assert_se(pa_asyncmsgq_send(u->manage_thread_mq.inq, PA_MSGOBJECT(u->msg_to_glib),
                                   GUPNP_SINK_SET_VOLUME, &s->real_volume, 0, NULL) == 0);
}

static void sink_get_volume_cb(pa_sink *s) {
    struct userdata *u = s->userdata;

    if (!PA_SINK_IS_LINKED(pa_sink_get_state(s)))
        return;

    pa_assert_se(pa_asyncmsgq_send(u->manage_thread_mq.inq, PA_MSGOBJECT(u->msg_to_glib),
                                   GUPNP_SINK_GET_VOLUME, &s->real_volume, 0, NULL) == 0);
}

static void sink_set_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    unsigned int muted;

    if (!PA_SINK_IS_LINKED(pa_sink_get_state(s)))
        return;

    muted = s->muted;
    pa_assert_se(pa_asyncmsgq_send(u->manage_thread_mq.inq, PA_MSGOBJECT(u->msg_to_glib),
                                   GUPNP_SINK_SET_MUTED, &muted, 0, NULL) == 0);
}

static void sink_get_mute_cb(pa_sink *s) {
    struct userdata *u = s->userdata;
    unsigned int muted;

    if (!PA_SINK_IS_LINKED(pa_sink_get_state(s)))
        return;

    pa_assert_se(pa_asyncmsgq_send(u->manage_thread_mq.inq, PA_MSGOBJECT(u->msg_to_glib),
                                   GUPNP_SINK_GET_MUTED, &muted, 0, NULL) == 0);
    s->muted = muted;
}

static int process_msg_from_glib(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    pa_gupnp_sink_msg *msg_from_glib = PA_GUPNP_SINK_MSG(o);
    struct userdata *u = msg_from_glib->userdata;

    switch (code) {
        case GUPNP_SINK_CONTEXT_AVAILABLE: {
            struct pa_gupnp_sink_init_data *init_data = data;
            pa_sink_new_data sdata;
            pa_module *m = u->module;
            pa_cvolume volume;

            pa_sink_new_data_init(&sdata);
            sdata.driver = __FILE__;
            sdata.module = m;
            pa_sink_new_data_set_name(&sdata, gupnp_device_info_get_friendly_name(u->device_info));
            pa_proplist_sets(sdata.proplist, PA_PROP_DEVICE_STRING, u->udn);
            pa_proplist_sets(sdata.proplist, PA_PROP_DEVICE_DESCRIPTION,
                             gupnp_device_info_get_friendly_name(u->device_info));
            pa_proplist_sets(sdata.proplist, PA_PROP_DEVICE_FORM_FACTOR, "speaker");
            pa_sink_new_data_set_sample_spec(&sdata, &m->core->default_sample_spec);
            pa_sink_new_data_set_channel_map(&sdata, &m->core->default_channel_map);

            pa_cvolume_set(&volume, 2, init_data->volume);
            pa_sink_new_data_set_volume(&sdata, &volume);
            pa_sink_new_data_set_muted(&sdata, init_data->muted);

            u->sink = pa_sink_new(m->core, &sdata, PA_SINK_LATENCY);
            pa_sink_new_data_done(&sdata);
            pa_assert(u->sink);

            u->sink->parent.process_msg = gupnp_sink_process_msg;
            u->sink->userdata = u;
            u->sink->set_state = gupnp_sink_set_state;

            pa_sink_set_asyncmsgq(u->sink, u->sink_thread_mq.inq);
            pa_sink_set_rtpoll(u->sink, u->sink_rtpoll);

            pa_sink_set_set_volume_callback(u->sink, sink_set_volume_cb);
            pa_sink_set_get_volume_callback(u->sink, sink_get_volume_cb);
            pa_sink_set_set_mute_callback(u->sink, sink_set_mute_cb);
            pa_sink_set_get_mute_callback(u->sink, sink_get_mute_cb);

#if 0
            pa_sink_set_max_request(u->sink, pa_pipe_buf(u->fd));
            pa_sink_set_fixed_latency(u->sink, pa_bytes_to_usec(pa_pipe_buf(u->fd), &u->sink->sample_spec));
#endif
            pa_sink_put(u->sink);

            break;
        }

        case GUPNP_SINK_VOLUME_CHANGED: {
            pa_volume_t *v = data;
            pa_cvolume volume;

            pa_cvolume_set(&volume, 2, *v);
            pa_sink_volume_changed(u->sink, &volume);
            break;
        }

        case GUPNP_SINK_MUTE_CHANGED: {
            unsigned int *muted = data;

            pa_sink_mute_changed(u->sink, *muted);
            break;
        }

        default:
            pa_assert_not_reached();
    }

    return 0;
}

int pa__init(pa_module *m) {

    struct userdata *u;
    pa_modargs *ma = NULL;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;

    u->msg_to_glib = pa_msgobject_new(pa_gupnp_sink_msg);
    u->msg_to_glib->parent.process_msg = process_msg_to_glib;
    u->msg_to_glib->userdata = u;

    u->msg_from_glib = pa_msgobject_new(pa_gupnp_sink_msg);
    u->msg_from_glib->parent.process_msg = process_msg_from_glib;
    u->msg_from_glib->userdata = u;

    /* Do not install an rtpoll for this mq as it will be handled by the GMainLoop */
    pa_thread_mq_init(&u->manage_thread_mq, m->core->mainloop, NULL);

    u->sink_rtpoll = pa_rtpoll_new();
    pa_thread_mq_init(&u->sink_thread_mq, m->core->mainloop, u->sink_rtpoll);

    u->udn = pa_xstrdup(pa_modargs_get_value(ma, "udn", NULL));

    if (!u->udn) {
        pa_log("Failed to parse udn argument.");
        goto fail;
    }

    u->manage_thread = pa_thread_new("gupnp-sink-manage", manage_thread_func, u);
    pa_assert(u->manage_thread);

    u->sink_thread = pa_thread_new("gupnp-sink", sink_thread_func, u);
    pa_assert(u->sink_thread);

    pa_modargs_free(ma);

    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata*u;
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->manage_thread) {
        pa_asyncmsgq_send(u->manage_thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->manage_thread);
        pa_thread_mq_done(&u->manage_thread_mq);
    }

    if (u->sink_thread) {
        pa_asyncmsgq_send(u->sink_thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->sink_thread);
        pa_thread_mq_done(&u->sink_thread_mq);
    }

    pa_rtpoll_free(u->sink_rtpoll);
    pa_xfree(u->msg_from_glib);
    pa_xfree(u->msg_to_glib);
    pa_xfree(u);
}
