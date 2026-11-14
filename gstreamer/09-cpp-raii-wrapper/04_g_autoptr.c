/*
 * 04_g_autoptr.c — RAII in plain C: g_autoptr, g_auto, g_steal_pointer
 *
 * Concept : GCC and Clang support __attribute__((cleanup)): a function that
 *           runs when a variable goes out of scope. GLib wraps it as
 *             g_autoptr(GstElement) e = ...;   -> gst_object_unref(e) at '}'
 *             g_autofree gchar *s = ...;       -> g_free(s)
 *             g_autoptr(GError) err = NULL;    -> g_error_free(err)
 *           GStreamer registers cleanup functions for its types (GstElement,
 *           GstBus, GstMessage, GstCaps, GstPad, GstSample, ...).
 *           g_steal_pointer(&p) hands ownership out and leaves p = NULL, the
 *           C version of std::move / unique_ptr::release.
 *           Not standard C (works on GCC/Clang, which is what every embedded
 *           Linux toolchain uses, MSVC excluded).
 * Build   : make run P=gstreamer/09-cpp-raii-wrapper/04_g_autoptr.c
 *           ./build/gstreamer/09-cpp-raii-wrapper/04_g_autoptr --display
 * Read    : README.md  (section "C vs C++")
 *
 *   videotestsrc num-buffers=25 ─► videoconvert ─► fakesink (name=sink)
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

static gint frames;

static GstPadProbeReturn count_cb(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    (void)pad; (void)info; (void)u;
    g_atomic_int_inc(&frames);
    return GST_PAD_PROBE_OK;
}

/* Returns a pipeline the CALLER owns, or NULL. Every early return below is
 * leak-free without a single explicit unref/free. */
static GstElement *build(gboolean display)
{
    g_autoptr(GError) err = NULL;
    g_autofree gchar *desc = g_strdup_printf(
        "videotestsrc num-buffers=25 ! videoconvert ! %s name=sink",
        display ? "autovideosink" : "fakesink sync=false");

    g_autoptr(GstElement) pipeline = gst_parse_launch(desc, &err);
    if (!pipeline || err) {           /* pipeline (if partial) unreffed at '}' */
        fprintf(stderr, "parse: %s\n", err ? err->message : "?");
        return NULL;
    }

    g_autoptr(GstElement) sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!sink) return NULL;                         /* pipeline auto-unreffed */
    g_autoptr(GstPad) pad = gst_element_get_static_pad(sink, "sink");
    if (!pad) return NULL;
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, count_cb, NULL, NULL);

    /* Success: give our ref to the caller. Without g_steal_pointer the
     * cleanup at '}' would unref it and we would return a dangling pointer. */
    return g_steal_pointer(&pipeline);
    /* at '}' : pad, sink unreffed; desc freed; pipeline is NULL -> no-op */
}

static gboolean run(GstElement *pipeline)
{
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE)
        return FALSE;
    g_autoptr(GstBus) bus = gst_element_get_bus(pipeline);
    g_autoptr(GstMessage) msg = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    if (!msg) return FALSE;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        g_autoptr(GError) e = NULL;
        g_autofree gchar *dbg = NULL;
        gst_message_parse_error(msg, &e, &dbg);
        fprintf(stderr, "ERROR: %s\n", e->message);
        return FALSE;                 /* e, dbg, msg, bus all released */
    }
    return TRUE;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    gboolean ok;
    {
        g_autoptr(GstElement) pipeline = build(display);
        CHECK(pipeline != NULL, "build failed");
        CHECK(GST_OBJECT_REFCOUNT_VALUE(pipeline) == 1, "exactly our ref");
        ok = run(pipeline);
        /* The cleanup only unrefs; setting NULL first is still OUR job. */
        gst_element_set_state(pipeline, GST_STATE_NULL);
    }   /* pipeline unreffed here */

    printf("frames counted: %d\n", g_atomic_int_get(&frames));
    CHECK(ok, "pipeline did not reach EOS");
    CHECK(g_atomic_int_get(&frames) == 25, "frame count");
    printf("OK: no explicit unref/free in build() or run(), nothing leaked\n");
    gst_deinit();
    return 0;
}
