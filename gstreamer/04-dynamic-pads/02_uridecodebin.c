/*
 * 02_uridecodebin.c — let uridecodebin autoplug the decoders, link raw video out
 *
 * Concept : uridecodebin = source + typefind + demuxer + parser + decoder,
 *           chosen automatically from the registry by caps and rank. You only
 *           see its output pads, which appear dynamically (pad-added) and
 *           carry RAW media (video/x-raw, audio/x-raw). You check the caps
 *           name and link video to your branch. We pull frames with appsink.
 * Build   : make run P=gstreamer/04-dynamic-pads/02_uridecodebin.c
 *           ./build/gstreamer/04-dynamic-pads/02_uridecodebin --display
 * Read    : README.md  (section "decodebin / uridecodebin / playbin")
 *
 *   ┌──────────────── uridecodebin (a bin) ─────────────────┐
 *   │ filesrc ─► typefind ─► matroskademux ─► jpegdec ─►[src_0]┼┄┄► videoconvert ─► appsink
 *   └────────────────────────────────────────────────────────┘    (linked in pad-added)
 */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <glib/gstdio.h>
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

#define NUM_FRAMES 25

static gboolean run_until_eos(GstElement *pipeline)
{
    CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "set_state(PLAYING)");
    GstBus *bus = gst_element_get_bus(pipeline);           /* we own a ref */
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    gboolean ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg && !ok) {
        GError *e = NULL;
        gst_message_parse_error(msg, &e, NULL);
        fprintf(stderr, "ERROR from %s: %s\n", GST_OBJECT_NAME(msg->src),
                e->message);
        g_clear_error(&e);
    }
    if (msg)
        gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    return ok;
}

typedef struct {
    GstElement *convert;   /* borrowed: owned by the pipeline */
    gint linked_video;     /* how many video pads we linked   */
} App;

/* Streaming-thread callback. new_pad: transfer none (borrowed). */
static void on_pad_added(GstElement *dbin, GstPad *new_pad, gpointer data)
{
    App *app = data;
    (void)dbin;
    GstCaps *caps = gst_pad_get_current_caps(new_pad);     /* own or NULL */
    if (!caps)
        caps = gst_pad_query_caps(new_pad, NULL);
    GstStructure *s = gst_caps_get_structure(caps, 0);      /* borrowed */
    const gchar *name = gst_structure_get_name(s);
    printf("pad-added %s with %s\n", GST_PAD_NAME(new_pad), name);

    /* decodebin output is always RAW (or it would not have finished
     * plugging). Audio would be audio/x-raw: we'd link it to an audio
     * branch or ignore it - an unlinked pad that receives data gives
     * "not-linked" error, which is why playbin exists for real players. */
    if (g_str_equal(name, "video/x-raw")) {
        GstPad *sink = gst_element_get_static_pad(app->convert, "sink");
        if (!gst_pad_is_linked(sink) &&
            gst_pad_link(new_pad, sink) == GST_PAD_LINK_OK)
            g_atomic_int_inc(&app->linked_video);
        gst_object_unref(sink);
    }
    gst_caps_unref(caps);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    /* Make an input file (MJPEG in Matroska) so the lesson is self-contained. */
    gchar *path = g_build_filename(g_get_tmp_dir(), "gst_lesson04_udb.mkv", NULL);
    gchar *desc = g_strdup_printf(
        "videotestsrc num-buffers=%d pattern=ball ! "
        "video/x-raw,width=320,height=240,framerate=25/1 ! jpegenc ! "
        "matroskamux ! filesink location=\"%s\"", NUM_FRAMES, path);
    GstElement *writer = gst_parse_launch(desc, NULL);
    CHECK(writer && run_until_eos(writer), "write test file");
    gst_object_unref(writer);
    g_free(desc);

    /* URIs, not paths: uridecodebin picks the source element by scheme
     * (file:// -> filesrc, http:// -> souphttpsrc, rtsp:// -> rtspsrc). */
    gchar *uri = gst_filename_to_uri(path, NULL);          /* own: g_free */
    printf("uri: %s\n", uri);

    GstElement *pipeline = gst_pipeline_new("p");
    GstElement *dbin = gst_element_factory_make("uridecodebin", "dbin");
    GstElement *conv = gst_element_factory_make("videoconvert", "conv");
    GstElement *sink = gst_element_factory_make(
        display ? "autovideosink" : "appsink", "sink");
    CHECK(pipeline && dbin && conv && sink, "make elements");
    g_object_set(dbin, "uri", uri, NULL);
    if (!display) {
        /* appsink: sync=false = as fast as possible; we pull later. */
        g_object_set(sink, "sync", FALSE, NULL);
        GstCaps *want = gst_caps_from_string("video/x-raw,format=RGB");
        gst_app_sink_set_caps(GST_APP_SINK(sink), want);  /* copies a ref */
        gst_caps_unref(want);
    }
    gst_bin_add_many(GST_BIN(pipeline), dbin, conv, sink, NULL); /* sinks floating refs */
    CHECK(gst_element_link(conv, sink), "link conv -> sink");

    App app = { conv, 0 };
    g_signal_connect(dbin, "pad-added", G_CALLBACK(on_pad_added), &app);

    gint frames = 0;
    gboolean ok;
    if (display) {
        ok = run_until_eos(pipeline);
    } else {
        /* Pull-mode consumer: main thread blocks on the appsink. */
        CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
                  GST_STATE_CHANGE_FAILURE, "PLAYING");
        GstSample *sample;
        while ((sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                                      5 * GST_SECOND))) {
            if (frames == 0) {
                GstCaps *c = gst_sample_get_caps(sample);  /* borrowed */
                gchar *str = gst_caps_to_string(c);
                printf("first frame caps: %s\n", str);
                g_free(str);
                GstStructure *s = gst_caps_get_structure(c, 0);
                gint w = 0, h = 0;
                gst_structure_get_int(s, "width", &w);
                gst_structure_get_int(s, "height", &h);
                CHECK(w == 320 && h == 240, "decoded size 320x240");
                CHECK(g_str_equal(gst_structure_get_string(s, "format"), "RGB"),
                      "format RGB");
            }
            frames++;
            gst_sample_unref(sample);                      /* we owned it */
        }
        ok = gst_app_sink_is_eos(GST_APP_SINK(sink));
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    gst_object_unref(pipeline);
    g_remove(path);
    g_free(path);
    g_free(uri);

    CHECK(ok, "reached EOS");
    CHECK(app.linked_video == 1, "exactly one video pad linked");
    if (!display) {
        printf("frames: %d (expected %d)\n", frames, NUM_FRAMES);
        CHECK(frames == NUM_FRAMES, "all frames decoded");
    }
    printf("OK: uridecodebin autoplugged demux+decoder, we linked raw video\n");
    gst_deinit();
    return 0;
}
