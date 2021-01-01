/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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
#include <sys/stat.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>
#endif

#include <pulse/rtclock.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/source.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/thread.h>
#include <pulsecore/thread-mq.h>
#include <pulsecore/rtpoll.h>
#include <pulsecore/poll.h>

#include "net.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("UNIX pipe source");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(false);
PA_MODULE_USAGE(
        "source_name=<name of source> "
        "format=<sample format> "
        "rate=<sample rate> "
        "channels=<number of channels> "
        "channel_map=<channel map> "
        "iface=<wireless interface> "
        );

#define DEFAULT_SOURCE_NAME "swsrc"
#define DEFAULT_IFACE "mon0"
#define MAX_FRAME_SIZE 1600

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_source *source;

    pa_thread *thread;
    pa_thread_mq thread_mq;
    pa_rtpoll *rtpoll;

    char *filename;
    int fd;

    pa_usec_t next_pb_ts;
    const char *iface;
    int retries;
    pa_memchunk memchunk;
    struct wicast wistream;
    pa_rtpoll_item *rtpoll_item;
};

static const char* const valid_modargs[] = {
    "source_name",
    "format",
    "rate",
    "channels",
    "channel_map",
    "iface",
    NULL
};

static int source_process_msg(
        pa_msgobject *o,
        int code,
        void *data,
        int64_t offset,
        pa_memchunk *chunk) {

    struct userdata *u = PA_SOURCE(o)->userdata;

    switch (code) {

        case PA_SOURCE_MESSAGE_GET_LATENCY: {
            *((int64_t*) data) = pa_bytes_to_usec(MAX_FRAME_SIZE, &u->source->sample_spec);
            return 0;
        }
    }

    return pa_source_process_msg(o, code, data, offset, chunk);
}

static void thread_func(void *userdata) {
    struct userdata *u = userdata;

    pa_assert(u);
    pa_log_debug("Thread starting up");
    pa_thread_mq_install(&u->thread_mq);
    uint32_t last_seq = 0;
    pa_usec_t last_ts = 0;
    pa_usec_t offset = 0;
    u->next_pb_ts = 0;
    pa_usec_t max_delay = pa_bytes_to_usec(MAX_FRAME_SIZE, &u->source->sample_spec);

    for (;;) {
        int ret;
        struct pollfd *pollfd;
        pa_usec_t now = pa_rtclock_now();
        if (now < (u->next_pb_ts + max_delay) || u->next_pb_ts > (now + max_delay)) {
            u->next_pb_ts = now + (3*max_delay)/4;
        }

        pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);

        /* Try to read some data and pass it on to the source driver */
        if (u->source->thread_info.state == PA_SOURCE_RUNNING && pollfd->revents) {
            ssize_t l;
            void *p;
            pollfd->revents = 0;

            // Re-alloc memblock if it doesn't exist ? TODO: Why is this needed ?
            if (!u->memchunk.memblock) {
                u->memchunk.memblock = pa_memblock_new(u->core->mempool, MAX_FRAME_SIZE);
            }

            u->memchunk.index = u->memchunk.length = 0;
            pa_assert(pa_memblock_get_length(u->memchunk.memblock) > u->memchunk.index);

            p = pa_memblock_acquire(u->memchunk.memblock);
            l = wicast_read(&u->wistream, (char*) p, pa_memblock_get_length(u->memchunk.memblock),
                    &u->memchunk.index);
            pa_memblock_release(u->memchunk.memblock);
            pa_assert(l != 0); /* EOF cannot happen, since we opened the fifo for both reading and writing */
            if (l < 100) {
                if (l == -4) {
                    pa_log("invalid dot11, type %u, subtype %u, rt offset : %zu",
                            u->wistream.dot11_in->type,
                            u->wistream.dot11_in->subtype,
                            u->memchunk.index);
                }
                if (errno == EAGAIN || errno == EINTR) {
                    continue;
                }
                pa_log("Failed to read wireless data : %s", pa_cstrerror(errno));
                goto fail;
            }

            /*
            pa_log("sw seq %u, sw ts %lu, sw len : %u, len : %lu, data offset : %lu",
                    u->wistream.sw_in->seq,
                    u->wistream.sw_in->timestamp,
                    u->wistream.sw_in->length,
                    l, u->memchunk.index);
            */
            u->memchunk.length = (size_t) l;
            if (!pa_frame_aligned(l, &u->source->sample_spec)) {
                pa_log("error, unaligned frame. l : %ld, swag seq : %u, swag length : %u", l,
                        u->wistream.sw_in->seq,
                        u->wistream.sw_in->length);
                continue;
            }

            if (u->wistream.sw_in->seq == last_seq) {
                // this is a retry, and we already got the last one : ignore.
                pa_log("@%lu, got a retry, ignoring", now);
                continue;
            } else if (u->wistream.sw_in->seq != (last_seq + 1)) {
                pa_log("last_seq : %u, sw seq %u, sw ts %lu, sw len : %u, len : %lu, data offset : %lu",
                        last_seq,
                        u->wistream.sw_in->seq,
                        u->wistream.sw_in->timestamp,
                        u->wistream.sw_in->length,
                        l, u->memchunk.index);
            }

            if (last_seq == 0) {
                offset = (3*max_delay)/4;
            } else {
                offset = u->wistream.sw_in->timestamp - last_ts;
            }

            //pa_log("to play in %luus, now %lu, timeout %lu", offset, pa_rtclock_now(), pb_ts + offset);
            last_ts = u->wistream.sw_in->timestamp;
            last_seq = u->wistream.sw_in->seq;
            //u->next_pb_ts += offset;
            pa_usec_t plen = pa_bytes_to_usec(l, &u->source->sample_spec);
            pa_log("@%lu, got a new audio packet, %luus long, next pb ts %lu, delay : %lu",
                    now, plen, u->next_pb_ts, u->next_pb_ts - now);
            if (now + max_delay/2 > u->next_pb_ts) {
                pa_log("@%lu, warning, we just got an audio packet but pb time %lu is very soon !",
                        now, u->next_pb_ts);
            }
            pa_rtpoll_set_timer_absolute(u->rtpoll, u->next_pb_ts);
        } else if (u->source->thread_info.state == PA_SOURCE_RUNNING && pollfd->revents == 0) {
            if (now >= u->next_pb_ts) {
                pa_log("@%lu time to play, next pb ts : %lu", now, u->next_pb_ts);
                // playback time
                if (u->memchunk.length != 0) {
                    pa_source_post(u->source, &u->memchunk);
                    u->next_pb_ts += pa_bytes_to_usec(u->memchunk.length, &u->source->sample_spec);
                    pa_memblock_unref(u->memchunk.memblock);
                    pa_memchunk_reset(&u->memchunk);
                } else {
                    pa_log("warning, empty buffer : %lu, last_seq : %u",
                            u->memchunk.length, last_seq);
                    u->next_pb_ts += max_delay;
                }

                // set next wakeup time provisionally, this should be update by the next
                // network packet before timer runs out.
                // u->next_pb_ts += max_delay;
                pa_rtpoll_set_timer_disabled(u->rtpoll);
            } else {
                pa_log("@%lu not time to play yet, next pb ts : %lu", now, u->next_pb_ts);
                pa_rtpoll_set_timer_absolute(u->rtpoll, u->next_pb_ts);
            }
        }

        /* Hmm, nothing to do. Let's sleep */
        pollfd->events = (short) (u->source->thread_info.state == PA_SOURCE_RUNNING ? POLLIN : 0);

        if ((ret = pa_rtpoll_run(u->rtpoll)) < 0)
            goto fail;

        if (ret == 0)
            goto finish;

        pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);

        if (pollfd->revents & ~POLLIN) {
            pa_log("Connection closed.");
            goto fail;
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

int pa__init(pa_module *m) {
    struct userdata *u;
    pa_sample_spec ss;
    pa_channel_map map;
    pa_modargs *ma;
    struct pollfd *pollfd;
    pa_source_new_data data;

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

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    pa_memchunk_reset(&u->memchunk);
    u->rtpoll = pa_rtpoll_new();

    if (pa_thread_mq_init(&u->thread_mq, m->core->mainloop, u->rtpoll) < 0) {
        pa_log("pa_thread_mq_init() failed.");
        goto fail;
    }

    u->iface = pa_modargs_get_value(ma, "iface", DEFAULT_IFACE);
    if (wicast_open(&u->wistream, u->iface)) {
        pa_log("Failed to open interface %s, error : %s\n", u->iface, pa_cstrerror(errno));
        goto fail;
    }

    pa_make_fd_nonblock(u->fd);
    pa_source_new_data_init(&data);
    data.driver = __FILE__;
    data.module = m;
    pa_source_new_data_set_name(&data, pa_modargs_get_value(ma, "source_name", DEFAULT_SOURCE_NAME));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_DESCRIPTION, _("Swag Source"));
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_CLASS, "abstract"); // TODO: what is this ?
    pa_proplist_sets(data.proplist, PA_PROP_DEVICE_STRING, u->iface);
    pa_source_new_data_set_sample_spec(&data, &ss);
    pa_source_new_data_set_channel_map(&data, &map);

    if (pa_modargs_get_proplist(ma, "source_properties", data.proplist, PA_UPDATE_REPLACE) < 0) {
        pa_log("Invalid properties");
        pa_source_new_data_done(&data);
        goto fail;
    }

    u->source = pa_source_new(m->core, &data, PA_SOURCE_LATENCY);
    pa_source_new_data_done(&data);

    if (!u->source) {
        pa_log("Failed to create source.");
        goto fail;
    }

    u->source->parent.process_msg = source_process_msg;
    u->source->userdata = u;

    pa_source_set_asyncmsgq(u->source, u->thread_mq.inq);
    pa_source_set_rtpoll(u->source, u->rtpoll);
    pa_source_set_fixed_latency(u->source, pa_bytes_to_usec(MAX_FRAME_SIZE, &u->source->sample_spec));

    u->rtpoll_item = pa_rtpoll_item_new(u->rtpoll, PA_RTPOLL_NEVER, 1);
    pollfd = pa_rtpoll_item_get_pollfd(u->rtpoll_item, NULL);
    pollfd->fd = u->wistream.fd;
    pollfd->events = pollfd->revents = 0;

    if (!(u->thread = pa_thread_new("swag-source", thread_func, u))) {
        pa_log("Failed to create thread.");
        goto fail;
    }

    pa_source_put(u->source);
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

    return pa_source_linked_by(u->source);
}

void pa__done(pa_module *m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->source)
        pa_source_unlink(u->source);

    if (u->thread) {
        pa_asyncmsgq_send(u->thread_mq.inq, NULL, PA_MESSAGE_SHUTDOWN, NULL, 0, NULL);
        pa_thread_free(u->thread);
    }

    pa_thread_mq_done(&u->thread_mq);

    if (u->source)
        pa_source_unref(u->source);

    if (u->memchunk.memblock)
        pa_memblock_unref(u->memchunk.memblock);

    if (u->rtpoll_item)
        pa_rtpoll_item_free(u->rtpoll_item);

    if (u->rtpoll)
        pa_rtpoll_free(u->rtpoll);

    pa_assert_se(wicast_close(&u->wistream) == 0);
    pa_xfree(u);
}
