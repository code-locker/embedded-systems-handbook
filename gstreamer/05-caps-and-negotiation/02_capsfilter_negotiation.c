/*
 * 02_capsfilter_negotiation.c — force a format with capsfilter, read back what was agreed
 *
 * Concept : videotestsrc can produce dozens of formats and any size. A
 *           capsfilter narrows the choice; negotiation then fixes ONE format
 *           that travels downstream in a CAPS event. We read the negotiated
 *           caps from the sink pad, parse them with GstVideoInfo and check
 *           them. We also watch the pad's "notify::caps" signal fire.
 * Build   : make run P=gstreamer/05-caps-and-negotiation/02_capsfilter_negotiation.c
 *           ./build/gstreamer/05-caps-and-negotiation/02_capsfilter_negotiation --display
 * Read    : README.md  (section "3. How it works": the negotiation sequence)
 *
 *   videotestsrc ──[src]──► capsfilter ──[src]──► fakesink[sink]  <- read caps here
 *                 caps="video/x-raw,format=NV12,width=640,height=360,framerate=15/1"
 */
#include <gst/gst.h>
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

/* Fires when the pad's caps change (after the CAPS event passed). Runs in
 * the streaming thread. */
static void on_caps_notify(GObject *pad, GParamSpec *pspec, gpointer data)
{
    (void)pspec;
    GstCaps *caps = gst_pad_get_current_caps(GST_PAD(pad)); /* own or NULL */
    if (caps) {
        gchar *s = gst_caps_to_string(caps);
        printf("notify::caps -> %s\n", s);
        g_free(s);
        gst_caps_unref(caps);
        g_atomic_int_inc((gint *)data);
    }
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    GstElement *pipeline = gst_pipeline_new("p");
    GstElement *src  = gst_element_factory_make("videotestsrc", "src");
    GstElement *filt = gst_element_factory_make("capsfilter", "filt");
    GstElement *conv = gst_element_factory_make("videoconvert", "conv");
    GstElement *sink = gst_element_factory_make(
        display ? "autovideosink" : "fakesink", "sink");
    CHECK(pipeline && src && filt && conv && sink, "make elements");

    g_object_set(src, "num-buffers", 30, NULL);
    /* The "caps" property takes a ref of its own, so we unref ours. */
    GstCaps *want = gst_caps_from_string(
        "video/x-raw,format=NV12,width=640,height=360,framerate=15/1");
    g_object_set(filt, "caps", want, NULL);
    gst_caps_unref(want);
    if (!display)
        g_object_set(sink, "sync", FALSE, NULL);

    /* videoconvert is a no-op passthrough here (sink accepts anything) but
     * needed for --display, where the window may not take NV12. */
    gst_bin_add_many(GST_BIN(pipeline), src, filt, conv, sink, NULL);
    CHECK(gst_element_link_many(src, filt, conv, sink, NULL), "link");

    /* Shortcut you will see a lot: gst_element_link_filtered(a, b, caps)
     * inserts exactly such a capsfilter for you. */

    GstPad *filt_src = gst_element_get_static_pad(filt, "src");    /* own */
    gint notified = 0;
    g_signal_connect(filt_src, "notify::caps", G_CALLBACK(on_caps_notify),
                     &notified);

    /* PAUSED: negotiation + preroll happen before PLAYING. After a
     * successful (blocking) get_state, caps are set on every link. */
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    CHECK(gst_element_get_state(pipeline, NULL, NULL, 5 * GST_SECOND) ==
              GST_STATE_CHANGE_SUCCESS, "prerolled");

    GstCaps *neg = gst_pad_get_current_caps(filt_src);   /* own */
    CHECK(neg != NULL, "caps negotiated on capsfilter src");
    CHECK(gst_caps_is_fixed(neg), "negotiated caps are always fixed");

    /* GstVideoInfo: the right way to turn video caps into numbers -
     * it knows strides, plane offsets and frame size for every format. */
    GstVideoInfo info;
    CHECK(gst_video_info_from_caps(&info, neg), "parse video info");
    printf("format=%s %dx%d @ %d/%d fps, frame size %zu bytes, %u planes\n",
           GST_VIDEO_INFO_NAME(&info), GST_VIDEO_INFO_WIDTH(&info),
           GST_VIDEO_INFO_HEIGHT(&info), GST_VIDEO_INFO_FPS_N(&info),
           GST_VIDEO_INFO_FPS_D(&info), GST_VIDEO_INFO_SIZE(&info),
           GST_VIDEO_INFO_N_PLANES(&info));
    CHECK(GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_NV12, "NV12");
    CHECK(GST_VIDEO_INFO_WIDTH(&info) == 640, "width");
    CHECK(GST_VIDEO_INFO_HEIGHT(&info) == 360, "height");
    CHECK(GST_VIDEO_INFO_FPS_N(&info) == 15 && GST_VIDEO_INFO_FPS_D(&info) == 1,
          "framerate");
    /* NV12 = 12 bits per pixel: Y plane w*h + interleaved UV plane w*h/2. */
    CHECK(GST_VIDEO_INFO_SIZE(&info) == 640 * 360 * 3 / 2, "NV12 frame size");
    CHECK(GST_VIDEO_INFO_N_PLANES(&info) == 2, "NV12 has 2 planes");
    gst_caps_unref(neg);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(filt_src);
    gst_object_unref(pipeline);

    CHECK(g_atomic_int_get(&notified) >= 1, "notify::caps fired");
    printf("OK: capsfilter forced NV12 640x360@15 and we verified it\n");
    gst_deinit();
    return 0;
}
