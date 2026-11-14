/*
 * 04_probe_block_swap.c — replace an element while the pipeline is PLAYING
 *
 * Concept : The classic "dynamic pipeline" recipe:
 *             1. BLOCK the pad that feeds the element (data stops there,
 *                the streaming thread waits inside our callback),
 *             2. push EOS into the old element so it drains what it holds,
 *             3. when EOS comes out of the old element: drop that EOS,
 *                set old to NULL, remove it, add + link the new one,
 *                sync its state with the parent,
 *             4. return from the blocking callback: data flows again,
 *                into the new element. Sticky events (stream-start, caps,
 *                segment) are re-sent to the new element automatically.
 *           Old element: identity (frame unchanged).
 *           New element: videoflip method=vertical-flip.
 *           An earlier probe paints a white square in the TOP-left corner,
 *           so appsink can tell "before" (square top) from "after" (bottom).
 * Build   : make run P=gstreamer/07-pad-probes/04_probe_block_swap.c
 *           ./build/gstreamer/07-pad-probes/04_probe_block_swap --display
 * Read    : README.md  (section "Blocking probes: changing a running pipeline")
 *
 *  videotestsrc(live) ─► capsfilter ─► queue ─[src]─► identity ─► tee ─► queue ─► appsink
 *                          ▲ paint           ▲ BLOCK      ▲ EOS probe   (└─► display branch)
 *                                            │            │
 *                                            └──► videoflip (after the swap)
 */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
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

#define N_FRAMES 45
#define W 160
#define H 120
#define SQ 8

typedef struct {
    GstElement *pipeline;   /* borrowed */
    GstElement *q;          /* borrowed: owned by pipeline */
    GstElement *cur;        /* borrowed: element being replaced */
    GstElement *next;       /* borrowed: owned by pipeline after add */
    GstElement *after;      /* borrowed: element downstream of cur (tee) */
    gint swapped;           /* atomic: set when the new element is linked */
} Swap;

static GstPadProbeReturn paint(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    (void)pad; (void)u;
    GstBuffer *buf = gst_buffer_make_writable(GST_PAD_PROBE_INFO_BUFFER(info));
    GST_PAD_PROBE_INFO_DATA(info) = buf;
    GstMapInfo m;
    if (gst_buffer_map(buf, &m, GST_MAP_WRITE)) {   /* GRAY8, stride == W */
        for (int y = 0; y < SQ; y++) memset(m.data + y * W, 0xFF, SQ);
        gst_buffer_unmap(buf, &m);
    }
    return GST_PAD_PROBE_OK;
}

/* Step 3: runs when the drained EOS leaves the old element. We are still in
 * the queue's streaming thread, *inside* gst_pad_send_event() called from
 * block_cb, i.e. upstream is blocked: nothing else touches these links. */
static GstPadProbeReturn eos_cb(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    Swap *s = u;
    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) != GST_EVENT_EOS)
        return GST_PAD_PROBE_PASS;          /* not ours: let it through */

    gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));
    printf("  [swap] old element drained, relinking\n");

    gst_element_set_state(s->cur, GST_STATE_NULL);
    /* gst_bin_remove drops the bin's ref -> identity is finalized. */
    gst_bin_remove(GST_BIN(s->pipeline), s->cur);
    s->cur = NULL;

    /* gst_bin_add sinks the floating ref of 'next': the bin now owns it. */
    gst_bin_add(GST_BIN(s->pipeline), s->next);
    gboolean ok = gst_element_link_many(s->q, s->next, s->after, NULL);
    if (!ok) fprintf(stderr, "relink failed\n");
    gst_element_sync_state_with_parent(s->next);   /* -> PLAYING */
    g_atomic_int_set(&s->swapped, 1);

    /* DROP the EOS: if it reached appsink the stream would end now. */
    return GST_PAD_PROBE_DROP;
}

/* Step 1+2: called in the queue's thread with a buffer held at q.src. */
static GstPadProbeReturn block_cb(GstPad *pad, GstPadProbeInfo *info, gpointer u)
{
    Swap *s = u;
    printf("  [swap] q.src blocked, draining old element with EOS\n");
    /* Remove the block probe now: once we return, data may flow again. */
    gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));

    GstPad *osrc = gst_element_get_static_pad(s->cur, "src");     /* +1 */
    gst_pad_add_probe(osrc,
        GST_PAD_PROBE_TYPE_BLOCK | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        eos_cb, s, NULL);
    gst_object_unref(osrc);

    /* identity has no thread of its own, so this call runs eos_cb
     * synchronously: by the time it returns, the new element is linked. */
    GstPad *osink = gst_element_get_static_pad(s->cur, "sink");   /* +1 */
    gst_pad_send_event(osink, gst_event_new_eos());   /* event: transfer full */
    gst_object_unref(osink);

    /* OK: the buffer that was waiting here is now pushed to the NEW peer,
     * because gst_pad_push looks up the peer after running probes. */
    return GST_PAD_PROBE_OK;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    /* is-live=true paces the source at 30 fps (1.5 s total) so main has
     * time to trigger the swap while frames are still flowing. */
    gchar *desc = g_strdup_printf(
        "videotestsrc is-live=true num-buffers=%d pattern=black"
        " ! capsfilter name=cf caps=video/x-raw,format=GRAY8,width=%d,height=%d,framerate=30/1"
        " ! queue name=q ! identity name=cur ! tee name=t"
        " t. ! queue ! appsink name=out sync=false"
        "%s",
        N_FRAMES, W, H,
        display ? " t. ! queue ! videoconvert ! autovideosink" : "");
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);           /* owned */
    g_free(desc);
    CHECK(pipeline != NULL, err ? err->message : "parse failed");

    Swap s = {0};
    s.pipeline = pipeline;
    /* gst_bin_get_by_name returns +1; we drop it immediately because the
     * pipeline keeps the elements alive (they are borrowed pointers). */
    s.q = gst_bin_get_by_name(GST_BIN(pipeline), "q");     gst_object_unref(s.q);
    s.cur = gst_bin_get_by_name(GST_BIN(pipeline), "cur"); gst_object_unref(s.cur);
    s.after = gst_bin_get_by_name(GST_BIN(pipeline), "t"); gst_object_unref(s.after);
    /* New element: floating ref until gst_bin_add sinks it. Ref-sink it now
     * so we own a real ref even if the swap never happens (no leak). */
    s.next = gst_object_ref_sink(gst_element_factory_make("videoflip", "flip"));
    CHECK(s.next != NULL, "videoflip missing (gst-plugins-good)");
    g_object_set(s.next, "method", 5 /* vertical-flip */, NULL);

    GstElement *cf = gst_bin_get_by_name(GST_BIN(pipeline), "cf");        /* +1 */
    GstPad *cfsrc = gst_element_get_static_pad(cf, "src");                /* +1 */
    gst_pad_add_probe(cfsrc, GST_PAD_PROBE_TYPE_BUFFER, paint, NULL, NULL);
    gst_object_unref(cfsrc);
    gst_object_unref(cf);

    GstElement *out = gst_bin_get_by_name(GST_BIN(pipeline), "out");      /* +1 */
    CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "PLAYING failed");

    int before = 0, after = 0, frames = 0, order_ok = 1;
    for (;;) {
        GstSample *smp = gst_app_sink_pull_sample(GST_APP_SINK(out)); /* owned */
        if (!smp) break;                                              /* EOS */
        GstBuffer *b = gst_sample_get_buffer(smp);
        GstMapInfo m;
        CHECK(gst_buffer_map(b, &m, GST_MAP_READ), "map");
        int top = m.data[0] == 0xFF;
        int bottom = m.data[(H - 1) * W] == 0xFF;
        gst_buffer_unmap(b, &m);
        gst_sample_unref(smp);
        if (top && !bottom) { before++; if (after) order_ok = 0; }
        else if (bottom && !top) after++;
        frames++;

        /* Trigger the swap from the app thread after 15 frames. The probe
         * callback will run later in the queue's streaming thread. */
        if (frames == 15) {
            GstPad *qsrc = gst_element_get_static_pad(s.q, "src");     /* +1 */
            gst_pad_add_probe(qsrc, GST_PAD_PROBE_TYPE_BLOCK_DOWNSTREAM,
                              block_cb, &s, NULL);
            gst_object_unref(qsrc);
        }
    }
    gst_object_unref(out);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    gst_object_unref(s.next);   /* our ref from ref_sink (bin had its own) */

    printf("frames=%d  before-swap=%d  after-swap=%d\n", frames, before, after);
    CHECK(g_atomic_int_get(&s.swapped), "swap never happened");
    CHECK(frames == N_FRAMES, "frames lost during the swap");
    CHECK(before >= 15 && after > 0, "both phases must be seen");
    CHECK(before + after == frames, "unexpected frame content");
    CHECK(order_ok, "an unflipped frame appeared after a flipped one");
    printf("OK: identity replaced by videoflip while PLAYING, no frame lost\n");
    gst_deinit();
    return 0;
}
