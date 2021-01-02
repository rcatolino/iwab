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

#include <pulsecore/core-error.h>
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
PA_MODULE_DESCRIPTION(_("iwab sink"));
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

#define DEFAULT_SINK_NAME "iwabsink"
#define DEFAULT_IFACE "mon0"
#define MAX_FRAME_SIZE 1400

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink *sink;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    pa_usec_t block_usec;
    pa_usec_t stream_ts_abs; // time for the next packet render
    pa_usec_t stream_resend_abs;
    const char *iface;
    int retries;
    struct iwab istream;
    pa_memchunk chunk;
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
            pa_usec_t now = pa_rtclock_now();
            pa_usec_t latency = u->stream_ts_abs - now;
            *((int64_t*) data) = (int64_t) latency;
            pa_log("Get latency : %ldus", latency);
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
            pa_log("Sink is opened");
            u->stream_ts_abs = pa_rtclock_now();
    } else if (PA_SINK_IS_OPENED(s->thread_info.state)) {
        if (new_state == PA_SINK_SUSPENDED) {
            pa_log("Sink is suspended");
        }
    }

    return 0;
}

static void sink_update_requested_latency_cb(pa_sink *s) {
    struct userdata *u;
    size_t nbytes;

    pa_sink_assert_ref(s);
    pa_assert_se(u = s->userdata);

    u->block_usec = pa_sink_get_requested_latency_within_thread(s);
    pa_log("Update requested latency to %ld", u->block_usec); // TODO: this is wrong, block_usec is used to determine the frame size

    if (u->block_usec == -1) {
        nbytes = pa_frame_align(MAX_FRAME_SIZE, &u->sink->sample_spec);
        u->block_usec = pa_bytes_to_usec(nbytes, &u->sink->sample_spec);
        pa_log("Requested latency is invalid, using %lu instead", u->block_usec);
    } else {
        nbytes = pa_usec_to_bytes(u->block_usec, &u->sink->sample_spec);
    }

    pa_log("Corresponding buffer size : %lu", nbytes);
    pa_sink_set_max_rewind_within_thread(s, 0);
    pa_sink_set_max_request_within_thread(s, nbytes);
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);
    pa_log_debug("Thread starting up");

    pa_thread_mq_install(&u->thread_mq);
    u->stream_ts_abs = pa_rtclock_now();
    u->retries = 0;

    for (;;) {
        pa_usec_t now = 0;
        int ret;
        char * p;

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state))
            now = pa_rtclock_now();

        if (PA_UNLIKELY(u->sink->thread_info.rewind_requested))
            pa_sink_process_rewind(u->sink, 0);

        if (PA_SINK_IS_OPENED(u->sink->thread_info.state)) {
            if (now >= u->stream_ts_abs) { // is it time to render the next chunk already ?
                pa_usec_t chunk_time = 0;
                pa_sink_render(u->sink, u->sink->thread_info.max_request, &u->chunk);
                u->retries = 0;
                pa_assert(u->chunk.length > 0);
                /*
                pa_log("stream_ts: %lu, now : %lu, retries : %d, rendering & sending.",
                        u->stream_ts_abs, now, u->retries);
                */

                p = pa_memblock_acquire(u->chunk.memblock);
                ret = iwab_send(&u->istream, (char*) p + u->chunk.index, u->chunk.length, u->stream_ts_abs, u->retries);
                if (ret < 0) {
                    pa_log("Error %d sending %zu byte buffer : %s", ret, u->chunk.length, pa_cstrerror(errno));
                    goto fail;
                }

                pa_memblock_release(u->chunk.memblock);
                u->retries += 1;
                chunk_time = pa_bytes_to_usec(u->chunk.length, &u->sink->sample_spec);
                u->stream_resend_abs = u->stream_ts_abs + chunk_time / 2;
                u->stream_ts_abs += chunk_time;
                pa_rtpoll_set_timer_absolute(u->rtpoll, u->stream_resend_abs);
            } else if (now >= u->stream_resend_abs && u->retries <= 1) {
                /*
                pa_log("stream_resend_ts: %lu, now : %lu, retries : %d, rendering & sending.",
                        u->stream_resend_abs, now, u->retries);
                */
                p = pa_memblock_acquire(u->chunk.memblock);
                ret = iwab_send(&u->istream, (char*) p + u->chunk.index, u->chunk.length, u->istream.wi_h.iw_h.timestamp, u->retries);
                if (ret < 0) {
                    pa_log("Error resending %zu byte buffer at %p", u->chunk.length, (char*) p + u->chunk.index);
                    goto fail;
                }

                pa_memblock_release(u->chunk.memblock);
                pa_memblock_unref(u->chunk.memblock);
                u->retries += 1;
                pa_rtpoll_set_timer_absolute(u->rtpoll, u->stream_ts_abs);
            } else {
                pa_rtpoll_set_timer_absolute(u->rtpoll, u->stream_ts_abs);
            }
        } else {
            pa_rtpoll_set_timer_disabled(u->rtpoll);
            pa_log("rtpoll set timer disabled ok");
        }

        /* Hmm, nothing to do. Let's sleep */
        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0) {
            pa_log("rtpoll fail : %d", ret);
            goto fail;
        }

        //pa_log("rtpoll run ok");

        if (ret == 0) {
            pa_log("rtpoll 0, finishing thread: %d", ret);
            goto finish;
        }
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
    size_t buffer_size = 0;

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
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, _("iwab output"));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract"); // TODO: what is this ?

    if (pa_modargs_get_proplist(ma, "sink_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_sink_new_data_done(&data);
        goto fail;
    }

    u->iface = pa_modargs_get_value(ma, "iface", DEFAULT_IFACE);
    if (iwab_open(&u->istream, u->iface)) {
        pa_log("Failed to open interface %s, error : %s\n", u->iface, pa_cstrerror(errno));
        goto fail;
    }

    u->sink = pa_sink_new(m->core, &data, PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY);
    pa_sink_new_data_done(&data);

    if (!u->sink) {
        pa_log("Failed to create sink object.");
        goto fail;
    }

    // We can send 1400 bytes frames max
    buffer_size = pa_frame_align(MAX_FRAME_SIZE, &u->sink->sample_spec);
    u->block_usec = pa_bytes_to_usec(buffer_size, &u->sink->sample_spec);
    pa_log("Buffer size : %zd, corresponding timing : %luus at %s %uch %dHz",
            buffer_size, u->block_usec,
            pa_sample_format_to_string(u->sink->sample_spec.format),
            u->sink->sample_spec.channels,
            u->sink->sample_spec.rate);
    pa_sink_set_latency_range(u->sink, 0, u->block_usec);
    pa_sink_set_max_rewind(u->sink, 0); // disable rewind on this sink
    pa_sink_set_max_request(u->sink, buffer_size);

    u->sink->parent.process_msg = sink_process_msg;
    u->sink->set_state_in_io_thread = sink_set_state_in_io_thread_cb;
    u->sink->update_requested_latency = sink_update_requested_latency_cb;
    u->sink->userdata = u;

    pa_sink_set_asyncmsgq(u->sink, u->thread_mq.inq);
    pa_sink_set_rtpoll(u->sink, u->rtpoll);
    pa_log("Sink rtpoll set!");

    if (!(u->thread = pa_thread_new("iwab-sink", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_log("RT Thread created !");
    pa_sink_put(u->sink);
    pa_log("Sink setup !");
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

    pa_thread_mq_done(&u->thread_mq);

    if (u->sink)
        pa_sink_unref(u->sink);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    pa_assert_se(iwab_close(&u->istream) == 0);

    pa_xfree(u);
}
