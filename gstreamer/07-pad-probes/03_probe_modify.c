/*
 * 03_probe_modify.c — change the pixels of a buffer inside a probe
 *
 * Concept : A probe receives a buffer it does NOT own exclusively. To write
 *           into it you must call gst_buffer_make_writable() (copies only if
 *           somebody else also holds a ref), then put the possibly-new buffer
 *           back into info->data. Then map it with gst_video_frame_map() so
 *           strides/planes are right. We stamp a white 16x16 square in the
 *           top-left of every black GRAY8 frame and check it in appsink.
 * Build   : make run P=gstreamer/07-pad-probes/03_probe_modify.c
 *           ./build/gstreamer/07-pad-probes/03_probe_modify --display
 * Read    : README.md  (section "Modifying buffers")
 *
 *   videotestsrc pattern=black ──► capsfilter(GRAY8 160x120) [src] ──► tee
 *                                                ▲                 │
 *                              probe: make_writable + paint        ├─► queue ─► appsink (check)
 *                                                                  └─► queue ─► videoconvert ─► autovideosink
 *                                                                      (only with --display)
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

#define N_FRAMES 20
#define SQ 16

static GstPadProbeReturn paint_square(GstPad *pad, GstPadProbeInfo *info,
                                      gpointer user_data)
{
    (void)user_data;
    /* Caps tell us the layout. Query them per call for simplicity; a real
     * element would cache the GstVideoInfo on the CAPS event. */
    GstCaps *caps = gst_pad_get_current_caps(pad);           /* +1 ref */
    GstVideoInfo vinfo;
    gboolean ok = caps && gst_video_info_from_caps(&vinfo, caps);
    if (caps) gst_caps_unref(caps);
    if (!ok) return GST_PAD_PROBE_OK;

    /* OWNERSHIP: info->data holds one ref. make_writable() consumes that ref
     * and returns a writable buffer (the same one if we were the only
     * owner, otherwise a copy). We MUST store the result back. */
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    buf = gst_buffer_make_writable(buf);
    GST_PAD_PROBE_INFO_DATA(info) = buf;

    GstVideoFrame frame;
    if (gst_video_frame_map(&frame, &vinfo, buf, GST_MAP_WRITE)) {
        guint8 *pix = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
        int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
        for (int y = 0; y < SQ; y++)          /* O(SQ^2) per frame */
            memset(pix + y * stride, 0xFF, SQ);
        gst_video_frame_unmap(&frame);
    }
    return GST_PAD_PROBE_OK;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    gchar *desc = g_strdup_printf(
        "videotestsrc num-buffers=%d pattern=black"
        " ! capsfilter name=cf caps=video/x-raw,format=GRAY8,width=160,height=120"
        " ! tee name=t"
        " t. ! queue ! appsink name=out sync=false"
        "%s",
        N_FRAMES,
        display ? " t. ! queue ! videoconvert ! autovideosink" : "");
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);     /* we own it */
    g_free(desc);
    CHECK(pipeline != NULL, err ? err->message : "parse failed");

    GstElement *cf = gst_bin_get_by_name(GST_BIN(pipeline), "cf");   /* +1 */
    GstPad *src = gst_element_get_static_pad(cf, "src");             /* +1 */
    gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, paint_square, NULL, NULL);
    gst_object_unref(src);
    gst_object_unref(cf);

    GstElement *out = gst_bin_get_by_name(GST_BIN(pipeline), "out"); /* +1 */
    CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "PLAYING failed");

    int frames = 0;
    for (;;) {
        /* Blocks until a sample or EOS; returns NULL on EOS. We own it. */
        GstSample *s = gst_app_sink_pull_sample(GST_APP_SINK(out));
        if (!s) break;
        GstBuffer *b = gst_sample_get_buffer(s);            /* borrowed */
        GstMapInfo m;
        CHECK(gst_buffer_map(b, &m, GST_MAP_READ), "map failed");
        /* GRAY8 160 wide: stride is 160 (already 4-aligned). */
        CHECK(m.data[0] == 0xFF, "top-left pixel not painted");
        CHECK(m.data[(SQ - 1) * 160 + SQ - 1] == 0xFF, "square incomplete");
        /* videotestsrc "black" is video-range black (luma 16), not 0. */
        CHECK(m.data[60 * 160 + 80] <= 0x10, "centre should stay black");
        gst_buffer_unmap(b, &m);
        gst_sample_unref(s);
        frames++;
    }
    gst_object_unref(out);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    printf("checked %d frames\n", frames);
    CHECK(frames == N_FRAMES, "wrong frame count");
    printf("OK: every frame has a white %dx%d square painted by the probe\n",
           SQ, SQ);
    gst_deinit();
    return 0;
}
