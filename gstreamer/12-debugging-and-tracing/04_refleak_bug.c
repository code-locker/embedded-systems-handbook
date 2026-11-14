/*
 * 04_refleak_bug.c — INTENTIONAL reference leaks, to be found with the
 *                    leaks tracer (this file is skipped by `make test-gst`)
 *
 * Concept : GStreamer objects are refcounted; ASan's leak checker is off for
 *           GStreamer programs (GLib keeps global memory), so how do you find
 *           a forgotten gst_object_unref()? The `leaks` tracer records every
 *           GstObject/GstMiniObject alive and lists the survivors at
 *           gst_deinit(), with an optional creation stack trace.
 * Bug     : two leaks below (marked BUG 1 / BUG 2).
 * Find it : build, then run
 *             GST_TRACERS=leaks GST_DEBUG=GST_TRACER:7 \
 *               ./build/gstreamer/12-debugging-and-tracing/04_refleak_bug
 *           look for "object-alive" lines: GstPad 'src' and a GstCaps.
 *           Stack traces of where they were created:
 *             GST_TRACERS="leaks(stack-traces-flags=full)" GST_DEBUG=GST_TRACER:7 ...
 *           Filter to one type:  GST_TRACERS="leaks(filters=GstPad)"
 * Build   : make run P=gstreamer/12-debugging-and-tracing/04_refleak_bug.c
 * Read    : README.md  (section "Finding reference leaks")
 *
 *   refcount of the src pad:
 *     owned by element ............ 1
 *     gst_element_get_static_pad .. 2   <- we must unref -> back to 1
 *     element destroyed ........... 1   <- BUG: never reaches 0 => leak
 */
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);

    GstElement *src = gst_element_factory_make("videotestsrc", NULL);
    if (!src)
        return 1;
    gst_object_ref_sink(src);            /* we own it (sink the floating ref) */

    /* BUG 1: get_static_pad returns a NEW reference (transfer full),
     * and we never gst_object_unref(pad). */
    GstPad *pad = gst_element_get_static_pad(src, "src");
    printf("pad refcount after get_static_pad: %d\n",
           GST_OBJECT_REFCOUNT_VALUE(pad));

    /* BUG 2: we create caps (we own the only ref) and never
     * gst_caps_unref() them. Typical real-life version: forgetting to unref
     * the result of gst_pad_query_caps() / gst_pad_get_current_caps(). */
    GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                        "width", G_TYPE_INT, 320, NULL);
    printf("caps refcount: %d\n", GST_CAPS_REFCOUNT_VALUE(caps));

    gst_object_unref(src);               /* element is gone, pad is not */

    /* FIX:  gst_caps_unref(caps); gst_object_unref(pad);  (before src unref) */
    printf("leaks tracer will now report the pad and the caps as alive\n");
    gst_deinit();
    return 0;
}
