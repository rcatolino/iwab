
/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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

#include <errno.h>

#include <pulse/rtclock.h>
#include <pulsecore/core-error.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/memblockq.h>
#include <pulsecore/modargs.h>
#include <pulsecore/macro.h>
#include <pulsecore/namereg.h>
#include <pulsecore/poll.h>

#include "net.h"

PA_MODULE_AUTHOR("");
PA_MODULE_DESCRIPTION("input sound from a wireless source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "sink=<name of the sink> "
        "iface=<wireless interface> "
);

#define DEFAULT_SOURCE_NAME "iwabsrc"
#define DEFAULT_IFACE "mon0"
#define MAX_FRAME_SIZE 1600
#define STAT_PERIOD 10*1000*1000 //10s

static const char* const valid_modargs[] = {
    "sink",
    "iface",
    NULL
};

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink_input *sink_input;
    const char *iface;
    pa_memblockq *queue;
    bool first_packet;
    pa_rtpoll_item *rtpoll_item;
    uint32_t seqnb;
    pa_usec_t last_pb_ts;
    struct iwab istream;
    int retries;
    pa_sample_spec ss;
    pa_usec_t stat_time;
    struct {
        pa_usec_t lost;
        pa_usec_t overrun;
        pa_usec_t underrun;
        uint32_t queue_nblocks;
        uint32_t count;
    } stats;
};

/* Called from I/O thread context */
static int sink_input_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK_INPUT(o)->userdata;
    pa_usec_t latency = 0;

    switch (code) {
        // TODO: add latency from sink
        case PA_SINK_INPUT_MESSAGE_GET_LATENCY:
            latency = pa_bytes_to_usec(pa_memblockq_get_length(u->queue),
                    &u->sink_input->sample_spec);
            *((pa_usec_t*) data) = latency;
            pa_log_debug("Sink input get latenyc, returning : %" PRIu64, latency);
            break;
        case PA_SINK_INPUT_MESSAGE_SET_STATE:
            pa_log_debug("Sink input state changed : %d", u->sink_input->thread_info.state);
            break;
    }

    return pa_sink_input_process_msg(o, code, data, offset, chunk);
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata* u;
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_memblockq_rewind(u->queue, nbytes);
}

// according to sink-input.h it is better to ignore the `length` argument if we already
// have the data
static int sink_input_pop_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    struct userdata* u;
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (pa_memblockq_peek(u->queue, chunk) < 0) {
        u->stats.underrun += pa_bytes_to_usec(length, &u->ss);
        pa_log("Warning, buffer underrun : %zd bytes requested but queue empty.",
                length);
        if (u->stats.underrun > 500000 && u->sink_input->thread_info.state != PA_SINK_INPUT_CORKED) {
            u->stats.underrun = 0;
            pa_usec_t sink_delay = pa_sink_get_latency_within_thread(u->sink_input->sink, false);
            pa_log("Lots of underrun, corking sink input. sink latency : %" PRIu64, sink_delay);
            pa_sink_input_set_state_within_thread(u->sink_input, PA_SINK_INPUT_CORKED);
        }

        return -1;
    }

    pa_memblockq_drop(u->queue, chunk->length);
    return 0;
}

static void sink_input_kill_cb(pa_sink_input *i) {
    pa_sink_input_assert_ref(i);

    pa_sink_input_unlink(i);
    pa_sink_input_unref(i);
}

/* Called from IO context */
static void sink_input_suspend_within_thread(pa_sink_input* i, bool b) {
    struct userdata *u;
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (b) {
        pa_memblockq_flush_read(u->queue);
        pa_log("sink input suspended");
    } else {
        pa_log("sink input resumed");
        u->last_pb_ts = 0;
        u->seqnb = 0;
        memset(&u->stats, 0, sizeof(u->stats));
    }
}

static void update_stats(struct userdata* u, pa_usec_t now) {
    pa_assert(now > u->stat_time);
    pa_usec_t timediff = (now - u->stat_time) / 1000; // ms per period
    u->stat_time = now;

    pa_proplist_setf(u->sink_input->proplist, "iwab.lost", "%" PRIu64 "ms/s",
        u->stats.lost / timediff);
    pa_proplist_setf(u->sink_input->proplist, "iwab.underrun", "%" PRIu64 "lums/s",
        u->stats.underrun / timediff);
    pa_proplist_setf(u->sink_input->proplist, "iwab.overrun", "%" PRIu64 "ms/s",
        u->stats.overrun / timediff);
    pa_proplist_setf(u->sink_input->proplist, "iwab.avg_queue_nblocks", "%u packets",
            u->stats.queue_nblocks / u->stats.count);
    u->stats.lost = 0;
    u->stats.overrun = 0;
    u->stats.underrun = 0;
    u->stats.queue_nblocks = 0;
    u->stats.count = 0;
}

static int rtpoll_work_cb(pa_rtpoll_item *i) {
    struct userdata *u;
    struct pollfd *pollfd;
    ssize_t l;
    void *p;
    pa_usec_t now = pa_rtclock_now();
    pa_memchunk newchunk;

    pa_assert_se(u = pa_rtpoll_item_get_work_userdata(i));
    pa_memchunk_reset(&newchunk);

    pollfd = pa_rtpoll_item_get_pollfd(i, NULL);

    if (pollfd->revents & (POLLERR|POLLNVAL|POLLHUP|POLLOUT)) {
        pa_log("poll() signalled bad revents.");
        return -1;
    }

    if ((pollfd->revents & POLLIN) == 0) {
        return 0;
    }

    pollfd->revents = 0;

    newchunk.memblock = pa_memblock_new(u->core->mempool, MAX_FRAME_SIZE);
    for (;;) {
        p = pa_memblock_acquire(newchunk.memblock);
        l = iwab_read(&u->istream, (char*) p, pa_memblock_get_length(newchunk.memblock),
                &newchunk.index);
        pa_memblock_release(newchunk.memblock);

        if (l < 0) {
            /*
            if (l == -4) {
                pa_log("invalid dot11, type %u, subtype %u, rt offset : %zu",
                        u->istream.dot11_in->type,
                        u->istream.dot11_in->subtype,
                        newchunk.index);
            }
            */

            if (errno == EINTR) {
                // retry;
                continue;
            }

            if (errno == EAGAIN) {
                // no data yet available, or invalid packet
                goto ignore;
            }

            pa_log("Failed to read wireless data : %s", pa_cstrerror(errno));
            goto ignore; // TODO: can/should we signal an error ?
        }

        newchunk.length = l;
        break;
    }

    if (u->sink_input->thread_info.state == PA_SINK_INPUT_CORKED) {
        // We read a valid packet, but sink input is corked, let's start up again
        pa_sink_input_set_state_within_thread(u->sink_input, PA_SINK_INPUT_RUNNING);
    }

    if (!PA_SINK_IS_OPENED(u->sink_input->sink->thread_info.state)) {
        return 0;
    }

    if (u->istream.iw_in->seq == u->seqnb) {
        // this is a repeat packet
        goto ignore;
    }

    pa_assert(pa_frame_aligned(newchunk.length, &u->ss));
    if (u->seqnb != 0 && u->istream.iw_in->seq < u->seqnb) {
        pa_log("Packet disordered. Previous seq : %u, last seq : %u, rewind : %u",
            u->seqnb, u->istream.iw_in->seq, u->seqnb - u->istream.iw_in->seq);
        // hum, maybe the source has restarted, let's reset the counters;
        u->seqnb = 0;
        u->last_pb_ts = 0;
        goto ignore;
    }

    if (u->last_pb_ts != 0 && u->istream.iw_in->timestamp < u->last_pb_ts) {
        pa_log("Timestamps disordered. Previous ts : %" PRIu64 \
            ", last ts : %" PRIu64 ", rewind : %" PRIu64,
            u->last_pb_ts, u->istream.iw_in->timestamp,
            u->last_pb_ts - u->istream.iw_in->timestamp);
        goto ignore;
    }

    if (u->seqnb != 0 && u->istream.iw_in->seq != (u->seqnb + 1)) {
        u->stats.lost += u->istream.iw_in->timestamp - u->last_pb_ts;
        pa_assert(u->istream.iw_in->timestamp > u->last_pb_ts);
        pa_usec_t missing = pa_usec_to_bytes(u->istream.iw_in->timestamp - u->last_pb_ts, &u->ss);
        pa_memchunk filler = newchunk;
        while (missing > 0) {
            if (newchunk.length > missing) {
                filler.length = missing;
            } else {
                filler.length = newchunk.length;
            }

            /*
            pa_log("Packet lost or disordered. Previous seq : %u, last seq : %u. \
                    Missing %luus of playback, duplicating this chunk of length %luus",
                u->seqnb, u->istream.iw_in->seq,
                pa_bytes_to_usec(missing, &u->ss),
                pa_bytes_to_usec(filler.length, &u->ss));
            */
            pa_memblockq_push(u->queue, &filler);
            missing -= filler.length;
        }
    }

    if (pa_memblockq_push(u->queue, &newchunk) < 0) {
        pa_log_debug("Buffer overrun, new packet received but audio queue is full (%u packets)",
                pa_memblockq_get_nblocks(u->queue));
        //pa_memblockq_seek(u->queue, (int64_t) newchunk.length, PA_SEEK_RELATIVE, true); // TODO: why ?
        u->stats.overrun += pa_bytes_to_usec(newchunk.length, &u->ss);
    }

    u->stats.count += 1;
    u->stats.queue_nblocks += pa_memblockq_get_nblocks(u->queue);

    u->seqnb = u->istream.iw_in->seq;
    u->last_pb_ts = u->istream.iw_in->timestamp + pa_bytes_to_usec(newchunk.length, &u->ss);
    pa_memblock_unref(newchunk.memblock);

    if (now >= u->stat_time + STAT_PERIOD) {
        update_stats(u, now);
    }

    return 1;

ignore:
    if (newchunk.memblock) {
        pa_memblock_unref(newchunk.memblock);
    }

    return 0;
}


/* Called from I/O thread context */
static void sink_input_attach(pa_sink_input *i) {
    struct userdata *u;
    struct pollfd *p;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);
    pa_assert(!u->rtpoll_item);
    pa_assert(i->sink->thread_info.rtpoll); // This sink must have an rtpoll !

    u->rtpoll_item = pa_rtpoll_item_new(i->sink->thread_info.rtpoll, PA_RTPOLL_LATE, 1);
    u->stat_time = pa_rtclock_now();
    p = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
    p->fd = u->istream.fd;
    p->events = POLLIN;
    p->revents = 0;

    pa_rtpoll_item_set_work_callback(u->rtpoll_item, rtpoll_work_cb, u);
}

void sink_input_update_max_request(pa_sink_input *i, size_t nbytes) {
    struct userdata *u;
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);
    pa_log("New max request size : %zu", nbytes);
}

/* Called from I/O thread context */
static void sink_input_detach(pa_sink_input *i) {
    struct userdata *u;
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);
    pa_assert(u->rtpoll_item);

    pa_rtpoll_item_free(u->rtpoll_item);
    u->rtpoll_item = NULL;
}

int pa__init(pa_module*m) {
    struct userdata *u = NULL;
    pa_modargs *ma = NULL;
    pa_sink *sink;
    pa_sink_input_new_data data;
    pa_memchunk silence;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    memset(&u->stats, 0, sizeof(u->stats));
    u->module = m;
    u->core = m->core;
    u->seqnb = 0;
    u->last_pb_ts = 0;
    u->rtpoll_item = NULL;
    // TODO: get actual sample spec from sender
    u->ss.format = PA_SAMPLE_S16LE;
    u->ss.rate = 44100;
    u->ss.channels = 2;
    if (!pa_sample_spec_valid(&u->ss)) {
        pa_log("Invalid sample spec");
        goto fail;
    }

    // open istream
    u->iface = pa_modargs_get_value(ma, "iface", DEFAULT_IFACE);
    if (iwab_open(&u->istream, u->iface)) {
        pa_log("Failed to open interface %s, error : %s\n", u->iface, pa_cstrerror(errno));
        goto fail;
    }

    pa_make_fd_nonblock(u->istream.fd);

    // setup sink input
    if (!(sink = pa_namereg_get(u->module->core,
                    pa_modargs_get_value(ma, "sink", NULL), PA_NAMEREG_SINK))) {
        pa_log("Sink does not exist.");
        goto fail;
    }

    pa_sink_input_new_data_init(&data);
    pa_sink_input_new_data_set_sink(&data, sink, false, true);
    data.driver = __FILE__;
    pa_proplist_sets(data.proplist, PA_PROP_MEDIA_ROLE, "stream");
    pa_proplist_setf(data.proplist, PA_PROP_MEDIA_NAME, "wiscast streaming from %s",
            u->iface);
    pa_proplist_setf(data.proplist, "iwab.lost", "%" PRIu64 "ms", (uint64_t) 0);
    pa_proplist_setf(data.proplist, "iwab.overrun", "%" PRIu64 "ms", (uint64_t) 0);
    pa_proplist_setf(data.proplist, "iwab.underrun", "%" PRIu64 "ms", (uint64_t) 0);
    data.module = u->module;
    pa_sink_input_new_data_set_sample_spec(&data, &u->ss);
    pa_sink_input_new(&u->sink_input, u->module->core, &data);
    pa_sink_input_new_data_done(&data);

    if (!u->sink_input) {
        pa_log("Failed to create sink input.");
        goto fail;
    }

    u->sink_input->userdata = u;
    u->sink_input->parent.process_msg = sink_input_process_msg;
    u->sink_input->pop = sink_input_pop_cb;
    u->sink_input->attach = sink_input_attach;
    u->sink_input->detach = sink_input_detach;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->update_max_request = sink_input_update_max_request;
    u->sink_input->process_rewind = sink_input_process_rewind_cb;
    u->sink_input->suspend_within_thread = sink_input_suspend_within_thread;
    pa_sink_input_set_requested_latency(u->sink_input, pa_bytes_to_usec(MAX_FRAME_SIZE, &u->ss));

    // setup audio buffer
    pa_sink_input_get_silence(u->sink_input, &silence);
    u->queue = pa_memblockq_new(
            "module-iwab-input memblockq",
            0,
            8*MAX_FRAME_SIZE,
            4*MAX_FRAME_SIZE,
            &u->ss,
            4*MAX_FRAME_SIZE,
            0,
            0,
            &silence);
    pa_memblock_unref(silence.memblock);

    pa_sink_input_put(u->sink_input);
    //pa_sink_input_cork(u->sink_input, true);
    pa_modargs_free(ma);
    return 0;

fail:
    if (ma) {
        pa_modargs_free(ma);
    }

    if (u->istream.fd) {
        pa_assert_se(iwab_close(&u->istream) == 0);
    }

    if (u) {
        pa_xfree(u);
    }

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink_input) {
        sink_input_kill_cb(u->sink_input);
    }

    if (u->queue) {
        pa_memblockq_free(u->queue);
    }

    if (u->istream.fd) {
        pa_assert_se(iwab_close(&u->istream) == 0);
    }

    memset(u, 0, sizeof(struct userdata));
    pa_xfree(u);
    m->userdata = NULL;
}
