/*
 * 02_dot_dump.c — draw your running pipeline as a Graphviz picture
 *
 * Concept : If GST_DEBUG_DUMP_DOT_DIR is set (before gst_init!), then
 *           GST_DEBUG_BIN_TO_DOT_FILE() writes <dir>/<name>.dot describing
 *           every element, pad, link and the NEGOTIATED caps on each link.
 *           gst-launch-1.0 does it automatically at every state change.
 *           Convert with:  dot -Tpng file.dot -o file.png   (apt: graphviz)
 * Build   : make run P=gstreamer/12-debugging-and-tracing/02_dot_dump.c
 * Read    : README.md  (section "How it works: pipeline graphs")
 *
 *   videotestsrc ─► capsfilter ─► tee ─┬─► queue ─► fakesink (sinkA)
 *                                      └─► queue ─► fakesink (sinkB)
 *
 *   the .dot file is that picture in text form:
 *     subgraph cluster_videotestsrc0 { ... }  "src_0x.." -> "sink_0x.." [label="video/x-raw ..."]
 */
#include <gst/gst.h>
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

/* Read a whole .dot file; caller g_free()s the result. */
static gchar *slurp(const gchar *dir, const gchar *name)
{
    gchar *path = g_build_filename(dir, name, NULL);
    gchar *text = NULL;
    gboolean ok = g_file_get_contents(path, &text, NULL, NULL);
    printf("  %s  %s\n", ok ? "found  " : "MISSING", path);
    g_free(path);
    return ok ? text : NULL;
}

int main(int argc, char *argv[])
{
    /* 1. Make a private temp directory and point the env var at it.
     *    MUST happen before gst_init(): GStreamer reads it once at init. */
    GError *err = NULL;
    gchar *dir = g_dir_make_tmp("gst-dot-XXXXXX", &err);   /* we g_free */
    CHECK(dir != NULL, "cannot create temp dir");
    g_setenv("GST_DEBUG_DUMP_DOT_DIR", dir, TRUE);

    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    const char *desc = display
        ? "videotestsrc num-buffers=90 ! video/x-raw,width=320,height=240 "
          "! tee name=t t. ! queue ! videoconvert ! autovideosink name=sinkA "
          "t. ! queue ! fakesink name=sinkB"
        : "videotestsrc num-buffers=30 ! video/x-raw,width=320,height=240 "
          "! tee name=t t. ! queue ! fakesink name=sinkA sync=false "
          "t. ! queue ! fakesink name=sinkB sync=false";
    GstElement *pipeline = gst_parse_launch(desc, &err);   /* we own it */
    CHECK(pipeline && !err, "parse failed");
    gst_element_set_name(pipeline, "lesson12");

    /* 2. Dump at PAUSED: prerolled, so caps are negotiated on every link. */
    gst_element_set_state(pipeline, GST_STATE_PAUSED);
    CHECK(gst_element_get_state(pipeline, NULL, NULL, 5 * GST_SECOND)
              == GST_STATE_CHANGE_SUCCESS, "preroll failed");
    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline), GST_DEBUG_GRAPH_SHOW_ALL,
                              "lesson12-paused");

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    /* Same name twice would overwrite the file. In real apps use
     * GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(), which prefixes the running time
     * (e.g. "0.00.00.123456789-name.dot"), so every dump is kept. */
    GST_DEBUG_BIN_TO_DOT_FILE(GST_BIN(pipeline),
                              GST_DEBUG_GRAPH_SHOW_CAPS_DETAILS,
                              "lesson12-playing");

    GstBus *bus = gst_element_get_bus(pipeline);           /* we own a ref */
    GstMessage *msg = gst_bus_timed_pop_filtered(bus, 20 * GST_SECOND,
                          GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "no EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    /* 3. Check the files exist and describe our pipeline. */
    printf("dot files in %s:\n", dir);
    gchar *paused  = slurp(dir, "lesson12-paused.dot");
    gchar *playing = slurp(dir, "lesson12-playing.dot");
    CHECK(paused && playing, "dot files not written");
    CHECK(strstr(paused, "digraph") != NULL, "not a graphviz file");
    CHECK(strstr(paused, "videotestsrc0") != NULL, "source missing");
    CHECK(strstr(paused, "sinkA") && strstr(paused, "sinkB"), "sinks missing");
    CHECK(strstr(paused, "320") != NULL, "negotiated caps (width) missing");
    printf("  paused graph: %zu bytes, mentions videotestsrc0, tee, sinkA, "
           "sinkB, width 320\n", strlen(paused));
    g_free(paused);
    g_free(playing);

    /* 4. Clean up the temp dir (keep it with KEEP_DOT=1 to render it). */
    if (!g_getenv("KEEP_DOT")) {
        GDir *d = g_dir_open(dir, 0, NULL);
        const gchar *n;
        while (d && (n = g_dir_read_name(d)) != NULL) {
            gchar *p = g_build_filename(dir, n, NULL);
            g_unlink(p);
            g_free(p);
        }
        if (d) g_dir_close(d);
        g_rmdir(dir);
    } else {
        printf("kept: dot -Tpng %s/lesson12-paused.dot -o graph.png\n", dir);
    }
    g_free(dir);

    printf("OK: pipeline graph dumped with negotiated caps\n");
    gst_deinit();
    return 0;
}
