
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

#include <pulsecore/core-error.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
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

#define DEFAULT_SOURCE_NAME "swsrc"
#define DEFAULT_IFACE "mon0"
#define MAX_FRAME_SIZE 1600

#define MEMBLOCKQ_MAXLENGTH (1024*1024*40)
#define DEATH_TIMEOUT 20
#define RATE_UPDATE_INTERVAL (5*PA_USEC_PER_SEC)

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
    pa_memchunk memchunk;
    bool first_packet;
    pa_usec_t offset;
    pa_rtpoll_item *rtpoll_item;
    uint32_t seqnb;
    struct wicast wistream;
    int retries;
    pa_sample_spec ss;
};

/* Called from I/O thread context */
static int sink_input_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct userdata *u = PA_SINK_INPUT(o)->userdata;

    switch (code) {
        case PA_SINK_INPUT_MESSAGE_GET_LATENCY:
            *((int64_t*) data) = pa_bytes_to_usec(MAX_FRAME_SIZE, &u->sink_input->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
            break;
    }

    return pa_sink_input_process_msg(o, code, data, offset, chunk);
}

/* Called from I/O thread context */
static void sink_input_process_rewind_cb(pa_sink_input *i, size_t nbytes) {
    struct userdata* u;
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (!u->memchunk.memblock) {
        return;
    }

    if (nbytes >= u->memchunk.length) {
        nbytes = u->memchunk.length;
        pa_memblock_unref(u->memchunk.memblock);
        pa_memchunk_reset(&u->memchunk);
    } else {
        u->memchunk.length -= nbytes;
        u->memchunk.index += nbytes;
    }
}

/* Called from I/O thread context */
static int sink_input_pop_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    struct userdata* u;
    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    if (u->memchunk.length == 0) {
        return -1;
    }

    *chunk = u->memchunk;
    pa_memchunk_reset(&u->memchunk);
    return 0;
}

static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_sink_input_assert_ref(i);
    pa_assert_se(u = i->userdata);

    pa_sink_input_unlink(u->sink_input);
    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    pa_module_unload_request(u->module, true);
}

static int rtpoll_work_cb(pa_rtpoll_item *i) {
    struct userdata *u;
    struct pollfd *pollfd;
    ssize_t l;
    void *p;
    pa_memchunk newchunk;
    pa_assert_se(u = pa_rtpoll_item_get_work_userdata(i));

    pa_memchunk_reset(&newchunk);
    newchunk.memblock = pa_memblock_new(u->core->mempool, MAX_FRAME_SIZE);

    pa_assert_se(u = pa_rtpoll_item_get_work_userdata(i));

    pollfd = pa_rtpoll_item_get_pollfd(i, NULL);

    if (pollfd->revents & (POLLERR|POLLNVAL|POLLHUP|POLLOUT)) {
        pa_log("poll() signalled bad revents.");
        return -1;
    }

    if ((pollfd->revents & POLLIN) == 0) {
        return 0;
    }

    pollfd->revents = 0;

    if (!PA_SINK_IS_OPENED(u->sink_input->sink->thread_info.state)) {
        return 0;
    }

    for (;;) {
        p = pa_memblock_acquire(newchunk.memblock);
        l = wicast_read(&u->wistream, (char*) p, pa_memblock_get_length(newchunk.memblock),
                &newchunk.index);
        pa_memblock_release(newchunk.memblock);

        if (l < 0) {
            if (l == -4) {
                pa_log("invalid dot11, type %u, subtype %u, rt offset : %zu",
                        u->wistream.dot11_in->type,
                        u->wistream.dot11_in->subtype,
                        u->memchunk.index);
            }

            if (errno == EINTR) {
                // retry;
                continue;
            }

            if (errno == EAGAIN) {
                // no data available, or invalid packet
                return 0;
            }

            pa_log("Failed to read wireless data : %s", pa_cstrerror(errno));
            return 0; // TODO: How can we signal an error ?
        }

        newchunk.length = l;
        break;
    }

    if (u->wistream.sw_in->seq == u->seqnb) {
        return 0;
    }

    if (u->memchunk.memblock) {
        pa_log("Buffer overrun, new packet received but previous chunk not yet consumed");
        pa_memblock_unref(u->memchunk.memblock);
    }

    u->memchunk = newchunk;

    if (u->seqnb == 0) {
        u->offset = u->wistream.sw_in->timestamp;
    } else if (u->wistream.sw_in->seq != (u->seqnb + 1)) {
        pa_log("Packet lost or disordered. Previous seq : %u, last seq : %u", u->seqnb, u->wistream.sw_in->seq);
    }

    u->seqnb = u->wistream.sw_in->seq;

    return 1;
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
    p = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
    p->fd = u->wistream.fd;
    p->events = POLLIN;
    p->revents = 0;

    pa_rtpoll_item_set_work_callback(u->rtpoll_item, rtpoll_work_cb, u);
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

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew(struct userdata, 1);
    u->module = m;
    u->core = m->core;
    pa_memchunk_reset(&u->memchunk);
    u->seqnb = 0;
    u->offset = 0;
    u->rtpoll_item = NULL;
    // TODO: get actual sample spec from sender
    u->ss.format = PA_SAMPLE_S16LE;
    u->ss.rate = 44100;
    u->ss.channels = 2;
    if (!pa_sample_spec_valid(&u->ss)) {
        pa_log("Invalid sample spec");
        goto fail;
    }

    // open wistream
    u->iface = pa_modargs_get_value(ma, "iface", DEFAULT_IFACE);
    if (wicast_open(&u->wistream, u->iface)) {
        pa_log("Failed to open interface %s, error : %s\n", u->iface, pa_cstrerror(errno));
        goto fail;
    }

    pa_make_fd_nonblock(u->wistream.fd);

    if (!(sink = pa_namereg_get(u->module->core,
                    pa_modargs_get_value(ma, "sink", NULL), PA_NAMEREG_SINK))) {
        pa_log("Sink does not exist.");
        goto fail;
    }

    // setup sink input
    pa_sink_input_new_data_init(&data);
    pa_sink_input_new_data_set_sink(&data, sink, false, true);
    data.driver = __FILE__;
    pa_proplist_sets(data.proplist, PA_PROP_MEDIA_ROLE, "stream");
    pa_proplist_setf(data.proplist, PA_PROP_MEDIA_NAME, "wiscast streaming from %s",
            u->iface);
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
    u->sink_input->process_rewind = sink_input_process_rewind_cb;

    pa_sink_input_put(u->sink_input);

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma) {
        pa_modargs_free(ma);
    }

    if (u->wistream.fd) {
        pa_assert_se(wicast_close(&u->wistream) == 0);
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

    if (u->memchunk.memblock) {
        pa_memblock_unref(u->memchunk.memblock);
    }

    if (u->wistream.fd) {
        pa_assert_se(wicast_close(&u->wistream) == 0);
    }

    pa_xfree(u);
}
