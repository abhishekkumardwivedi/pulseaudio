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

#include <pulse/xmalloc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/modargs.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/glib-utils.h>

#include "module-gupnp-discover-symdef.h"

#define DEVICE_MODULE_NAME "module-gupnp-sink"

PA_MODULE_AUTHOR("Daniel Mack");
PA_MODULE_DESCRIPTION("UPnp/AV Service Discovery");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(true);

#define PA_UPNP_MEDIA_RENDERER_URN "urn:schemas-upnp-org:device:MediaRenderer:1"

static const char * const valid_modargs[] = {
    NULL
};

enum {
    GUPNP_DISCOVER_CONTEXT_AVAILABLE = PA_SINK_MESSAGE_MAX,
    GUPNP_DISCOVER_CONTEXT_UNAVAILABLE,
};

typedef struct pa_upnp_device pa_upnp_device;
typedef struct pa_gupnp_discover_msg pa_gupnp_discover_msg;
struct userdata;

struct pa_gupnp_discover_msg {
    pa_msgobject parent;
    struct userdata *userdata;
    GUPnPDeviceProxy *proxy;
};

PA_DEFINE_PRIVATE_CLASS(pa_gupnp_discover_msg, pa_msgobject);
#define PA_GUPNP_DISCOVER_MSG(o) (pa_gupnp_discover_msg_cast(o))

struct pa_upnp_device {
    GUPnPDeviceProxy *proxy;
    unsigned int module_index;
    PA_LLIST_FIELDS(pa_upnp_device);
};

struct userdata {
    pa_core *core;
    pa_module *module;

    GMainLoop *loop;
    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_gupnp_discover_msg *msg_from_glib;

    PA_LLIST_HEAD(pa_upnp_device, devices);
};

static void
device_proxy_available_cb(GUPnPControlPoint *cp,
                          GUPnPDeviceProxy *proxy,
                          gpointer data) {

    struct userdata *u = data;

    u->msg_from_glib->proxy = proxy;
    pa_assert_se(pa_asyncmsgq_send(u->thread_mq.outq, PA_MSGOBJECT(u->msg_from_glib),
                                   GUPNP_DISCOVER_CONTEXT_AVAILABLE, NULL, 0, NULL) == 0);
}

static void
device_proxy_unavailable_cb(GUPnPControlPoint *cp,
                            GUPnPDeviceProxy *proxy,
                            gpointer data) {

    struct userdata *u = data;

    u->msg_from_glib->proxy = proxy;
    pa_assert_se(pa_asyncmsgq_send(u->thread_mq.outq, PA_MSGOBJECT(u->msg_from_glib),
                                   GUPNP_DISCOVER_CONTEXT_UNAVAILABLE, NULL, 0, NULL) == 0);
}

static void
context_available_cb(GUPnPContextManager *context_manager,
                     GUPnPContext *context,
                     gpointer data) {

    GUPnPControlPoint *cp;
    struct userdata *u = data;

    cp = gupnp_control_point_new(context, PA_UPNP_MEDIA_RENDERER_URN);

    g_signal_connect(cp, "device-proxy-available",
                     G_CALLBACK(device_proxy_available_cb), u);
    g_signal_connect(cp, "device-proxy-unavailable",
                     G_CALLBACK(device_proxy_unavailable_cb), u);

    gssdp_resource_browser_set_active(GSSDP_RESOURCE_BROWSER(cp), true);
    gupnp_context_manager_manage_control_point(context_manager, cp);
    g_object_unref(cp);
}

static void thread_func(void *data) {

    struct userdata *u = data;
    GUPnPContextManager *context_manager;
    GMainContext *context;
    GSource *source;

    context = g_main_context_new();
    g_main_context_push_thread_default(context);
    u->loop = g_main_loop_new(context, false);

    source = pa_gsource_for_thread_mq(&u->thread_mq, u->loop);
    g_source_attach(source, context);

    context_manager = gupnp_context_manager_new(NULL, 0);
    g_signal_connect(context_manager, "context-available",
                     G_CALLBACK(context_available_cb), u);

    g_main_loop_run(u->loop);
    g_object_unref(context_manager);
}

static int process_msg_from_glib(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {

    pa_gupnp_discover_msg *msg_from_glib = PA_GUPNP_DISCOVER_MSG(o);
    GUPnPDeviceProxy *proxy = msg_from_glib->proxy;
    GUPnPDeviceInfo *info = GUPNP_DEVICE_INFO(proxy);
    struct userdata *u = msg_from_glib->userdata;

    switch (code) {
        case GUPNP_DISCOVER_CONTEXT_AVAILABLE: {
            pa_module *mod;
            char *args;

            args = pa_sprintf_malloc("udn=%s", gupnp_device_info_get_udn(info));
            pa_log_debug("Loading %s with arguments '%s'", DEVICE_MODULE_NAME, args);
            mod = pa_module_load(u->core, DEVICE_MODULE_NAME, args);

            if (mod) {
                struct pa_upnp_device *dev;

                dev = pa_xnew0(pa_upnp_device, 1);
                dev->proxy = proxy;
                dev->module_index = mod->index;

                PA_LLIST_INIT(pa_upnp_device, dev);
                PA_LLIST_PREPEND(pa_upnp_device, u->devices, dev);
            } else {
                pa_log("Unable to load module %s with arguments '%s'.\n", DEVICE_MODULE_NAME, args);
            }

            pa_xfree(args);
            break;
        }

        case GUPNP_DISCOVER_CONTEXT_UNAVAILABLE: {
            struct pa_upnp_device *dev, *tmp;
            PA_LLIST_FOREACH_SAFE(dev, tmp, u->devices)
                if (dev->proxy == proxy) {
                    pa_log_debug("object %s has been removed (module index %d)",
                                 gupnp_device_info_get_udn(info),
                                 dev->module_index);
                    pa_module_unload_request_by_index(u->core, dev->module_index, true);
                    PA_LLIST_REMOVE(pa_upnp_device, u->devices, dev);
                    g_object_unref(proxy);
                    pa_xfree(dev);
                }
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
        pa__done(m);
        return -1;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    PA_LLIST_HEAD_INIT(pa_upnp_device, u->devices);

    u->msg_from_glib = pa_msgobject_new(pa_gupnp_discover_msg);
    u->msg_from_glib->parent.process_msg = process_msg_from_glib;
    u->msg_from_glib->userdata = u;

    /* Do not install an rtpoll for this mq as it will be handled by the GMainLoop */
    pa_thread_mq_init(&u->thread_mq, m->core->mainloop, NULL);

    u->thread = pa_thread_new("gupnp-discover", thread_func, u);
    pa_assert(u->thread);

    pa_modargs_free(ma);

    return 0;
}

void pa__done(pa_module*m) {
    struct pa_upnp_device *dev, *tmp;
    struct userdata *u;
    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
        pa_thread_mq_done(&u->thread_mq);
    }

    PA_LLIST_FOREACH_SAFE(dev, tmp, u->devices) {
        pa_module_unload_request_by_index(u->core, dev->module_index, true);
        g_object_unref(dev->proxy);
        pa_xfree(dev);
    }

    pa_xfree(u->msg_from_glib);
    pa_xfree(u);
}
