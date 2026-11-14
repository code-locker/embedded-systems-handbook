/*
 * 01_appsink_pull.c — get frames OUT of GStreamer into your C code (pull mode)
 *
 * Concept : appsink is a sink whose "screen" is your program. In pull mode
 *           your thread calls gst_app_sink_pull_sample() and blocks until a
 *           frame is ready, like reading from a queue. A GstSample bundles
 *           the buffer + its caps + segment. Map the buffer to read pixels.
 * Build   : make run P=gstreamer/06-appsrc-appsink/01_appsink_pull.c
 *           ./build/gstreamer/06-appsrc-appsink/01_appsink_pull --display
 * Read    : README.md  (section "Pull mode: the consumer asks")
 *
 *   streaming thread                               │ your main thread
 *   videotestsrc ─► capsfilter(RGB 64x48) ─► appsink ══ internal queue ══► pull_sample()
 *   pattern=solid-color foreground-color=0xFF2080C0 │   (max-buffers=4)     map, read pixels
 *                                                   │                       unref sample
 *
 *   --display: a tee also shows the same frames in a window.
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

#define NUM_FRAMES 40
#define W 64
#define H 48

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    /* foreground-color is ARGB: R=0x20 G=0x80 B=0xC0. With format=RGB and
     * W*3 = 192 (multiple of 4) there is no row padding. */
    gchar *desc = g_strdup_printf(
        "videotestsrc num-buffers=%d pattern=solid-color "
        "foreground-color=0xFF2080C0 ! "
        "video/x-raw,format=RGB,width=%d,height=%d,framerate=30/1 ! %s"
        "appsink name=out max-buffers=4 drop=false sync=%s",
        NUM_FRAMES, W, H,
        display ? "tee name=t ! queue ! videoconvert ! autovideosink t. ! queue ! " : "",
        display ? "true" : "false");
    GstElement *pipeline = gst_parse_launch(desc, NULL);
    g_free(desc);
    CHECK(pipeline != NULL, "parse");
    GstElement *out = gst_bin_get_by_name(GST_BIN(pipeline), "out"); /* own */
    GstAppSink *sink = GST_APP_SINK(out);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    int frames = 0;
    GstClockTime last_pts = GST_CLOCK_TIME_NONE;
    for (;;) {
        /* Blocks until a sample, EOS (returns NULL) or timeout (NULL).
         * pull_sample() without timeout would block forever on a stall -
         * use try_pull_sample in real code. Transfer FULL: we unref. */
        GstSample *sample = gst_app_sink_try_pull_sample(sink, 5 * GST_SECOND);
        if (!sample)
            break;
        GstBuffer *buf = gst_sample_get_buffer(sample);    /* borrowed */
        GstCaps *caps = gst_sample_get_caps(sample);       /* borrowed */
        if (frames == 0) {
            gchar *s = gst_caps_to_string(caps);
            printf("caps: %s\n", s);
            g_free(s);
        }

        /* gst_buffer_map gives a pointer valid until unmap. With dmabuf or
         * GPU memory this is where a copy/sync may happen (lesson 13). */
        GstMapInfo map;
        CHECK(gst_buffer_map(buf, &map, GST_MAP_READ), "map");
        CHECK(map.size == W * H * 3, "RGB frame size");
        /* Check a few pixels anywhere in the frame. */
        for (int i = 0; i < W * H; i += 97) {
            const guint8 *px = map.data + i * 3;
            CHECK(px[0] == 0x20 && px[1] == 0x80 && px[2] == 0xC0,
                  "pixel colour is the solid colour we asked for");
        }
        gst_buffer_unmap(buf, &map);

        /* Timestamps: PTS increases by 1/30 s per frame. */
        GstClockTime pts = GST_BUFFER_PTS(buf);
        if (GST_CLOCK_TIME_IS_VALID(last_pts))
            CHECK(pts - last_pts == gst_util_uint64_scale(GST_SECOND, 1, 30) ||
                  pts - last_pts == gst_util_uint64_scale_ceil(GST_SECOND, 1, 30),
                  "PTS step = 1/30 s");
        last_pts = pts;
        if (frames % 10 == 0)
            printf("frame %2d pts=%" GST_TIME_FORMAT " size=%zu\n", frames,
                   GST_TIME_ARGS(pts), gst_buffer_get_size(buf));

        gst_sample_unref(sample);   /* releases buffer back (maybe to a pool) */
        frames++;
    }
    CHECK(gst_app_sink_is_eos(sink), "loop ended because of EOS, not timeout");

    gst_object_unref(out);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    printf("frames pulled: %d (expected %d)\n", frames, NUM_FRAMES);
    CHECK(frames == NUM_FRAMES, "frame count");
    printf("OK: pulled every frame, pixels and timestamps verified\n");
    gst_deinit();
    return 0;
}
