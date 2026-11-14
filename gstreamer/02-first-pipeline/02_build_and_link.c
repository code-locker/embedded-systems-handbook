/*
 * 02_build_and_link.c — build a pipeline element by element (no parser)
 *
 * Concept : gst_element_factory_make + gst_bin_add_many + gst_element_link*
 *           is what gst_parse_launch does for you. Doing it by hand gives
 *           you a pointer to every element, an error check for every link,
 *           and caps filters via gst_element_link_filtered.
 * Build   : make run P=gstreamer/02-first-pipeline/02_build_and_link.c
 *           ./build/gstreamer/02-first-pipeline/02_build_and_link --display
 * Read    : README.md  (section "Building by hand")
 *
 *  ┌──────────── pipeline ──────────────────────────────────────────────┐
 *  │ videotestsrc ─► [320x240 I420] ─► videoconvert ─► videoflip ─► sink │
 *  │   "src"          (link_filtered)    "conv"          "flip"          │
 *  └─────────────────────────────────────────────────────────────────────┘
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

/* Make an element or die with a helpful message. Returns a FLOATING ref. */
static GstElement *make(const char *factory, const char *name)
{
    GstElement *e = gst_element_factory_make(factory, name);
    if (!e) {
        fprintf(stderr, "element '%s' not found: install the plugin that "
                        "provides it (gst-inspect-1.0 %s)\n", factory, factory);
        exit(1);
    }
    return e;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    /* 1. Create. All four are floating refs: nobody owns them yet. */
    GstElement *pipeline = gst_pipeline_new("my-pipeline");
    GstElement *src  = make("videotestsrc", "src");
    GstElement *conv = make("videoconvert", "conv");
    GstElement *flip = make("videoflip", "flip");
    GstElement *sink = make(display ? "autovideosink" : "fakesink", "sink");

    /* 2. Configure with properties (NULL-terminated list!). */
    g_object_set(src, "num-buffers", 50, "pattern", 18 /* ball */, NULL);
    gst_util_set_object_arg(G_OBJECT(flip), "method", "horizontal-flip");
    if (!display)
        g_object_set(sink, "sync", FALSE, NULL);

    /* 3. Add to the pipeline: it sinks the floating refs and now OWNS the
     *    elements. Our pointers are borrowed from here on. */
    gst_bin_add_many(GST_BIN(pipeline), src, conv, flip, sink, NULL);

    /* 4. Link. Linking checks that the PAD TEMPLATES are compatible; the
     *    real format is negotiated later, when data starts to flow.
     *    link_filtered inserts a capsfilter between src and conv. */
    GstCaps *caps = gst_caps_from_string(
        "video/x-raw,format=I420,width=320,height=240,framerate=30/1");
    gboolean linked = gst_element_link_filtered(src, conv, caps);
    gst_caps_unref(caps);     /* link_filtered took its own ref */
    CHECK(linked, "src -> conv");
    CHECK(gst_element_link_many(conv, flip, sink, NULL), "conv->flip->sink");

    /* A link that MUST fail: a sink has no src pad. Good to know that
     * gst_element_link returns FALSE instead of crashing. */
    CHECK(!gst_element_link(sink, src), "sink cannot link to anything");

    /* 5. Run and wait, like program 01. */
    CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING)
              != GST_STATE_CHANGE_FAILURE, "PLAYING");
    GstBus *bus = gst_element_get_bus(pipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg != NULL, "timeout");
    gboolean eos = GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;

    /* 6. Read the caps that were really negotiated on flip's sink pad. */
    GstPad *pad = gst_element_get_static_pad(flip, "sink");   /* full */
    GstCaps *cur = gst_pad_get_current_caps(pad);             /* full */
    CHECK(cur != NULL, "negotiated caps present");
    gchar *s = gst_caps_to_string(cur);
    printf("negotiated on flip.sink: %s\n", s);
    GstStructure *st = gst_caps_get_structure(cur, 0);        /* none */
    gint w = 0, h = 0;
    gst_structure_get_int(st, "width", &w);
    gst_structure_get_int(st, "height", &h);
    g_free(s);
    gst_caps_unref(cur);
    gst_object_unref(pad);

    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);   /* frees src, conv, flip, sink too */

    CHECK(eos, "reached EOS");
    CHECK(w == 320 && h == 240, "caps filter forced 320x240");
    printf("OK: hand-built pipeline ran, 320x240 negotiated\n");
    gst_deinit();
    return 0;
}
