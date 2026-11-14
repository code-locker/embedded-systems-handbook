/*
 * 03_leaky_queue.c — a full queue: block the producer, or drop old frames?
 *
 * Concept : When the consumer is slower than the producer, a queue fills up.
 *           Default (leaky=no): the producer's thread BLOCKS in the queue
 *           until there is room -> nothing is lost, but a live source would
 *           overflow and latency grows to max-size. leaky=downstream throws
 *           away the OLDEST buffer in the queue to make room -> fresh frames,
 *           bounded latency, frames lost. We make the sink slow with a probe
 *           that sleeps 20 ms per frame and compare both modes, counting
 *           drops with the queue's "overrun" signal and the frames that
 *           reach the sink.
 * Build   : make run P=gstreamer/08-queues-threads-tee/03_leaky_queue.c
 *           ./build/gstreamer/08-queues-threads-tee/03_leaky_queue --display
 * Read    : README.md  (section "queue properties: max-size and leaky")
 *
 *  videotestsrc(live, 100 fps) ─► queue(max 3 buffers, leaky=?) ─► [sink]sink
 *                                                                    ▲ sleeps 20 ms
 *                                                                    (only 50 fps)
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

#define N_FRAMES 60

typedef struct {
    gint overruns;     /* atomic: emitted in the source thread */
    gint delivered;    /* atomic: counted in the queue thread  */
} Stats;

static void on_overrun(GstElement *queue, gpointer user_data)
{
    (void)queue;
    g_atomic_int_inc(&((Stats *)user_data)->overruns);
}

static GstPadProbeReturn slow_sink(GstPad *pad, GstPadProbeInfo *info,
                                   gpointer user_data)
{
    (void)pad; (void)info;
    g_atomic_int_inc(&((Stats *)user_data)->delivered);
    g_usleep(20 * 1000);     /* pretend: slow display / slow inference */
    return GST_PAD_PROBE_OK;
}

static Stats run(const char *leaky, gboolean display)
{
    /* is-live=true: the source produces on the clock at 100 fps, like a
     * camera. It does not wait for the consumer. */
    gchar *desc = g_strdup_printf(
        "videotestsrc is-live=true num-buffers=%d"
        " ! video/x-raw,width=160,height=120,framerate=100/1"
        " ! queue name=q max-size-buffers=3 max-size-bytes=0 max-size-time=0 leaky=%s"
        " ! videoconvert ! %s",
        N_FRAMES, leaky,
        display ? "autovideosink name=sink" : "fakesink name=sink sync=false");
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);           /* owned */
    g_free(desc);
    CHECK(pipeline != NULL, err ? err->message : "parse failed");

    Stats st = { 0, 0 };
    GstElement *q = gst_bin_get_by_name(GST_BIN(pipeline), "q");       /* +1 */
    /* "overrun": the queue is full. With leaky=no the pushing thread then
     * blocks; with leaky=downstream an old buffer is dropped. */
    g_signal_connect(q, "overrun", G_CALLBACK(on_overrun), &st);
    gst_object_unref(q);

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink"); /* +1 */
    GstPad *p = gst_element_get_static_pad(sink, "sink");              /* +1 */
    gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, slow_sink, &st, NULL);
    gst_object_unref(p);
    gst_object_unref(sink);

    gint64 t0 = g_get_monotonic_time();
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);                       /* +1 */
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 20 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "no EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    printf("  leaky=%-10s overruns=%2d delivered=%2d/%d  wall=%3lld ms\n",
           leaky, st.overruns, st.delivered, N_FRAMES,
           (long long)(g_get_monotonic_time() - t0) / 1000);
    return st;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    Stats blocking = run("no", display);
    Stats leaking = run("downstream", display);

    /* leaky=no never loses data: the source thread just waits. (A real live
     * camera cannot wait; it would drop frames in the driver instead.) */
    CHECK(blocking.delivered == N_FRAMES, "leaky=no must deliver everything");
    /* leaky=downstream: the queue overflowed and threw old frames away. */
    CHECK(leaking.overruns > 0, "queue should have overflowed");
    CHECK(leaking.delivered < N_FRAMES, "leaky queue should drop frames");
    CHECK(leaking.delivered > 0, "but some frames must get through");
    printf("OK: leaky=no blocks the producer, leaky=downstream drops old frames\n");
    gst_deinit();
    return 0;
}
