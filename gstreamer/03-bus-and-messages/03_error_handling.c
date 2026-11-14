/*
 * 03_error_handling.c — what errors look like and how to classify them
 *
 * Concept : errors arrive as GST_MESSAGE_ERROR with a GError (domain +
 *           code + message) and a debug string for developers. Some fail
 *           the state change immediately (FAILURE), others happen later
 *           while streaming. Classify by domain/code, not by string.
 * Build   : make run P=gstreamer/03-bus-and-messages/03_error_handling.c
 * Read    : README.md  (section "Errors and warnings")
 *
 *   case A: filesrc location=/nonexistent ─► fakesink
 *           READY fails: RESOURCE error, NOT_FOUND / OPEN_READ
 *   case B: videotestsrc ─► capsfilter caps=audio/x-raw ─► fakesink
 *           nothing can satisfy the filter ─► streaming error from the
 *           source: "not-negotiated" (STREAM domain)
 *   case C: element posts a WARNING but keeps running
 */
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond, msg)                                                   \
    do {                                                                   \
        if (!(cond)) {                                                     \
            fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", msg, __FILE__,   \
                    __LINE__);                                             \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

/* Run a pipeline until EOS/ERROR/WARNING; return the first such message
 * (transfer full) and the set_state result through *ret. */
static GstMessage *run_until(const char *desc, GstStateChangeReturn *ret,
                             GstMessageType types)
{
    GError *perr = NULL;
    GstElement *p = gst_parse_launch(desc, &perr);
    CHECK(p && !perr, "parse");
    /* If there is a capsfilter named "f", give it impossible caps now. */
    GstElement *f = gst_bin_get_by_name(GST_BIN(p), "f");   /* full or NULL */
    if (f) {
        GstCaps *bad = gst_caps_from_string("audio/x-raw");
        g_object_set(f, "caps", bad, NULL);          /* filter refs it */
        gst_caps_unref(bad);
        gst_object_unref(f);
    }
    *ret = gst_element_set_state(p, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND, types);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);    /* the message keeps its own ref on msg->src */
    return m;
}

static void print_error(GstMessage *m, GError *e, const gchar *dbg)
{
    printf("  %s from '%s'\n    domain=%s code=%d\n    message: %s\n"
           "    debug  : %.110s...\n", GST_MESSAGE_TYPE_NAME(m),
           GST_OBJECT_NAME(GST_MESSAGE_SRC(m)), g_quark_to_string(e->domain),
           e->code, e->message, dbg ? dbg : "(none)");
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    GstStateChangeReturn ret;
    const GstMessageType stop = GST_MESSAGE_EOS | GST_MESSAGE_ERROR;

    /* ---- A: resource error during NULL->READY ---------------------- */
    printf("A) missing file\n");
    GstMessage *m = run_until(
        "filesrc location=/nonexistent/file.mkv ! fakesink", &ret, stop);
    CHECK(ret == GST_STATE_CHANGE_FAILURE, "set_state fails immediately");
    CHECK(m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR, "error message");
    GError *e = NULL;
    gchar *dbg = NULL;
    gst_message_parse_error(m, &e, &dbg);           /* both transfer full */
    print_error(m, e, dbg);
    CHECK(e->domain == GST_RESOURCE_ERROR, "RESOURCE domain");
    CHECK(e->code == GST_RESOURCE_ERROR_NOT_FOUND, "NOT_FOUND");
    g_clear_error(&e);
    g_free(dbg);
    gst_message_unref(m);

    /* ---- B: negotiation error while streaming ---------------------- *
     * Ask for audio caps after a video source. If we wrote the caps in
     * the description, gst_parse_launch would refuse to LINK (template
     * check) and we would get a parse error instead. Setting the caps
     * after linking reproduces the real-life case: the link exists, the
     * failure appears only when data starts to flow. */
    printf("B) impossible caps -> not-negotiated\n");
    m = run_until("videotestsrc num-buffers=10 ! capsfilter name=f "
                  "! fakesink", &ret, stop);
    CHECK(ret != GST_STATE_CHANGE_FAILURE, "state change itself is fine");
    CHECK(m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR, "streaming error");
    gst_message_parse_error(m, &e, &dbg);
    print_error(m, e, dbg);
    /* Source elements report flow errors as STREAM domain; the debug
     * string carries the flow return ("not-negotiated"). */
    CHECK(e->domain == GST_STREAM_ERROR || e->domain == GST_CORE_ERROR,
          "STREAM/CORE domain");
    CHECK(dbg && g_strrstr(dbg, "not-negotiated"), "reason: not-negotiated");
    g_clear_error(&e);
    g_free(dbg);
    gst_message_unref(m);

    /* ---- C: a WARNING does not stop the pipeline -------------------- *
     * We post one ourselves with GST_ELEMENT_WARNING-like API from the
     * outside: gst_message_new_warning. The pipeline keeps running. */
    printf("C) warning is not fatal\n");
    GstElement *p = gst_parse_launch(
        "videotestsrc name=src num-buffers=20 ! fakesink sync=false", NULL);
    GstBus *bus = gst_element_get_bus(p);
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstElement *src = gst_bin_get_by_name(GST_BIN(p), "src");
    GError *w = g_error_new_literal(GST_STREAM_ERROR, GST_STREAM_ERROR_DECODE,
                                    "corrupt frame skipped (simulated)");
    gst_element_post_message(src, gst_message_new_warning(GST_OBJECT(src), w,
                             "simulated by 03_error_handling.c"));
    g_error_free(w);                  /* new_warning copied the GError */
    gst_object_unref(src);
    gboolean saw_warning = FALSE, saw_eos = FALSE;
    while (!saw_eos) {
        m = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
                                       stop | GST_MESSAGE_WARNING);
        CHECK(m != NULL, "timeout");
        if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_WARNING) {
            gst_message_parse_warning(m, &e, &dbg);
            print_error(m, e, dbg);
            g_clear_error(&e);
            g_free(dbg);
            saw_warning = TRUE;
        }
        CHECK(GST_MESSAGE_TYPE(m) != GST_MESSAGE_ERROR, "no error");
        saw_eos = GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
        gst_message_unref(m);
    }
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    CHECK(saw_warning, "warning seen and pipeline still reached EOS");

    printf("OK: resource error, negotiation error and warning classified\n");
    gst_deinit();
    return 0;
}
