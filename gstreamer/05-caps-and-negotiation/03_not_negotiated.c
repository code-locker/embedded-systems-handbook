/*
 * 03_not_negotiated.c — make negotiation FAIL on purpose and recognise the error
 *
 * Concept : When no format is acceptable to both sides of a link, the
 *           source's streaming thread gets GST_FLOW_NOT_NEGOTIATED and posts
 *           an ERROR "Internal data stream error ... not-negotiated (-4)".
 *           This is the most common GStreamer error. Here we provoke it with
 *           two contradictory capsfilters, catch it on the bus, and show the
 *           fix (insert videoconvert). Note: the second filter's caps are set
 *           AFTER linking; if they were in the launch string, parse_launch
 *           would already refuse the link and you would get "not-linked"
 *           instead (link-time vs run-time caps checks). The program SUCCEEDS if the error is
 *           seen in case A and the fixed pipeline runs in case B.
 * Build   : make run P=gstreamer/05-caps-and-negotiation/03_not_negotiated.c
 * Read    : README.md  (section "7. Common mistakes and pitfalls")
 *
 *   A (broken):  videotestsrc ! video/x-raw,format=RGB  ! video/x-raw,format=GRAY8 ! fakesink
 *                                     RGB ∩ GRAY8 = ∅   ──►  ERROR not-negotiated
 *   B (fixed) :  videotestsrc ! video/x-raw,format=RGB ! videoconvert ! video/x-raw,format=GRAY8 ! fakesink
 *                                            videoconvert bridges RGB -> GRAY8
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

/* Runs desc; returns the final message type (EOS or ERROR). Prints the
 * error text and debug string (the debug string is where "not-negotiated"
 * is spelled out). */
static GstMessageType run(const char *desc, const char *late_caps,
                          gboolean *saw_not_negotiated)
{
    printf("\n$ %s   (then f2 caps=%s)\n", desc, late_caps);
    GError *perr = NULL;
    GstElement *p = gst_parse_launch(desc, &perr);
    CHECK(p != NULL && perr == NULL, "parse + link");

    /* Set the 2nd filter's caps only now, after the link succeeded: the
     * conflict is discovered when data flows and the CAPS event is
     * refused -> GST_FLOW_NOT_NEGOTIATED in the streaming thread. */
    GstElement *f2 = gst_bin_get_by_name(GST_BIN(p), "f2");   /* own */
    GstCaps *c = gst_caps_from_string(late_caps);
    g_object_set(f2, "caps", c, NULL);
    gst_caps_unref(c);
    gst_object_unref(f2);

    gst_element_set_state(p, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(p);
    GstMessage *m = gst_bus_timed_pop_filtered(
        bus, 5 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(m != NULL, "pipeline finished");
    GstMessageType t = GST_MESSAGE_TYPE(m);
    if (t == GST_MESSAGE_ERROR) {
        GError *e = NULL;
        gchar *dbg = NULL;
        gst_message_parse_error(m, &e, &dbg);
        printf("ERROR from %s: %s\n  debug: %s\n", GST_OBJECT_NAME(m->src),
               e->message, dbg ? dbg : "(none)");
        /* Robust check: the error domain/code, plus the flow name in dbg. */
        if (dbg && strstr(dbg, "not-negotiated"))
            *saw_not_negotiated = TRUE;
        g_clear_error(&e);
        g_free(dbg);
    } else {
        printf("EOS\n");
    }
    gst_message_unref(m);
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    return t;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    gboolean nn = FALSE;
    GstMessageType a = run(
        "videotestsrc num-buffers=10 ! video/x-raw,format=RGB ! "
        "capsfilter name=f2 ! fakesink sync=false",
        "video/x-raw,format=GRAY8", &nn);
    CHECK(a == GST_MESSAGE_ERROR, "case A must fail");
    CHECK(nn, "case A failure is 'not-negotiated'");

    gboolean nn2 = FALSE;
    GstMessageType b = run(
        "videotestsrc num-buffers=10 ! video/x-raw,format=RGB ! videoconvert ! "
        "capsfilter name=f2 ! fakesink sync=false",
        "video/x-raw,format=GRAY8", &nn2);
    CHECK(b == GST_MESSAGE_EOS && !nn2, "case B (with videoconvert) works");

    printf("\nOK: not-negotiated reproduced, recognised and fixed\n");
    gst_deinit();
    return 0;
}
