/*
 * 02_appsink_callbacks.c — appsink push mode: GStreamer calls YOU per frame
 *
 * Concept : Instead of a thread blocking on pull_sample, register
 *           GstAppSinkCallbacks. new_sample() runs IN THE STREAMING THREAD
 *           for each frame; inside it you still call pull_sample (it returns
 *           immediately). Keep the callback short: while it runs, upstream
 *           is stalled. Shared state needs atomics or a mutex.
 *           (The older way, emit-signals=TRUE + "new-sample" signal, is
 *           slower: signal emission goes through GObject marshalling.)
 * Build   : make run P=gstreamer/06-appsrc-appsink/02_appsink_callbacks.c
 *           ./build/gstreamer/06-appsrc-appsink/02_appsink_callbacks --display
 * Read    : README.md  (section "Signals vs callbacks, and which thread runs them")
 *
 *   main thread:  set PLAYING ─► wait on bus for EOS ──────────────────────► read counters
 *   streaming:    src ─► convert ─► appsink ─► new_sample() ─► new_sample() ... eos()
 *                                     (callback runs HERE, not in main)
 */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
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

#define NUM_FRAMES 50

typedef struct {
    gint frames;          /* atomic: written in streaming thread          */
    gint eos_seen;        /* atomic                                       */
    guint64 luma_sum;     /* only touched in streaming thread             */
    GThread *main_thread; /* to prove callbacks run in another thread     */
    gint wrong_thread;    /* atomic: callback ran in main thread? (bad)   */
} Stats;

static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data)
{
    Stats *st = user_data;
    if (g_thread_self() == st->main_thread)
        g_atomic_int_set(&st->wrong_thread, 1);

    GstSample *sample = gst_app_sink_pull_sample(sink);  /* full; ready now */
    if (!sample)
        return GST_FLOW_EOS;
    GstBuffer *buf = gst_sample_get_buffer(sample);
    GstMapInfo m;
    if (gst_buffer_map(buf, &m, GST_MAP_READ)) {
        st->luma_sum += m.data[0];   /* trivial "processing": top-left Y */
        gst_buffer_unmap(buf, &m);
    }
    gst_sample_unref(sample);
    g_atomic_int_inc(&st->frames);
    /* Returning an error flow (e.g. GST_FLOW_ERROR) would stop the pipeline. */
    return GST_FLOW_OK;
}

static void on_eos(GstAppSink *sink, gpointer user_data)
{
    (void)sink;
    g_atomic_int_set(&((Stats *)user_data)->eos_seen, 1);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    GstElement *pipeline = gst_parse_launch(display
        ? "videotestsrc num-buffers=50 ! videoconvert ! video/x-raw,format=GRAY8 ! "
          "tee name=t ! queue ! videoconvert ! autovideosink "
          "t. ! queue ! appsink name=out"
        : "videotestsrc num-buffers=50 ! videoconvert ! video/x-raw,format=GRAY8 ! "
          "appsink name=out sync=false", NULL);
    CHECK(pipeline != NULL, "parse");
    GstElement *out = gst_bin_get_by_name(GST_BIN(pipeline), "out"); /* own */

    Stats st = { 0, 0, 0, g_thread_self(), 0 };
    /* The struct is copied by appsink; fields we do not use stay NULL.
     * The last arg is a GDestroyNotify for user_data (none: on our stack). */
    GstAppSinkCallbacks cbs = { 0 };
    cbs.new_sample = on_new_sample;
    cbs.eos = on_eos;
    gst_app_sink_set_callbacks(GST_APP_SINK(out), &cbs, &st, NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 20 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "EOS on bus");
    gst_message_unref(msg);
    gst_object_unref(bus);

    /* Stop BEFORE &st goes out of scope: after NULL no callback can run. */
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(out);
    gst_object_unref(pipeline);

    printf("frames via callback: %d, eos callback: %d, luma sum: %" G_GUINT64_FORMAT "\n",
           st.frames, st.eos_seen, st.luma_sum);
    CHECK(st.frames == NUM_FRAMES, "callback ran once per frame");
    CHECK(st.eos_seen, "eos callback ran");
    CHECK(!st.wrong_thread, "callbacks ran in the streaming thread");
    printf("OK: appsink callbacks delivered every frame from the streaming thread\n");
    gst_deinit();
    return 0;
}
