/*
 * 01_hello_pipeline.c — the smallest real GStreamer program
 *
 * Concept : gst_init -> build a pipeline from a text description
 *           (gst_parse_launch) -> set PLAYING -> wait on the bus for EOS or
 *           ERROR -> set NULL -> unref. Every GStreamer app has this skeleton.
 * Build   : make run P=gstreamer/02-first-pipeline/01_hello_pipeline.c
 *           ./build/gstreamer/02-first-pipeline/01_hello_pipeline --display
 * Read    : README.md  (section "How it works: the 6-step skeleton")
 *
 *   videotestsrc num-buffers=60 ──► videoconvert ──► fakesink
 *   (makes 60 frames, then EOS)      (format glue)    (swallows frames)
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

int main(int argc, char *argv[])
{
    /* Step 1: initialise the library. Parses --gst-* options out of argv,
     * loads the plugin registry (cached in ~/.cache/gstreamer-1.0). */
    gst_init(&argc, &argv);

    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    /* Step 2: describe the pipeline. Same syntax as gst-launch-1.0.
     * num-buffers=60 makes the source send EOS after 60 frames, so the
     * program ends on its own (headless, no Ctrl-C needed).
     * fakesink sync=false: consume frames as fast as possible; with
     * --display we use a real window and sync to the clock (30 fps). */
    const char *desc = display
        ? "videotestsrc num-buffers=60 ! videoconvert ! autovideosink"
        : "videotestsrc num-buffers=60 ! videoconvert ! fakesink sync=false";

    GError *err = NULL;
    /* OWNERSHIP: gst_parse_launch returns a new GstPipeline (documented as
     * "transfer floating"). Nobody else holds it, so it is ours: one
     * gst_object_unref() at the end frees it and all its children. */
    GstElement *pipeline = gst_parse_launch(desc, &err);
    if (!pipeline) {
        fprintf(stderr, "parse error: %s\n", err ? err->message : "?");
        g_clear_error(&err);
        return 1;
    }
    printf("pipeline: %s\n", desc);

    /* Step 3: start it. PLAYING makes the source's streaming thread run. */
    GstStateChangeReturn ret =
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    CHECK(ret != GST_STATE_CHANGE_FAILURE, "set_state(PLAYING) failed");

    /* Step 4: wait. The bus carries messages from the streaming threads
     * to us (the application thread). Block until EOS or ERROR. */
    GstBus *bus = gst_element_get_bus(pipeline);          /* we own a ref */
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);

    CHECK(msg != NULL, "timed out waiting for EOS");
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *e = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(msg, &e, &dbg);
        fprintf(stderr, "ERROR from %s: %s\n%s\n",
                GST_OBJECT_NAME(msg->src), e->message, dbg ? dbg : "");
        g_clear_error(&e);
        g_free(dbg);
    }
    gboolean ok = (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS);
    printf("got %s\n", GST_MESSAGE_TYPE_NAME(msg));

    /* Step 5: stop. NULL frees device/thread resources. ALWAYS do this
     * before the final unref, or elements are destroyed while running. */
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);

    /* Step 6: release our reference; the pipeline frees its children. */
    gst_object_unref(pipeline);

    CHECK(ok, "pipeline did not reach EOS");
    printf("OK: 60 frames flowed source -> sink and EOS arrived\n");
    gst_deinit();
    return 0;
}
