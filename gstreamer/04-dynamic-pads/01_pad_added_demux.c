/*
 * 01_pad_added_demux.c — link a demuxer's "sometimes" pad in a pad-added callback
 *
 * Concept : A demuxer does not know which streams a file contains until it
 *           has read the header, so its src pads appear at run time. You
 *           cannot link them in main(); you connect to "pad-added" and link
 *           there, from the demuxer's streaming thread.
 * Build   : make run P=gstreamer/04-dynamic-pads/01_pad_added_demux.c
 *           ./build/gstreamer/04-dynamic-pads/01_pad_added_demux --display
 * Read    : README.md  (section "3. How it works: pad-added step by step")
 *
 *  Phase 1 (make a test file, 30 frames of MJPEG in Matroska):
 *    videotestsrc num-buffers=30 ─► jpegenc ─► matroskamux ─► filesink
 *
 *  Phase 2 (play it back, the lesson):
 *    filesrc ─► matroskademux  ┄┄(no src pad yet!)┄┄   jpegdec ─► videoconvert ─► fakesink
 *                    │
 *                    └── reads header, finds a video track, creates "video_0"
 *                        and emits "pad-added"  ──► on_pad_added() links
 *                        video_0 ─► jpegdec:sink
 */
#include <gst/gst.h>
#include <glib/gstdio.h>   /* g_remove */
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

#define NUM_FRAMES 30

/* Run a pipeline until EOS/ERROR. Returns TRUE on EOS. Takes no ownership. */
static gboolean run_until_eos(GstElement *pipeline)
{
    CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "set_state(PLAYING)");
    GstBus *bus = gst_element_get_bus(pipeline);           /* we own a ref */
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    gboolean ok = msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
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

/* Count buffers reaching fakesink. Called from the STREAMING thread, so the
 * counter is atomic (main thread reads it later). */
static void on_handoff(GstElement *sink, GstBuffer *buf, GstPad *pad,
                       gpointer user_data)
{
    (void)sink; (void)buf; (void)pad;
    g_atomic_int_inc((gint *)user_data);
}

/* The heart of the lesson. Runs in the demuxer's streaming thread, NOT in
 * main(). new_pad is transfer-none: we borrow it, we must not unref it. */
static void on_pad_added(GstElement *demux, GstPad *new_pad, gpointer user_data)
{
    GstElement *decoder = GST_ELEMENT(user_data);
    printf("pad-added: %s:%s\n", GST_ELEMENT_NAME(demux), GST_PAD_NAME(new_pad));

    /* A real file may have audio + video + subtitles: look at the caps and
     * only link what we can handle. current_caps may be NULL very early;
     * query_caps always answers. Both return a ref we own. */
    GstCaps *caps = gst_pad_get_current_caps(new_pad);
    if (!caps)
        caps = gst_pad_query_caps(new_pad, NULL);
    const gchar *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
    printf("  caps: %s\n", name);

    if (g_str_has_prefix(name, "image/jpeg")) {
        GstPad *sinkpad = gst_element_get_static_pad(decoder, "sink"); /* own */
        if (gst_pad_is_linked(sinkpad)) {
            printf("  decoder already linked, ignoring\n");
        } else {
            GstPadLinkReturn r = gst_pad_link(new_pad, sinkpad);
            printf("  link -> %s\n", gst_pad_link_get_name(r));
        }
        gst_object_unref(sinkpad);
    } else {
        printf("  not video we decode, leaving it unlinked\n");
    }
    gst_caps_unref(caps);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    /* ---- Phase 1: create a small MJPEG-in-Matroska file ---------------- */
    gchar *path = g_build_filename(g_get_tmp_dir(), "gst_lesson04_demux.mkv",
                                   NULL);                 /* we own: g_free */
    gchar *desc = g_strdup_printf(
        "videotestsrc num-buffers=%d ! video/x-raw,width=320,height=240 ! "
        "jpegenc ! matroskamux ! filesink location=\"%s\"", NUM_FRAMES, path);
    GError *err = NULL;
    GstElement *writer = gst_parse_launch(desc, &err);
    CHECK(writer != NULL, "parse writer pipeline");
    CHECK(run_until_eos(writer), "writing the test file");
    gst_object_unref(writer);
    g_free(desc);
    printf("wrote %s\n", path);

    /* ---- Phase 2: build the playback pipeline by hand ------------------ */
    /* gst_element_factory_make returns a FLOATING ref. gst_bin_add sinks it:
     * the bin becomes the owner, so we never unref these elements. */
    GstElement *pipeline = gst_pipeline_new("player");
    GstElement *src   = gst_element_factory_make("filesrc", "src");
    GstElement *demux = gst_element_factory_make("matroskademux", "demux");
    GstElement *dec   = gst_element_factory_make("jpegdec", "dec");
    GstElement *conv  = gst_element_factory_make("videoconvert", "conv");
    GstElement *sink  = gst_element_factory_make(
        display ? "autovideosink" : "fakesink", "sink");
    CHECK(pipeline && src && demux && dec && conv && sink, "make elements");

    g_object_set(src, "location", path, NULL);
    gint frames = 0;
    if (!display) {
        g_object_set(sink, "sync", FALSE, "signal-handoffs", TRUE, NULL);
        g_signal_connect(sink, "handoff", G_CALLBACK(on_handoff), &frames);
    }

    gst_bin_add_many(GST_BIN(pipeline), src, demux, dec, conv, sink, NULL);

    /* Static links: these pads exist now (ALWAYS pads). */
    CHECK(gst_element_link(src, demux), "link filesrc -> demux");
    CHECK(gst_element_link_many(dec, conv, sink, NULL), "link dec->conv->sink");

    /* demux -> dec CANNOT be linked yet: demux has no src pad.
     *   gst_element_link(demux, dec) here would fail (or, with
     *   gst_element_link it would try "delayed linking" - avoid relying on
     *   that magic; be explicit). */
    g_signal_connect(demux, "pad-added", G_CALLBACK(on_pad_added), dec);

    gboolean ok = run_until_eos(pipeline);
    gst_object_unref(pipeline);

    g_remove(path);
    g_free(path);

    CHECK(ok, "playback reached EOS");
    if (!display) {
        printf("frames decoded: %d (expected %d)\n", frames, NUM_FRAMES);
        CHECK(frames == NUM_FRAMES, "every frame went through the dynamic link");
    }
    printf("OK: sometimes-pad linked in pad-added, all frames played\n");
    gst_deinit();
    return 0;
}
