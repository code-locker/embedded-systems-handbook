/*
 * 01_probe_inspect.c — watch buffers and events go past a pad (read-only probe)
 *
 * Concept : A pad probe is a callback that GStreamer calls *in the streaming
 *           thread* every time data crosses a pad. Here we attach a BUFFER
 *           probe (count frames, check PTS only grows) and an EVENT_DOWNSTREAM
 *           probe (see STREAM_START, CAPS, SEGMENT, EOS in order) to the
 *           sink pad of fakesink. We only look; we return GST_PAD_PROBE_OK.
 * Build   : make run P=gstreamer/07-pad-probes/01_probe_inspect.c
 *           ./build/gstreamer/07-pad-probes/01_probe_inspect --display
 * Read    : README.md  (sections "Mental model" and "Probe types")
 *
 *   videotestsrc num-buffers=30 ──► videoconvert ──► [sink pad]sink
 *                                                   ▲
 *                                   probe: BUFFER | EVENT_DOWNSTREAM
 *                                   (runs in videotestsrc's streaming thread)
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

#define N_FRAMES 30

/* Shared between the streaming thread (writer) and main (reader after EOS).
 * The bus EOS message is posted after the last probe call, and the bus is a
 * synchronisation point, so plain fields are safe to read in main later. */
typedef struct {
    int buffers;
    GstClockTime last_pts;
    gboolean pts_monotonic;
    /* event order as seen, e.g. "stream-start caps segment eos" */
    char events[256];
} ProbeStats;

static GstPadProbeReturn on_data(GstPad *pad, GstPadProbeInfo *info,
                                 gpointer user_data)
{
    (void)pad;
    ProbeStats *st = user_data;

    if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
        /* BORROWED: the buffer belongs to the pipeline. Do not unref it. */
        GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
        GstClockTime pts = GST_BUFFER_PTS(buf);
        if (st->buffers > 0 && GST_CLOCK_TIME_IS_VALID(st->last_pts) &&
            pts <= st->last_pts)
            st->pts_monotonic = FALSE;
        st->last_pts = pts;
        st->buffers++;
        if (st->buffers <= 3 || st->buffers == N_FRAMES)
            printf("  buffer #%2d pts=%" GST_TIME_FORMAT " size=%zu\n",
                   st->buffers, GST_TIME_ARGS(pts), gst_buffer_get_size(buf));
    } else if (info->type & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        GstEvent *ev = GST_PAD_PROBE_INFO_EVENT(info);     /* BORROWED */
        const char *name = GST_EVENT_TYPE_NAME(ev);
        printf("  event  %s\n", name);
        if (GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) {
            GstCaps *caps = NULL;
            gst_event_parse_caps(ev, &caps);            /* BORROWED too */
            gchar *s = gst_caps_to_string(caps);         /* we own s */
            printf("         caps = %s\n", s);
            g_free(s);
        }
        strncat(st->events, name, sizeof st->events - strlen(st->events) - 2);
        strcat(st->events, " ");
    }
    /* OK = "let it pass unchanged and keep this probe installed". */
    return GST_PAD_PROBE_OK;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    const char *desc = display
        ? "videotestsrc num-buffers=" G_STRINGIFY(N_FRAMES)
          " ! video/x-raw,format=I420,width=320,height=240 ! videoconvert"
          " ! autovideosink name=sink"
        : "videotestsrc num-buffers=" G_STRINGIFY(N_FRAMES)
          " ! video/x-raw,format=I420,width=320,height=240 ! videoconvert"
          " ! fakesink name=sink sync=false";

    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);   /* we own it */
    CHECK(pipeline != NULL, err ? err->message : "parse failed");

    GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink"); /* +1 ref */
    GstPad *pad = gst_element_get_static_pad(sink, "sink");            /* +1 ref */

    ProbeStats st = { 0, GST_CLOCK_TIME_NONE, TRUE, "" };
    /* One probe can listen to several kinds of data: OR the mask bits. */
    gulong id = gst_pad_add_probe(
        pad, GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        on_data, &st, NULL);
    CHECK(id != 0, "gst_pad_add_probe failed");

    /* The probe holds no ref we need; drop ours now (the pad lives on
     * inside the pipeline). */
    gst_object_unref(pad);
    gst_object_unref(sink);

    CHECK(gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "PLAYING failed");

    GstBus *bus = gst_element_get_bus(pipeline);                       /* +1 */
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "no EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    printf("events seen in order: %s\n", st.events);
    CHECK(st.buffers == N_FRAMES, "wrong number of buffers");
    CHECK(st.pts_monotonic, "PTS went backwards");
    /* Sticky events always arrive in this order before the first buffer. */
    CHECK(strstr(st.events, "stream-start caps segment") == st.events,
          "expected stream-start, caps, segment first");
    CHECK(strstr(st.events, "eos") != NULL, "EOS event not seen");
    printf("OK: %d buffers, PTS monotonic, sticky events in order\n",
           st.buffers);
    gst_deinit();
    return 0;
}
