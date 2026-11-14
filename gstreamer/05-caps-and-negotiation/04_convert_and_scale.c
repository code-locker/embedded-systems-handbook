/*
 * 04_convert_and_scale.c — convert + scale to exactly what you need, check bytes and strides
 *
 * Concept : videoconvert changes the pixel FORMAT, videoscale changes the
 *           SIZE; the capsfilter after them says what you want and both
 *           negotiate to produce it. In appsink we check the buffer size and
 *           the row stride with GstVideoInfo / GstVideoFrame: rows are often
 *           padded (stride >= width * bytes_per_pixel), a classic bug source.
 * Build   : make run P=gstreamer/05-caps-and-negotiation/04_convert_and_scale.c
 *           ./build/gstreamer/05-caps-and-negotiation/04_convert_and_scale --display
 * Read    : README.md  (section "Strides and frame size")
 *
 *   videotestsrc (I420 640x480) ─► videoconvert ─► videoscale ─► capsfilter ─► appsink
 *                                  I420->GRAY8      640x480->     GRAY8 158x120
 *                                                   158x120       (158: forces padding!)
 *
 *   GRAY8 row of 158 bytes is padded to a stride of 160 (4-byte aligned):
 *   ┌──────────── 158 pixels ────────────┬pad┐
 *   │ Y Y Y Y ...                        Y│ x x│  <- stride = 160 bytes
 *   └────────────────────────────────────┴───┘
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

#define W 158
#define H 120
#define NUM_FRAMES 20

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    if (display) {
        GstElement *p = gst_parse_launch(
            "videotestsrc num-buffers=100 ! videoconvert ! videoscale ! "
            "video/x-raw,format=GRAY8,width=158,height=120 ! videoconvert ! "
            "autovideosink", NULL);
        CHECK(p != NULL, "parse");
        gst_element_set_state(p, GST_STATE_PLAYING);
        GstBus *bus = gst_element_get_bus(p);
        GstMessage *m = gst_bus_timed_pop_filtered(bus, 30 * GST_SECOND,
                            GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
        if (m)
            gst_message_unref(m);
        gst_object_unref(bus);
        gst_element_set_state(p, GST_STATE_NULL);
        gst_object_unref(p);
        gst_deinit();
        return 0;
    }

    /* pattern=white: every luma byte becomes 235 (video range white),
     * a known value we can assert after conversion. */
    gchar *desc = g_strdup_printf(
        "videotestsrc num-buffers=%d pattern=white ! "
        "video/x-raw,format=I420,width=640,height=480 ! "
        "videoconvert ! videoscale ! "
        "video/x-raw,format=GRAY8,width=%d,height=%d ! "
        "appsink name=out sync=false", NUM_FRAMES, W, H);
    GstElement *pipeline = gst_parse_launch(desc, NULL);
    g_free(desc);
    CHECK(pipeline != NULL, "parse");

    /* gst_bin_get_by_name: transfer full -> unref when done. */
    GstElement *out = gst_bin_get_by_name(GST_BIN(pipeline), "out");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    int frames = 0;
    GstSample *sample;
    while ((sample = gst_app_sink_try_pull_sample(GST_APP_SINK(out),
                                                  5 * GST_SECOND))) {
        GstVideoInfo info;
        CHECK(gst_video_info_from_caps(&info, gst_sample_get_caps(sample)),
              "video info");
        GstBuffer *buf = gst_sample_get_buffer(sample);   /* borrowed */

        /* GstVideoFrame maps the buffer AND knows strides/planes. Prefer it
         * over gst_buffer_map for video. Unmap before releasing the sample. */
        GstVideoFrame frame;
        CHECK(gst_video_frame_map(&frame, &info, buf, GST_MAP_READ), "map");
        gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
        const guint8 *y = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
        if (frames == 0) {
            printf("GRAY8 %dx%d: stride=%d, frame size=%zu, buffer=%zu bytes\n",
                   GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
                   stride, GST_VIDEO_INFO_SIZE(&info), gst_buffer_get_size(buf));
            CHECK(GST_VIDEO_INFO_WIDTH(&info) == W, "width");
            CHECK(GST_VIDEO_INFO_HEIGHT(&info) == H, "height");
            CHECK(stride == GST_ROUND_UP_4(W), "stride rounded up to 4");
            CHECK(stride > W, "with W=158 the rows ARE padded");
            CHECK(gst_buffer_get_size(buf) == (gsize)stride * H,
                  "buffer = stride * height, NOT width * height");
        }
        /* Walk pixels row by row USING THE STRIDE. Using W here instead
         * would drift 2 bytes per row and read padding garbage. */
        for (int r = 0; r < H; r++)
            for (int c = 0; c < W; c++)
                CHECK(y[r * stride + c] == 235, "white luma = 235");
        gst_video_frame_unmap(&frame);
        gst_sample_unref(sample);
        frames++;
    }
    CHECK(gst_app_sink_is_eos(GST_APP_SINK(out)), "EOS");
    gst_object_unref(out);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    printf("frames: %d (expected %d)\n", frames, NUM_FRAMES);
    CHECK(frames == NUM_FRAMES, "frame count");
    printf("OK: converted + scaled to GRAY8 %dx%d, strides handled\n", W, H);
    gst_deinit();
    return 0;
}
