/*
 * 02_tee_two_branches.c — one source, two consumers, with a queue per branch
 *
 * Concept : tee pushes the SAME buffer (an extra ref, not a copy) into each
 *           of its request src pads, one after another, in the upstream
 *           thread. Each branch starts with a queue so each branch gets its
 *           own thread and a slow branch cannot stall the other one (and so
 *           prerolling sinks cannot deadlock the tee). Two appsinks count
 *           frames with the "new-sample" callback; both must receive every
 *           frame with identical timestamps.
 * Build   : make run P=gstreamer/08-queues-threads-tee/02_tee_two_branches.c
 *           ./build/gstreamer/08-queues-threads-tee/02_tee_two_branches --display
 * Read    : README.md  (section "tee: why every branch needs a queue")
 *
 *                              ┌─[src_0]─► queue ─► appsink A   (thread T2)
 *  videotestsrc ─► tee ────────┤
 *    (thread T1)               └─[src_1]─► queue ─► appsink B   (thread T3)
 *                              (└─[src_2]─► queue ─► videoconvert ─► autovideosink)
 */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
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

#define N_FRAMES 50

typedef struct {
    const char *name;
    int count;                       /* only written by this branch's thread */
    GstClockTime pts[N_FRAMES];
} Branch;

/* Called in the branch's queue thread for every frame. */
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data)
{
    Branch *br = user_data;
    GstSample *s = gst_app_sink_pull_sample(sink);   /* owned: must unref */
    if (!s) return GST_FLOW_EOS;
    GstBuffer *b = gst_sample_get_buffer(s);         /* borrowed from s */
    if (br->count < N_FRAMES) br->pts[br->count] = GST_BUFFER_PTS(b);
    br->count++;
    gst_sample_unref(s);
    return GST_FLOW_OK;
}

static void hook(GstElement *pipeline, const char *name, Branch *br)
{
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), name);  /* +1 */
    GstAppSinkCallbacks cb = { 0 };
    cb.new_sample = on_new_sample;
    gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cb, br, NULL);
    gst_object_unref(sink);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    gchar *desc = g_strdup_printf(
        "videotestsrc num-buffers=%d ! video/x-raw,width=320,height=240 ! tee name=t"
        " t. ! queue ! appsink name=a sync=false"
        " t. ! queue ! appsink name=b sync=false"
        "%s", N_FRAMES,
        display ? " t. ! queue ! videoconvert ! autovideosink" : "");
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);           /* owned */
    g_free(desc);
    CHECK(pipeline != NULL, err ? err->message : "parse failed");

    static Branch A = { "A", 0, {0} }, B = { "B", 0, {0} };
    hook(pipeline, "a", &A);
    hook(pipeline, "b", &B);

    /* tee's src pads are REQUEST pads ("src_%u"), created by parse_launch
     * for each "t." link. Show them. */
    GstElement *tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");     /* +1 */
    int srcpads = 0;
    GstIterator *it = gst_element_iterate_src_pads(tee);
    GValue v = G_VALUE_INIT;
    while (gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
        printf("  tee pad: %s\n", GST_OBJECT_NAME(g_value_get_object(&v)));
        srcpads++;
        g_value_reset(&v);
    }
    g_value_unset(&v);
    gst_iterator_free(it);
    gst_object_unref(tee);
    CHECK(srcpads == (display ? 3 : 2), "unexpected number of tee pads");

    CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "PLAYING failed");
    GstBus *bus = gst_element_get_bus(pipeline);                       /* +1 */
    /* The pipeline posts EOS only when ALL sinks have received EOS. */
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 15 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "no EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);  /* joins all threads */
    gst_object_unref(pipeline);

    printf("branch A got %d frames, branch B got %d frames\n", A.count, B.count);
    CHECK(A.count == N_FRAMES && B.count == N_FRAMES, "a branch lost frames");
    for (int i = 0; i < N_FRAMES; i++)
        CHECK(A.pts[i] == B.pts[i], "branches saw different timestamps");
    printf("OK: tee duplicated every frame (by reference) to both branches\n");
    gst_deinit();
    return 0;
}
