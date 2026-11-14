/*
 * 02_camera_or_test.c — one capture program for PC tests and the real camera
 *
 * Concept : Default: videotestsrc (headless, deterministic, CI-friendly).
 *           With --camera [/dev/videoN]: v4l2src from a USB UVC or CSI camera
 *           (on the BeagleBone AI-64 e.g. a UVC webcam, or an IMX219 behind
 *           TI's ISP in the TI SDK image). The downstream part is identical,
 *           so everything you test on the PC is what runs on the board. We
 *           print the negotiated caps — the first thing to check on new
 *           hardware — and assert frames arrive.
 * Build   : make run P=gstreamer/14-embedded-tda4vm/02_camera_or_test.c
 *           ./build/gstreamer/14-embedded-tda4vm/02_camera_or_test --camera /dev/video0
 *           ./build/gstreamer/14-embedded-tda4vm/02_camera_or_test --display
 * Read    : README.md  (section "How it works: camera capture with v4l2src")
 *
 *   source (videotestsrc | v4l2src device=/dev/videoN)
 *     ─► capsfilter video/x-raw,width=640,height=480   (ask, don't convert)
 *     ─► queue leaky=downstream max-size-buffers=2     (live: newest frame wins)
 *     ─► videoconvert ─► video/x-raw,format=GRAY8      (stand-in for "my algorithm's format")
 *     ─► appsink max-buffers=2 drop=true               (your C code gets frames)
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

#define WANT_FRAMES 30

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    gboolean camera = FALSE, display = FALSE;
    const char *device = "/dev/video0";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--camera") == 0) {
            camera = TRUE;
            if (i + 1 < argc && argv[i + 1][0] != '-') device = argv[++i];
        } else if (strcmp(argv[i], "--display") == 0) {
            display = TRUE;
        }
    }

    /* A camera is live: it produces frames in real time whether we keep
     * up or not, so we use a leaky queue and a dropping appsink. The test
     * source gets num-buffers so it ends on its own; is-live=true makes
     * it behave like a camera (timestamps from the clock, no preroll). */
    gchar *source = camera
        ? g_strdup_printf("v4l2src device=%s", device)
        : g_strdup_printf("videotestsrc is-live=true num-buffers=%d "
                          "pattern=ball", WANT_FRAMES + 10);
    gchar *desc = g_strdup_printf(
        "%s ! video/x-raw,width=640,height=480 ! "
        "queue leaky=downstream max-size-buffers=2 ! videoconvert ! "
        "tee name=t "
        "t. ! queue ! video/x-raw,format=GRAY8 ! "
        "appsink name=sink max-buffers=2 drop=true sync=false "
        "%s",
        source,
        display ? "t. ! queue ! videoconvert ! autovideosink" : "");
    g_free(source);
    printf("pipeline: %s\n", desc);

    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);   /* we own it */
    g_free(desc);
    if (!pipeline || err) {
        fprintf(stderr, "parse failed: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        if (pipeline) gst_object_unref(pipeline);
        return 1;
    }
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink"); /* +1 */

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING)
            == GST_STATE_CHANGE_FAILURE) {
        /* Typical with --camera: no such device, or busy (another app). */
        fprintf(stderr, "cannot start (camera %s missing or busy?)\n", device);
        gst_object_unref(sink);
        gst_object_unref(pipeline);
        return camera ? 1 : 2;
    }

    int frames = 0;
    gboolean caps_printed = FALSE;
    while (frames < WANT_FRAMES) {
        /* try_pull_sample with a timeout: a dead camera must not hang us */
        GstSample *s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                                    5 * GST_SECOND);  /* +1 */
        if (!s) break;     /* EOS or timeout */
        if (!caps_printed) {
            GstCaps *caps = gst_sample_get_caps(s);      /* transfer none */
            gchar *str = gst_caps_to_string(caps);
            printf("negotiated at appsink: %s\n", str);
            g_free(str);
            GstVideoInfo vi;
            CHECK(gst_video_info_from_caps(&vi, caps), "video info");
            CHECK(GST_VIDEO_INFO_FORMAT(&vi) == GST_VIDEO_FORMAT_GRAY8, "GRAY8");
            CHECK(GST_VIDEO_INFO_WIDTH(&vi) == 640, "width 640");
            CHECK(GST_VIDEO_INFO_HEIGHT(&vi) == 480, "height 480");
            caps_printed = TRUE;
        }
        GstBuffer *b = gst_sample_get_buffer(s);         /* transfer none */
        CHECK(GST_BUFFER_PTS_IS_VALID(b), "live frames carry timestamps");
        frames++;
        gst_sample_unref(s);
    }

    /* Also show what the SOURCE produced (before videoconvert): on a real
     * camera this is the driver's native format, e.g. YUY2 or MJPG. */
    GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline),
                          camera ? "v4l2src0" : "videotestsrc0");   /* +1 */
    if (src) {
        GstPad *pad = gst_element_get_static_pad(src, "src");       /* +1 */
        GstCaps *c = gst_pad_get_current_caps(pad);                 /* +1 */
        if (c) {
            gchar *str = gst_caps_to_string(c);
            printf("source native caps   : %s\n", str);
            g_free(str);
            gst_caps_unref(c);
        }
        gst_object_unref(pad);
        gst_object_unref(src);
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(pipeline);

    printf("received %d frames from %s\n", frames,
           camera ? device : "videotestsrc");
    CHECK(frames == WANT_FRAMES, "not enough frames (camera stalled?)");
    printf("OK: capture path works; same code runs with --camera on the board\n");
    gst_deinit();
    return 0;
}
