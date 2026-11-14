/*
 * 03_states_and_preroll.c — NULL, READY, PAUSED, PLAYING, step by step
 *
 * Concept : going to PAUSED is ASYNCHRONOUS: data must flow until every sink
 *           holds one buffer (PREROLL). gst_element_set_state returns ASYNC,
 *           gst_element_get_state waits for completion. A LIVE source cannot
 *           preroll (nothing exists before "now"): PAUSED returns NO_PREROLL.
 * Build   : make run P=gstreamer/02-first-pipeline/03_states_and_preroll.c
 * Read    : README.md  (section "States and preroll")
 *
 *   NULL ─► READY ─► PAUSED ───────────────► PLAYING
 *                     │ ASYNC: thread starts, first buffer reaches sink,
 *                     │ sink BLOCKS holding it ("prerolled"), ASYNC_DONE
 *
 *   videotestsrc ─► fakesink   (sink's "last-sample" = the prerolled frame)
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

static const char *ret_name(GstStateChangeReturn r)
{
    return gst_element_state_change_return_get_name(r);
}

/* Change state, print what set_state returned, then wait for the final
 * result. Returns the set_state return value. */
static GstStateChangeReturn go(GstElement *p, GstState target)
{
    GstStateChangeReturn r = gst_element_set_state(p, target);
    GstState cur, pending;
    /* Blocks until the (async) change completes or 5 s pass. */
    GstStateChangeReturn done =
        gst_element_get_state(p, &cur, &pending, 5 * GST_SECOND);
    printf("  set_state(%-7s) -> %-10s  then get_state -> %-10s now=%s\n",
           gst_element_state_get_name(target), ret_name(r), ret_name(done),
           gst_element_state_get_name(cur));
    CHECK(done != GST_STATE_CHANGE_FAILURE, "state change failed");
    CHECK(cur == target, "reached target state");
    return r;
}

/* Does the sink hold a prerolled frame? "last-sample" is a GstSample
 * (transfer full) or NULL. */
static gboolean sink_has_frame(GstElement *pipeline)
{
    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    GstSample *sample = NULL;
    g_object_get(sink, "last-sample", &sample, NULL);
    gboolean has = sample != NULL;
    if (sample)
        gst_sample_unref(sample);
    gst_object_unref(sink);
    return has;
}

static void run(gboolean live)
{
    printf("== %s source ==\n", live ? "LIVE" : "non-live");
    gchar *desc = g_strdup_printf(
        "videotestsrc is-live=%s ! video/x-raw,width=160,height=120"
        " ! fakesink name=sink", live ? "true" : "false");
    GstElement *p = gst_parse_launch(desc, NULL);
    g_free(desc);
    CHECK(p, "parse");

    go(p, GST_STATE_READY);              /* sync: just opens resources */
    CHECK(!sink_has_frame(p), "READY: no data yet");

    GstStateChangeReturn r = go(p, GST_STATE_PAUSED);
    if (!live) {
        /* Non-live: ASYNC, then prerolled: the sink holds frame #0
         * although the clock is not running. A video sink would show it. */
        CHECK(r == GST_STATE_CHANGE_ASYNC, "PAUSED is async");
        CHECK(sink_has_frame(p), "PAUSED: sink prerolled one frame");
    } else {
        /* Live: NO_PREROLL. A camera does not produce while paused. */
        CHECK(r == GST_STATE_CHANGE_NO_PREROLL, "live -> NO_PREROLL");
        CHECK(!sink_has_frame(p), "live: nothing prerolled");
    }

    go(p, GST_STATE_PLAYING);
    g_usleep(200 * 1000);                     /* let a few frames render */
    CHECK(sink_has_frame(p), "PLAYING: frames rendered");

    go(p, GST_STATE_NULL);                    /* always end in NULL */
    gst_object_unref(p);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    run(FALSE);
    run(TRUE);
    printf("OK: preroll (ASYNC) and live (NO_PREROLL) behaviour verified\n");
    gst_deinit();
    return 0;
}
