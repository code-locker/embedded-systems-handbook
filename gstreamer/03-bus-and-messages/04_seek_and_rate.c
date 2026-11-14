/*
 * 04_seek_and_rate.c — events and queries: SEEK, SEGMENT, FLUSH, rate
 *
 * Concept : the app sends a SEEK event (travels UPSTREAM to the source).
 *           A flushing seek emits FLUSH_START/FLUSH_STOP (throw away what
 *           is in flight), then a new SEGMENT event downstream carrying the
 *           new start and the playback RATE. With rate=2.0 the sink plays
 *           the same stream time in half the wall-clock time.
 *           Queries ask questions synchronously: SEEKING, POSITION.
 * Build   : make run P=gstreamer/03-bus-and-messages/04_seek_and_rate.c
 *           ./build/gstreamer/03-bus-and-messages/04_seek_and_rate --display
 * Read    : README.md  (sections "Events and queries" and "Seeking")
 *
 *   app ──SEEK(rate=2, 2s..3s)──► pipeline ──► sink ──► ... ──► src (upstream)
 *   src ──FLUSH_START, FLUSH_STOP, SEGMENT(rate=2,start=2s), buffers──► sink
 *
 *   videotestsrc ─► [320x240@30] ─► fakesink sync=true  (pad probe on sink)
 */
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", msg, __FILE__,   \
                    __LINE__);                                             \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

typedef struct {
    GMutex lock;              /* probe runs in the streaming thread */
    int flush_start, flush_stop, segments, buffers;
    gdouble seg_rate;
    GstClockTime seg_start, first_pts, last_pts;
} Seen;

/* Pad probe on the sink's sink pad: sees every event and buffer that
 * arrives (lesson 07 explains probes in depth). */
static GstPadProbeReturn probe(GstPad *pad, GstPadProbeInfo *info,
                               gpointer data)
{
    (void)pad;
    Seen *s = data;
    g_mutex_lock(&s->lock);
    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        GstBuffer *b = GST_PAD_PROBE_INFO_BUFFER(info);
        if (s->buffers == 0)
            s->first_pts = GST_BUFFER_PTS(b);
        s->last_pts = GST_BUFFER_PTS(b);
        s->buffers++;
    } else {
        GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);
        switch (GST_EVENT_TYPE(ev)) {
        case GST_EVENT_FLUSH_START: s->flush_start++; break;
        case GST_EVENT_FLUSH_STOP:  s->flush_stop++;  break;
        case GST_EVENT_SEGMENT: {
            const GstSegment *seg = NULL;
            gst_event_parse_segment(ev, &seg);      /* transfer none */
            s->segments++;
            s->seg_rate = seg->rate;
            s->seg_start = seg->start;
            /* new segment = new playback window: restart counting */
            s->buffers = 0;
            break;
        }
        default: break;
        }
    }
    g_mutex_unlock(&s->lock);
    return GST_PAD_PROBE_OK;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    /* No num-buffers: the SEEK's stop position will end the stream. */
    GstElement *p = gst_parse_launch(display
        ? "videotestsrc pattern=ball ! video/x-raw,width=320,height=240,"
          "framerate=30/1 ! timeoverlay ! videoconvert ! autovideosink name=sink"
        : "videotestsrc pattern=ball ! video/x-raw,width=320,height=240,"
          "framerate=30/1 ! fakesink name=sink sync=true", NULL);
    CHECK(p, "parse");

    Seen seen = { 0 };
    g_mutex_init(&seen.lock);
    GstElement *sink = gst_bin_get_by_name(GST_BIN(p), "sink");
    GstPad *sinkpad = gst_element_get_static_pad(sink, "sink");
    /* autovideosink is a bin: use its ghost "sink" pad, same API. */
    gst_pad_add_probe(sinkpad, GST_PAD_PROBE_TYPE_BUFFER |
                      GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM |
                      GST_PAD_PROBE_TYPE_EVENT_FLUSH, probe, &seen, NULL);
    gst_object_unref(sinkpad);
    gst_object_unref(sink);

    /* 1. Seek only makes sense once prerolled: PAUSED and WAIT for it. */
    gst_element_set_state(p, GST_STATE_PAUSED);
    CHECK(gst_element_get_state(p, NULL, NULL, 5 * GST_SECOND)
              == GST_STATE_CHANGE_SUCCESS, "prerolled");

    /* 2. QUERY: can we seek, and in which range? */
    GstQuery *q = gst_query_new_seeking(GST_FORMAT_TIME);       /* full */
    gboolean seekable = FALSE;
    if (gst_element_query(p, q))
        gst_query_parse_seeking(q, NULL, &seekable, NULL, NULL);
    gst_query_unref(q);
    printf("seekable: %s\n", seekable ? "yes" : "no");
    CHECK(seekable, "videotestsrc is seekable in TIME");

    /* 3. SEEK EVENT: rate 2.0, play stream time [2 s, 3 s), then EOS.
     *    FLUSH: discard queued data now (instant response).
     *    ACCURATE: start exactly at 2 s (not at the nearest keyframe). */
    gboolean ok = gst_element_seek(p, 2.0, GST_FORMAT_TIME,
        GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
        GST_SEEK_TYPE_SET, 2 * GST_SECOND,
        GST_SEEK_TYPE_SET, 3 * GST_SECOND);
    CHECK(ok, "seek accepted");
    /* A flushing seek makes the pipeline preroll again: wait for it. */
    gst_element_get_state(p, NULL, NULL, 5 * GST_SECOND);

    gint64 t0 = g_get_monotonic_time();
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
                                    GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    gint64 wall_us = g_get_monotonic_time() - t0;
    CHECK(m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS, "EOS at seek stop");
    gst_message_unref(m);

    /* 4. QUERY position after EOS: should be at the stop (3 s). */
    gint64 pos = -1;
    gst_element_query_position(p, GST_FORMAT_TIME, &pos);

    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);

    printf("flush start/stop: %d/%d, segments: %d, last segment rate=%.1f "
           "start=%" GST_TIME_FORMAT "\n", seen.flush_start, seen.flush_stop,
           seen.segments, seen.seg_rate, GST_TIME_ARGS(seen.seg_start));
    printf("buffers after seek: %d, PTS %" GST_TIME_FORMAT " .. %"
           GST_TIME_FORMAT "\n", seen.buffers, GST_TIME_ARGS(seen.first_pts),
           GST_TIME_ARGS(seen.last_pts));
    printf("1 s of stream played in %.2f s wall time; position=%"
           GST_TIME_FORMAT "\n", wall_us / 1e6, GST_TIME_ARGS(pos));

    CHECK(seen.flush_start >= 1 && seen.flush_stop >= 1, "flushing seek");
    CHECK(seen.segments >= 2, "initial segment + seek segment");
    CHECK(seen.seg_rate == 2.0, "segment carries rate 2.0");
    CHECK(seen.seg_start == 2 * GST_SECOND, "segment starts at 2 s");
    CHECK(seen.first_pts == 2 * GST_SECOND, "first frame after seek at 2 s");
    CHECK(seen.last_pts < 3 * GST_SECOND, "stopped before 3 s");
    CHECK(seen.buffers == 30, "1 s at 30 fps = 30 frames");
    /* rate 2.0: 1 s of stream in ~0.5 s of wall time */
    CHECK(wall_us > 350000 && wall_us < 800000, "double speed");
    g_mutex_clear(&seen.lock);
    printf("OK: seek + rate verified through events, queries and timing\n");
    gst_deinit();
    return 0;
}
