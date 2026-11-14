/*
 * 01_bus_message_tour.c — every message a simple pipeline posts, in order
 *
 * Concept : the bus is a thread-safe queue of GstMessage from elements to
 *           the application. Popping ALL types (not just EOS/ERROR) shows
 *           the life of a pipeline: state changes of every element,
 *           stream-start, async-done (preroll finished), latency, EOS.
 * Build   : make run P=gstreamer/03-bus-and-messages/01_bus_message_tour.c
 * Read    : README.md  (section "The bus" and "Message catalogue")
 *
 *   streaming threads ──post──► [ bus queue ] ──pop──► main thread (here)
 *   videotestsrc ─► fakesink        msg, msg, msg ...
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

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    GstElement *p = gst_parse_launch(
        "videotestsrc name=src num-buffers=5 ! fakesink name=sink sync=false",
        NULL);
    CHECK(p, "parse");
    GstBus *bus = gst_element_get_bus(p);                 /* full ref */
    gst_element_set_state(p, GST_STATE_PLAYING);

    int order = 0, at_stream_start = -1, at_async_done = -1, at_eos = -1;
    int pipeline_state_changes = 0;
    GstState final_state = GST_STATE_VOID_PENDING;

    for (;;) {
        /* GST_MESSAGE_ANY: take everything. Timeout so a bug cannot hang. */
        GstMessage *m = gst_bus_timed_pop(bus, 5 * GST_SECOND);  /* full */
        CHECK(m != NULL, "timed out before EOS");
        order++;
        const char *from = GST_OBJECT_NAME(GST_MESSAGE_SRC(m)); /* none */

        switch (GST_MESSAGE_TYPE(m)) {
        case GST_MESSAGE_STATE_CHANGED: {
            GstState old, cur, pend;
            gst_message_parse_state_changed(m, &old, &cur, &pend);
            printf("%2d %-14s from %-10s %s -> %s\n", order, "STATE_CHANGED",
                   from, gst_element_state_get_name(old),
                   gst_element_state_get_name(cur));
            /* Every element posts these; usually you only care about the
             * pipeline's own (the top-level). */
            if (GST_MESSAGE_SRC(m) == GST_OBJECT(p)) {
                pipeline_state_changes++;
                final_state = cur;
            }
            break;
        }
        case GST_MESSAGE_STREAM_START:
            at_stream_start = order;
            printf("%2d %-14s from %s\n", order, "STREAM_START", from);
            break;
        case GST_MESSAGE_ASYNC_DONE:  /* preroll finished: PAUSED reached */
            at_async_done = order;
            printf("%2d %-14s from %s\n", order, "ASYNC_DONE", from);
            break;
        case GST_MESSAGE_EOS:
            at_eos = order;
            printf("%2d %-14s from %s\n", order, "EOS", from);
            break;
        case GST_MESSAGE_ERROR: {
            GError *e = NULL;
            gst_message_parse_error(m, &e, NULL);
            fprintf(stderr, "ERROR from %s: %s\n", from, e->message);
            g_error_free(e);
            exit(1);
        }
        default:   /* NEW_CLOCK, LATENCY, STREAM_STATUS, TAG, QOS ... */
            printf("%2d %-14s from %s\n", order, GST_MESSAGE_TYPE_NAME(m),
                   from);
            break;
        }
        gboolean done = GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
        gst_message_unref(m);
        if (done)
            break;
    }

    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);

    /* The pipeline went NULL->READY->PAUSED->PLAYING: 3 changes. */
    CHECK(pipeline_state_changes == 3, "3 pipeline state changes");
    CHECK(final_state == GST_STATE_PLAYING, "pipeline reached PLAYING");
    CHECK(at_stream_start > 0 && at_async_done > at_stream_start,
          "stream-start before async-done");
    CHECK(at_eos > at_async_done, "EOS last");
    printf("OK: message order verified (STREAM_START < ASYNC_DONE < EOS)\n");
    gst_deinit();
    return 0;
}
