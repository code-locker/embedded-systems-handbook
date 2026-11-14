/*
 * 01_pick_best_element.c — choose the hardware encoder at RUNTIME, fall back
 *                          to software, and run the same app on PC and board
 *
 * Concept : The same binary must work on your PC (no video codec hardware) and on the
 *           BeagleBone AI-64 (TI SDK image with v4l2h264enc). Ask the plugin
 *           registry what exists (gst_element_factory_find), prefer hardware,
 *           and build the pipeline around what you got. We also list every
 *           video encoder in the registry, sorted by rank — the same ranking
 *           decodebin/encodebin use for autoplugging.
 * Build   : make run P=gstreamer/14-embedded-tda4vm/01_pick_best_element.c
 *           ./build/gstreamer/14-embedded-tda4vm/01_pick_best_element --display
 * Read    : README.md  (section "How it works: one binary, PC and board")
 *
 *   preference:  v4l2h264enc (HW codec, V4L2 M2M) > x264enc (CPU, ugly)
 *                > openh264enc (CPU, bad) > jpegenc (CPU, good: always here)
 *
 *   videotestsrc num-buffers=30 ─► videoconvert ─► [chosen encoder] ─► fakesink
 *                                    (gives the encoder its preferred      ▲
 *                                     format, e.g. NV12 for v4l2h264enc)    probe counts
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

typedef struct {
    const char *factory;   /* element name to look up               */
    const char *kind;      /* what it is, for the printout          */
    const char *extra;     /* properties that keep it fast/short    */
} Candidate;

static const Candidate candidates[] = {
    { "v4l2h264enc", "HW H.264 (V4L2 M2M driver of the SoC codec)", "" },
    { "x264enc",     "SW H.264 (gstreamer1.0-plugins-ugly)",
      "speed-preset=ultrafast tune=zerolatency" },
    { "openh264enc", "SW H.264 (gstreamer1.0-plugins-bad)", "" },
    { "jpegenc",     "SW JPEG  (gstreamer1.0-plugins-good)", "" },
};

static gint encoded;    /* written by the streaming thread only */

static GstPadProbeReturn count_buf(GstPad *pad, GstPadProbeInfo *info,
                                   gpointer data)
{
    (void)pad; (void)info; (void)data;
    encoded++;
    return GST_PAD_PROBE_OK;
}

/* Print all video encoders the registry knows, best rank first. */
static void list_video_encoders(void)
{
    /* transfer full: a GList of refs to factories */
    GList *list = gst_element_factory_list_get_elements(
        GST_ELEMENT_FACTORY_TYPE_ENCODER | GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO,
        GST_RANK_NONE);
    list = g_list_sort(list, gst_plugin_feature_rank_compare_func);
    printf("video encoders in this registry (rank):\n");
    int shown = 0;
    for (GList *l = list; l && shown < 12; l = l->next, shown++) {
        GstPluginFeature *f = l->data;
        printf("  %-20s %u\n", gst_plugin_feature_get_name(f),
               gst_plugin_feature_get_rank(f));
    }
    gst_plugin_feature_list_free(list);    /* unrefs every factory */
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    list_video_encoders();

    /* Note: jpegenc is classified "Codec/Encoder/Image", so the VIDEO
     * list above does not contain it. Klass strings matter for autoplug. */
    const Candidate *chosen = NULL;
    printf("candidates, in order of preference:\n");
    for (size_t i = 0; i < G_N_ELEMENTS(candidates); i++) {
        /* transfer full: a ref to the factory, or NULL if not installed */
        GstElementFactory *f = gst_element_factory_find(candidates[i].factory);
        printf("  %-12s %s\n", candidates[i].factory,
               f ? "available" : "not installed");
        if (f) {
            gst_object_unref(f);
            if (!chosen) chosen = &candidates[i];
        }
    }
    CHECK(chosen != NULL, "no encoder at all (install plugins-good)");
    printf("chosen: %s — %s\n", chosen->factory, chosen->kind);

    /* --display: decode JPEG back and show it (only meaningful for jpegenc;
     * for H.264 you would add h264parse ! a decoder). */
    gboolean show = display && strcmp(chosen->factory, "jpegenc") == 0;
    gchar *desc = g_strdup_printf(
        "videotestsrc num-buffers=30 ! "
        "video/x-raw,width=640,height=480,framerate=30/1 ! videoconvert ! "
        "%s name=enc %s ! %s",
        chosen->factory, chosen->extra,
        show ? "jpegdec ! videoconvert ! autovideosink"
             : "fakesink name=sink sync=false");
    printf("pipeline: %s\n", desc);

    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);   /* we own it */
    g_free(desc);
    CHECK(pipeline && !err, "parse failed");

    GstElement *enc = gst_bin_get_by_name(GST_BIN(pipeline), "enc"); /* +1 */
    GstPad *srcpad = gst_element_get_static_pad(enc, "src");         /* +1 */
    gst_pad_add_probe(srcpad, GST_PAD_PROBE_TYPE_BUFFER, count_buf, NULL, NULL);
    gst_object_unref(srcpad);
    gst_object_unref(enc);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND,
                          GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg != NULL, "timeout");
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *e = NULL;
        gst_message_parse_error(msg, &e, NULL);
        fprintf(stderr, "ERROR: %s\n", e->message);
        g_clear_error(&e);
    }
    gboolean eos = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    printf("encoded buffers out of %s: %d\n", chosen->factory, encoded);
    CHECK(eos, "pipeline must reach EOS");
    /* JPEG: 1 buffer per frame. H.264 encoders may hold a few frames
     * (lookahead) but flush them at EOS, so >= 1 is the safe check. */
    CHECK(encoded >= 1, "encoder produced nothing");
    if (strcmp(chosen->factory, "jpegenc") == 0)
        CHECK(encoded == 30, "jpegenc: one JPEG per frame");

    printf("OK: best available encoder picked at runtime and used\n");
    gst_deinit();
    return 0;
}
