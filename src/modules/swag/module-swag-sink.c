/***
  This file is part of PulseAudio.

  Copyright 2004-2008 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>

#include <pulse/rtclock.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/xmalloc.h>

#include <pulsecore/i18n.h>
#include <pulsecore/macro.h>
#include <pulsecore/sink.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>

#include "net.h"

PA_MODULE_AUTHOR("rca");
PA_MODULE_DESCRIPTION(_("swag sink"));
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "sink_name=<name of sink> "
        "format=<sample format> "
        "rate=<sample rate> "
        "channels=<number of channels> "
        "channel_map=<channel map>"
        "iface=<wireless interface>"
        );

#define DEFAULT_SINK_NAME "swag"
#define DEFAULT_IFACE "wlan0"
#define MAX_FRAME_SIZE 1480

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_usec_t block_usec;
    size_t buffer_size;
    pa_usec_t processed_ts;
    pa_memchunk memchunk;
    const char *iface;
    int retries;
    struct wicast wistream;
};

static const char* const valid_modargs[] = {
    "sink_name",
    "format",
    "rate",
    "channels",
    "channel_map",
    "iface",
    NULL
};

static int sink_process_msg(
        pa_msgobject *o,
        int code,
        void *data,
        int64_t offset,
        pa_memchunk *chunk) {

    struct userdata *u = PA_SINK(o)->userdata;

    switch (code) {
        case PA_SINK_MESSAGE_GET_LATENCY: { // We can do this lock free, we should override get_latency() instead
            pa_usec_t now;

            now = pa_rtclock_now();
            *((int64_t*) data) = (int64_t)u->processed_ts - (int64_t)now;

            return 0;
        }
    }

    return pa_sink_process_msg(o, code, data, offset, chunk);
}

/* Called from the IO thread. */
static int sink_set_state_in_io_thread_cb(pa_sink *s, pa_sink_state_t new_state, pa_suspend_cause_t new_suspend_cause) {
    struct userdata *u;

    pa_assert(s);
    pa_assert_se(u = s->userdata);

    if (s->thread_info.state == PA_SINK_SUSPENDED || s->thread_info.state == PA_SINK_INIT) {
        if (PA_SINK_IS_OPENED(new_state))
            u->processed_ts = pa_rtclock_now();
    }

    return 0;
}

static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;
    size_t nbytes;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    u->block_usec = pa_sink_get_requested_latency_within_thread(s);

    if (u->block_usec == (pa_usec_t) -1)
        u->block_usec = s->thread_info.max_latency;

    nbytes = pa_usec_to_bytes(u->block_usec, &s->sample_spec);

    pa_sink_set_max_rewind_within_thread(s, 0);

    pa_sink_set_max_request_within_thread(s, nbytes);
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);
    pa_log_debug("Thread starting up");

    /* is this really needed ?
    if (u->core->realtime_scheduling)
        pa_thread_make_realtime(u->core->realtime_priority);
    */

    pa_thread_mq_install(&u->thread_mq);
    u->processed_ts = pa_rtclock_now();
    u->retries = 0;

    for (;;) {
        // Send audio stream in 1400 byte chunks (~one frame every 8ms at 44.1x16x2)
        pa_usec_t now = pa_rtclock_now();
        int ret;

        // TODO: maybe deal with rewind

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            if (u->processed_ts <= now) {
                char * p;
                pa_memchunk chunk;
                pa_sink_render(u->sink, u->buffer_size, &chunk); // pa_sink_render_full always return a buffer_size length chunk
                pa_assert(chunk.length > 0);
                p = pa_memblock_acquire(chunk.memblock);
                wicast_send(&u->wistream, (char*) p + chunk.index, chunk.length, u->processed_ts, 0);
                pa_memblock_release(chunk.memblock);
                pa_memblock_unref(chunk.memblock);
                u->processed_ts += pa_bytes_to_usec(chunk.length, &u->sink->sample_spec);
                u->retries = 1;
            }

            pa_rtpoll_set_timer_relative(u->rtpoll, u->block_usec);
        } else {
            pa_rtpoll_set_timer_disabled(u->rtpoll);
        }

        /* Hmm, nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;
    }

fail:
    /* If this was no regular exit from the loop we have to continue
     * processing messages until we received PA_MESSAGE_SHUTDOWN */
    pa_asyncmsgq_post(u->thread_mq.outq, PA_MSGOBJECT(u->core), PA_CORE_MESSAGE_UNLOAD_MODULE, u->module, 0, NULL, NULL);
    pa_asyncmsgq_wait_for(u->thread_mq.inq, PA_MESSAGE_SHUTDOWN);

finish:
    pa_log_debug("Thread shutting down");
}

int pa__init(pa_module*m) {
    struct userdata *u = NULL;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma = NULL;
    pa_sink_new_data data;
    int ret;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto fail;
    }

    ss = m->core->default_sample_spec;
    map = m->core->default_channel_map;
    if (pa_modargs_get_sample_spec_and_channel_map(ma, &ss, &map, PA_CHANNEL_MAP_DEFAULT) < 0) {
        pa_log("Invalid sample format specification or channel map");
        goto fail;
    }

    // setup userdata
    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->rtpoll = pa_rtpoll_new();
    if (pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll) < 0) {
        pa_log("pa_thread_mq_init() failed.");
        goto fail;
    }

    // setup sink data
    pa_sink_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_sink_new_data_set_name(&data, pa_modargs_get_value(ma, "sink_name", DEFAULT_SINK_NAME));
    pa_sink_new_data_set_sample_spec(&data, &ss);
    pa_sink_new_data_set_channel_map(&data, &map);
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, _("Swag Output"));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract"); // TODO: what is this ?

    if (pa_modargs_get_proplist(ma, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&data);
        goto fail;
    }

    u->iface = pa_modargs_get_value(ma, "iface", DEFAULT_IFACE);
    ret = wicast_open(&u->wistream, u->iface);
    if (ret < 0) {
        pa_log("Failed to open interface %s, error : %d\n", u->iface, ret);
        goto fail;
    }

    u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink object.");
        goto fail;
    }

    // We can send 1400 bytes frames max
    u->buffer_size = pa_frame_align(MAX_FRAME_SIZE, &u->sink->sample_spec);
    u->block_usec = pa_bytes_to_usec(u->buffer_size, &u->sink->sample_spec);
    pa_log("Buffer size : %zd, corresponding timing : %luus at %s %uch %dHz",
            u->buffer_size, u->block_usec,
            pa_sample_format_to_string(u->sink->sample_spec.format),
            u->sink->sample_spec.channels,
            u->sink->sample_spec.rate);
    pa_sink_set_latency_range(u->sink, 0, u->block_usec);
    pa_sink_set_max_rewind(u->sink, 0); // disable rewind on this sink
    pa_sink_set_max_request(u->sink, u->buffer_size);

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
    u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);

    if (!(u->thread = pa_thread_new("swag-sink", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_sink_put(u->sink);
    pa_modargs_free(ma);
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);

    return -1;
}

int pa__get_n_used(pa_module *m) {
    struct userdata *u;

    pa_assert(m);
    pa_assert_se(u = m->userdata);

    return pa_sink_linked_by(u->sink);
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink)
        pa_sink_unlink(u->sink);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    if (u->memchunk.memblock) {
        pa_memblock_unref(u->memchunk.memblock);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    pa_assert_se(wicast_close(&u->wistream) == 0);

    pa_xfree(u);
}
