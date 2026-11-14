/*
 * 04_tee_add_remove_branch.c — add and remove a tee branch while PLAYING
 *
 * Concept : The "start recording / stop recording" pattern. The main branch
 *           runs all the time. At frame 20 we request a new src pad from the
 *           tee and attach queue ! fakesink to it (sync_state_with_parent
 *           brings the new elements to PLAYING). At frame 50 we install an
 *           IDLE probe on that tee pad: when the tee is between two pushes
 *           we unlink the branch; the app thread then sets the branch
 *           elements to NULL, removes them and releases the request pad.
 * Build   : make run P=gstreamer/08-queues-threads-tee/04_tee_add_remove_branch.c
 *           ./build/gstreamer/08-queues-threads-tee/04_tee_add_remove_branch --display
 * Read    : README.md  (section "Adding and removing branches at runtime")
 *
 *  videotestsrc(live) ─► tee ─[src_0]─► queue ─► main sink        (always)
 *                            └[src_1]─► queue ─► fakesink "rec"   (frames ~20..50)
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

#define N_FRAMES 75
#define ADD_AT 20
#define REMOVE_AT 50

typedef struct {
    GstElement *queue, *sink;  /* our own refs (ref_sink'ed), released at end */
    GstPad *teepad;            /* request pad: our ref from request_pad_simple */
    gint unlinked;             /* atomic: set by the IDLE probe */
    gint frames;               /* atomic: frames that reached this branch */
} Branch;

static GstPadProbeReturn count_cb(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    (void)pad; (void)info;
    g_atomic_int_inc((gint *)u);
    return GST_PAD_PROBE_OK;
}

static void count_frames(GstElement *e, gint *counter)
{
    GstPad *p = gst_element_get_static_pad(e, "sink");        /* +1 */
    gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, count_cb, counter, NULL);
    gst_object_unref(p);
}

static void add_branch(GstElement *pipeline, GstElement *tee, Branch *br)
{
    /* Create with factory_make (floating), ref_sink so WE own a ref that
     * survives gst_bin_remove later; the bin adds its own ref. */
    br->queue = gst_object_ref_sink(gst_element_factory_make("queue", "rec_q"));
    br->sink = gst_object_ref_sink(gst_element_factory_make("fakesink", "rec"));
    g_object_set(br->sink, "sync", FALSE, NULL);
    count_frames(br->sink, &br->frames);

    gst_bin_add_many(GST_BIN(pipeline), br->queue, br->sink, NULL);
    CHECK(gst_element_link(br->queue, br->sink), "link queue->sink");
    /* Bring them up BEFORE linking to the tee: data may arrive immediately
     * and must not hit an element still in NULL (-> FLUSHING error). */
    gst_element_sync_state_with_parent(br->sink);
    gst_element_sync_state_with_parent(br->queue);

    /* Request pads are created on demand from the "src_%u" template. */
    br->teepad = gst_element_request_pad_simple(tee, "src_%u");   /* +1 */
    GstPad *qsink = gst_element_get_static_pad(br->queue, "sink"); /* +1 */
    CHECK(gst_pad_link(br->teepad, qsink) == GST_PAD_LINK_OK, "tee link");
    gst_object_unref(qsink);
    printf("  [app] branch added on %s\n", GST_OBJECT_NAME(br->teepad));
}

/* Runs in the tee's (source's) streaming thread when the pad is idle:
 * no buffer is being pushed through it right now, and none will be until
 * we return. Safe moment to unlink. */
static GstPadProbeReturn unlink_cb(GstPad *teepad, GstPadProbeInfo *info, gpointer u)
{
    (void)info;
    Branch *br = u;
    GstPad *qsink = gst_element_get_static_pad(br->queue, "sink"); /* +1 */
    gst_pad_unlink(teepad, qsink);
    /* Tell the branch its stream is over (a muxer would finalise its file
     * here). It is now unlinked, so this EOS does not reach the main sink. */
    gst_pad_send_event(qsink, gst_event_new_eos());
    gst_object_unref(qsink);
    g_atomic_int_set(&br->unlinked, 1);
    printf("  [idle probe] branch unlinked from %s\n", GST_OBJECT_NAME(teepad));
    return GST_PAD_PROBE_REMOVE;
}

static void remove_branch(GstElement *pipeline, GstElement *tee, Branch *br)
{
    /* App thread: the branch is no longer linked, so we may stop it. NULL
     * joins the branch queue's thread. Never do this inside the probe. */
    gst_element_set_state(br->queue, GST_STATE_NULL);
    gst_element_set_state(br->sink, GST_STATE_NULL);
    gst_bin_remove_many(GST_BIN(pipeline), br->queue, br->sink, NULL);
    gst_element_release_request_pad(tee, br->teepad);
    gst_object_unref(br->teepad);      /* our ref from request_pad_simple */
    br->teepad = NULL;
    printf("  [app] branch removed, request pad released\n");
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    gchar *desc = g_strdup_printf(
        "videotestsrc is-live=true num-buffers=%d"
        " ! video/x-raw,width=320,height=240,framerate=60/1"
        " ! tee name=t allow-not-linked=true"
        " t. ! queue ! videoconvert ! %s",
        N_FRAMES, display ? "autovideosink name=main" : "fakesink name=main sync=false");
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);           /* owned */
    g_free(desc);
    CHECK(pipeline != NULL, err ? err->message : "parse failed");

    GstElement *tee = gst_bin_get_by_name(GST_BIN(pipeline), "t");     /* +1 */
    GstElement *mainsink = gst_bin_get_by_name(GST_BIN(pipeline), "main"); /* +1 */
    gint main_frames = 0;
    count_frames(mainsink, &main_frames);
    gst_object_unref(mainsink);

    Branch br = { 0 };
    int state = 0;               /* 0 = not added, 1 = added, 2 = removing, 3 = removed */
    int frames_at_removal = -1;

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);                       /* +1 */
    gboolean eos = FALSE;
    while (!eos) {
        /* Poll the bus every 10 ms; between polls drive our state machine. */
        GstMessage *msg = gst_bus_timed_pop_filtered(
            bus, 10 * GST_MSECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
        if (msg) {
            CHECK(GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "pipeline error");
            gst_message_unref(msg);
            eos = TRUE;
        }
        int n = g_atomic_int_get(&main_frames);
        if (state == 0 && n >= ADD_AT) {
            add_branch(pipeline, tee, &br);
            state = 1;
        } else if (state == 1 && n >= REMOVE_AT) {
            gst_pad_add_probe(br.teepad, GST_PAD_PROBE_TYPE_IDLE, unlink_cb, &br, NULL);
            state = 2;
        } else if (state == 2 && g_atomic_int_get(&br.unlinked)) {
            frames_at_removal = g_atomic_int_get(&br.frames);
            remove_branch(pipeline, tee, &br);
            state = 3;
        }
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(tee);
    gst_object_unref(pipeline);

    int rec = g_atomic_int_get(&br.frames);
    printf("main branch: %d frames, recording branch: %d frames\n",
           main_frames, rec);
    CHECK(state == 3, "branch was not added and removed");
    CHECK(main_frames == N_FRAMES, "main branch must not lose frames");
    CHECK(rec > 0 && rec < N_FRAMES, "recording branch got a slice");
    CHECK(rec == frames_at_removal, "no frames after removal");
    gst_object_unref(br.queue);   /* our refs: elements finalized now */
    gst_object_unref(br.sink);
    printf("OK: branch added and removed live, main branch untouched\n");
    gst_deinit();
    return 0;
}
