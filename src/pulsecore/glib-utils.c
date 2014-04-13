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
#include <glib.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/glib-utils.h>

typedef struct thread_mq_source thread_mq_source;

struct thread_mq_source {
    GSource source;
    pa_asyncmsgq *q;
    GPollFD poll;
    GMainLoop *mainloop;

    pa_msgobject *object;
    pa_memchunk chunk;
    int64_t offset;
    void *data;
    int code;
};

static gboolean thread_mq_in_source_prepare(GSource *source,
                                            gint *timeout) {

    thread_mq_source *s = (thread_mq_source *) source;

    if (pa_asyncmsgq_get(s->q, &s->object, &s->code, &s->data, &s->offset, &s->chunk, 0) == 0) {
        if (!s->object && s->code == PA_MESSAGE_SHUTDOWN) {
            pa_asyncmsgq_done(s->q, 0);
            g_main_loop_quit(s->mainloop);
            return true;
        }

        /* dispatching is done from thread_mq_in_source_dispatch() */
        return true;
    }

    pa_asyncmsgq_read_before_poll(s->q);
    *timeout = -1;
    return false;
}

static gboolean thread_mq_in_source_check(GSource *source) {

    thread_mq_source *s = (thread_mq_source *) source;
    pa_asyncmsgq_read_after_poll(s->q);
    return s->poll.revents & G_IO_IN;
}

static gboolean thread_mq_in_source_dispatch(GSource *source,
                                             GSourceFunc callback,
                                             gpointer user_data) {

    thread_mq_source *s = (thread_mq_source *) source;

    /* Don't call the provided callback here - it should be unset anyway.
     * All dispatching of messages will be done through the .process_msg
     * callback of the message that is passed around.
     */

    if (s->object) {
        int ret = pa_asyncmsgq_dispatch(s->object, s->code, s->data, s->offset, &s->chunk);
        pa_asyncmsgq_done(s->q, ret);
        s->object = NULL;
    }

    return true;
}

static GSourceFuncs thread_mq_in_source_funcs = {
    .prepare = thread_mq_in_source_prepare,
    .check = thread_mq_in_source_check,
    .dispatch = thread_mq_in_source_dispatch,
};

GSource *pa_gsource_for_thread_mq(pa_thread_mq *mq, GMainLoop *mainloop) {

    thread_mq_source *mq_source;
    GSource *source;

    source = g_source_new(&thread_mq_in_source_funcs, sizeof(*mq_source));
    mq_source = (thread_mq_source *) source;

    mq_source->poll.fd = pa_asyncmsgq_read_fd(mq->inq);
    mq_source->poll.events = G_IO_IN;
    mq_source->mainloop = mainloop;
    mq_source->q = mq->inq;

    g_source_add_poll(source, &mq_source->poll);

    return source;
}
