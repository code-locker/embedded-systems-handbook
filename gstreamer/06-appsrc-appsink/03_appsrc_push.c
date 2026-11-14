/*
 * 03_appsrc_push.c — feed YOUR frames into a pipeline (push mode), check them at the other end
 *
 * Concept : appsrc is a source whose "camera" is your program. You set caps
 *           (what the bytes mean), format=TIME, and push GstBuffers with PTS
 *           and duration; then call end_of_stream(). appsrc queues them and
 *           its own streaming thread pushes downstream. Here we generate
 *           GRAY8 gradient frames whose first byte = frame number, and an
 *           appsink checks the exact bytes and timestamps came through.
 * Build   : make run P=gstreamer/06-appsrc-appsink/03_appsrc_push.c
 *           ./build/gstreamer/06-appsrc-appsink/03_appsrc_push --display
 * Read    : README.md  (section "Push mode: feeding your own frames")
 *
 *   main thread                    appsrc's streaming thread       main thread (later)
 *   make buffer ─► push_buffer() ═► [appsrc queue] ─► identity ─► appsink ═► pull_sample()
 *   (ownership passes to appsrc)    block=true: push waits when the queue is full
 */
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
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

#define W 160          /* multiple of 4 -> no row padding for GRAY8 */
#define H 120
#define FPS 25
#define NUM_FRAMES 30

/* Produce one frame. We allocate with gst_buffer_new_allocate (GStreamer
 * owns the memory) and fill it via a WRITE map. Returns transfer full. */
static GstBuffer *make_frame(int n)
{
    GstBuffer *buf = gst_buffer_new_allocate(NULL, W * H, NULL);
    GstMapInfo m;
    gst_buffer_map(buf, &m, GST_MAP_WRITE);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            m.data[y * W + x] = (guint8)((x + y + n) & 0xFF); /* moving gradient */
    m.data[0] = (guint8)n;                                    /* frame id tag */
    gst_buffer_unmap(buf, &m);

    /* Timestamps are YOUR job. Without them, sinks with sync=true cannot
     * schedule frames and muxers/encoders misbehave. */
    GST_BUFFER_PTS(buf) = gst_util_uint64_scale(n, GST_SECOND, FPS);
    GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(1, GST_SECOND, FPS);
    return buf;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    GstElement *pipeline = gst_parse_launch(display
        ? "appsrc name=in ! tee name=t ! queue ! videoconvert ! autovideosink "
          "t. ! queue ! appsink name=out"
        : "appsrc name=in ! identity ! appsink name=out sync=false", NULL);
    CHECK(pipeline != NULL, "parse");
    GstElement *in  = gst_bin_get_by_name(GST_BIN(pipeline), "in");   /* own */
    GstElement *out = gst_bin_get_by_name(GST_BIN(pipeline), "out");  /* own */

    /* Configure appsrc: caps describe the raw bytes; format TIME because we
     * stamp in nanoseconds; block=TRUE + max-bytes = backpressure. */
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "GRAY8",
        "width", G_TYPE_INT, W, "height", G_TYPE_INT, H,
        "framerate", GST_TYPE_FRACTION, FPS, 1, NULL);
    g_object_set(in, "caps", caps, "format", GST_FORMAT_TIME,
                 "is-live", FALSE, "block", TRUE,
                 "max-bytes", (guint64)(4 * W * H), NULL);
    gst_caps_unref(caps);
    /* Without a large enough appsink queue, block=TRUE could deadlock this
     * single-threaded demo: we push everything first, then pull. So let
     * appsink hold all frames (max-buffers=0 means unlimited). In a real app
     * producer and consumer run in different threads. */
    g_object_set(out, "max-buffers", 0, NULL);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    for (int n = 0; n < NUM_FRAMES; n++) {
        GstBuffer *buf = make_frame(n);
        /* push_buffer TAKES ownership (transfer full): do not unref buf.
         * (The "push-buffer" action SIGNAL does NOT take ownership - a classic
         * leak when switching between the two styles.) */
        GstFlowReturn fr = gst_app_src_push_buffer(GST_APP_SRC(in), buf);
        CHECK(fr == GST_FLOW_OK, "push_buffer");
    }
    /* Tell downstream there is no more data: becomes an EOS event after the
     * queued buffers. Without this the pipeline never ends. */
    CHECK(gst_app_src_end_of_stream(GST_APP_SRC(in)) == GST_FLOW_OK, "EOS");

    int got = 0;
    GstSample *s;
    while ((s = gst_app_sink_try_pull_sample(GST_APP_SINK(out), 5 * GST_SECOND))) {
        GstBuffer *b = gst_sample_get_buffer(s);
        GstMapInfo m;
        CHECK(gst_buffer_map(b, &m, GST_MAP_READ), "map");
        CHECK(m.size == W * H, "size");
        CHECK(m.data[0] == got, "frame id byte survived the pipeline");
        CHECK(m.data[5 * W + 7] == (guint8)((7 + 5 + got) & 0xFF), "pixel ok");
        gst_buffer_unmap(b, &m);
        CHECK(GST_BUFFER_PTS(b) == gst_util_uint64_scale(got, GST_SECOND, FPS),
              "PTS preserved");
        gst_sample_unref(s);
        got++;
    }
    CHECK(gst_app_sink_is_eos(GST_APP_SINK(out)), "EOS reached appsink");

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(in);
    gst_object_unref(out);
    gst_object_unref(pipeline);

    printf("pushed %d, received %d frames with correct bytes and PTS\n",
           NUM_FRAMES, got);
    CHECK(got == NUM_FRAMES, "frame count");
    printf("OK: appsrc -> appsink round trip\n");
    gst_deinit();
    return 0;
}
