/*
 * 02_probe_drop.c — throw away data with a probe (GST_PAD_PROBE_DROP)
 *
 * Concept : Returning GST_PAD_PROBE_DROP from a BUFFER probe makes the buffer
 *           vanish: GStreamer unrefs it for you and the push returns GST_FLOW_OK
 *           upstream as if it had been delivered. We drop every odd frame
 *           (a crude 30 -> 15 fps decimator) and count at the sink that
 *           exactly half arrived. A second probe counts what passes.
 * Build   : make run P=gstreamer/07-pad-probes/02_probe_drop.c
 *           ./build/gstreamer/07-pad-probes/02_probe_drop --display
 * Read    : README.md  (section "Probe return values")
 *
 *   videotestsrc ──► [src]videoconvert ──► [sink]sink
 *                       ▲                     ▲
 *            probe A: DROP odd frames   probe B: count survivors
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

#define N_FRAMES 40

static GstPadProbeReturn drop_odd(GstPad *pad, GstPadProbeInfo *info,
                                  gpointer user_data)
{
    (void)pad; (void)info;
    int *seen = user_data;               /* only touched by this one thread */
    int idx = (*seen)++;
    /* DROP: GStreamer unrefs the buffer; nothing downstream sees it.
     * NOTE: dropping an EVENT this way (e.g. CAPS or EOS) would break the
     * stream; this probe only listens to BUFFERs so that cannot happen. */
    return (idx % 2) ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
}

static GstPadProbeReturn count_cb(GstPad *pad, GstPadProbeInfo *info,
                                  gpointer user_data)
{
    (void)pad; (void)info;
    (*(int *)user_data)++;
    return GST_PAD_PROBE_OK;
}

static void add_probe(GstElement *pipeline, const char *elem, const char *padname,
                      GstPadProbeCallback cb, int *counter)
{
    GstElement *e = gst_bin_get_by_name(GST_BIN(pipeline), elem);   /* +1 */
    CHECK(e != NULL, "element not found");
    GstPad *pad = gst_element_get_static_pad(e, padname);           /* +1 */
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, cb, counter, NULL);
    gst_object_unref(pad);
    gst_object_unref(e);
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    const char *desc = display
        ? "videotestsrc num-buffers=" G_STRINGIFY(N_FRAMES) " pattern=ball"
          " ! videoconvert name=conv ! autovideosink name=sink"
        : "videotestsrc num-buffers=" G_STRINGIFY(N_FRAMES)
          " ! videoconvert name=conv ! fakesink name=sink sync=false";

    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);       /* we own it */
    CHECK(pipeline != NULL, err ? err->message : "parse failed");

    int seen = 0, arrived = 0;
    add_probe(pipeline, "conv", "src", drop_odd, &seen);
    add_probe(pipeline, "sink", "sink", count_cb, &arrived);

    CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "PLAYING failed");
    GstBus *bus = gst_element_get_bus(pipeline);                     /* +1 */
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 15 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "no EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    printf("produced %d frames, sink received %d\n", seen, arrived);
    CHECK(seen == N_FRAMES, "probe A did not see every frame");
    CHECK(arrived == N_FRAMES / 2, "sink should get exactly half");
    printf("OK: DROP removed every odd frame, EOS still reached the sink\n");
    gst_deinit();
    return 0;
}
