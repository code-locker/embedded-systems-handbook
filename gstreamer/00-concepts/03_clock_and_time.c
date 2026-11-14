/*
 * 03_clock_and_time.c — clock, base time, running time and timestamps
 *
 * Concept : a pipeline picks ONE clock. When it goes to PLAYING it records
 *           base_time = clock_now. Every buffer has a PTS in *running time*;
 *           a synchronising sink waits until
 *               clock_now - base_time >= PTS   (+ latency)
 *           before rendering. This program checks that rule with real
 *           numbers by watching buffers arrive at a sink with sync=true.
 * Build   : make run P=gstreamer/00-concepts/03_clock_and_time.c
 * Read    : README.md  (section "Clocks and time")
 *
 *   absolute clock time ──────────────────────────────────────────►
 *          |base_time              |base_time + PTS
 *          └── running time 0 ──── └── buffer with PTS is shown here
 *
 *   videotestsrc (25 fps, 15 frames) ──► fakesink sync=true
 *                                          └ "handoff": records clock time
 */
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", msg, __FILE__,   \
                    __LINE__);                                             \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

#define NFRAMES 15

typedef struct {
    GstElement *pipeline;           /* borrowed: main() owns it          */
    int n;
    gint64 max_early_ns;            /* worst "rendered too early" seen   */
    GstClockTime last_pts;
} Timing;

/* fakesink "handoff": runs in the STREAMING thread, right when the sink
 * renders a buffer (after the clock wait, because sync=true). */
static void on_handoff(GstElement *sink, GstBuffer *buf, GstPad *pad,
                       gpointer user_data)
{
    (void)sink; (void)pad;
    Timing *t = user_data;
    GstClock *clock = gst_element_get_clock(t->pipeline);   /* full ref */
    if (!clock)
        return;
    GstClockTime now = gst_clock_get_time(clock);
    GstClockTime base = gst_element_get_base_time(t->pipeline);
    gst_object_unref(clock);

    GstClockTime running = now - base;        /* "how long have we played" */
    GstClockTime pts = GST_BUFFER_PTS(buf);   /* "when should it show"     */
    gint64 early = (gint64)pts - (gint64)running; /* >0 means too early   */
    if (early > t->max_early_ns)
        t->max_early_ns = early;
    if (t->n < 4 || t->n == NFRAMES - 1)
        printf("  frame %2d  PTS=%" GST_TIME_FORMAT "  running=%"
               GST_TIME_FORMAT "\n", t->n, GST_TIME_ARGS(pts),
               GST_TIME_ARGS(running));
    t->last_pts = pts;
    t->n++;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(
        "videotestsrc num-buffers=" G_STRINGIFY(NFRAMES)
        " ! video/x-raw,width=160,height=120,framerate=25/1"
        " ! fakesink name=sink sync=true signal-handoffs=true", &err);
    CHECK(pipeline && !err, "parse");

    Timing t = { .pipeline = pipeline, .max_early_ns = G_MININT64 };
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    g_signal_connect(sink, "handoff", G_CALLBACK(on_handoff), &t);
    gst_object_unref(sink);       /* the pipeline still holds the sink */

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "EOS");
    gst_message_unref(msg);

    /* Which clock did the pipeline choose? With no audio sink or live
     * source providing one, it is the GstSystemClock (monotonic). */
    GstClock *clock = gst_pipeline_get_clock(GST_PIPELINE(pipeline));
    printf("pipeline clock: %s (%s)\n", GST_OBJECT_NAME(clock),
           G_OBJECT_TYPE_NAME(clock));
    gst_object_unref(clock);

    /* Position query: "where are we in the stream?" (stream time). */
    gint64 pos = -1;
    gboolean have_pos =
        gst_element_query_position(pipeline, GST_FORMAT_TIME, &pos);
    printf("position at EOS: %" GST_TIME_FORMAT "\n", GST_TIME_ARGS(pos));

    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    printf("worst case: buffer rendered %.3f ms before its PTS\n",
           t.max_early_ns / 1e6);
    CHECK(t.n == NFRAMES, "all frames rendered");
    /* 25 fps -> PTS step 40 ms; last PTS = 14 * 40 ms = 560 ms. */
    CHECK(t.last_pts == (NFRAMES - 1) * 40 * GST_MSECOND, "PTS = n / fps");
    /* sync=true: the sink never renders before PTS (allow 2 ms jitter). */
    CHECK(t.max_early_ns <= 2 * (gint64)GST_MSECOND, "sink waited for PTS");
    CHECK(have_pos && pos >= (gint64)(t.last_pts), "position query works");

    printf("OK: sink rendered each buffer at base_time + PTS\n");
    gst_deinit();
    return 0;
}
