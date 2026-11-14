/*
 * 01_rtp_jpeg_loopback.c — stream MJPEG over RTP/UDP to yourself
 *
 * Concept : two independent pipelines in one process, connected only by a
 *           UDP socket on 127.0.0.1, exactly as a camera and a viewer on two
 *           machines would be. The receiver binds port 0 (kernel picks a
 *           free port) and we read the chosen port back, so parallel runs
 *           never collide. EOS does NOT travel over UDP: the receiver stops
 *           when no frame arrives for a while, as real receivers must.
 * Build   : make run P=gstreamer/11-network-streaming/01_rtp_jpeg_loopback.c
 *           ./build/gstreamer/11-network-streaming/01_rtp_jpeg_loopback --display
 * Read    : README.md  (sections "Mental model" and "How it works")
 *
 *  SENDER  (pipeline 1)
 *  videotestsrc ─► I420 320x240@30 ─► jpegenc ─► rtpjpegpay ─► udpsink ─┐
 *   raw frames                        ~10 KB/frm  split into   sendto()  │ UDP
 *                                                 ≤1400 B pkts           │ 127.0.0.1:P
 *  RECEIVER (pipeline 2)                                                 │
 *  udpsrc port=0(→P) ◄───────────────────────────────────────────────────┘
 *    │ caps=application/x-rtp (MUST be given: UDP carries no caps)
 *    ▼
 *  rtpjitterbuffer ─► rtpjpegdepay ─► jpegdec ─► appsink
 *   reorder,           glue packets    decode     count frames,
 *   de-jitter          into frames                check 320x240
 */
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
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

/* Pops one ERROR (if any) from a bus without blocking; prints it. */
static gboolean bus_has_error(GstElement *pipe)
{
    GstBus *bus = gst_element_get_bus(pipe);               /* owned */
    GstMessage *m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (!m)
        return FALSE;
    GError *e = NULL;
    gst_message_parse_error(m, &e, NULL);
    fprintf(stderr, "ERROR from %s: %s\n", GST_OBJECT_NAME(m->src),
            e->message);
    g_clear_error(&e);
    gst_message_unref(m);
    return TRUE;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);
    GError *err = NULL;

    /* ---------------- receiver first: it must be listening ------------ */
    /* The caps string is the "contract" an SDP file would carry:
     * media=video, 90 kHz RTP clock (always for video), JPEG = static
     * payload type 26 (RFC 2435). Without caps the depayloader cannot
     * negotiate and you get "not-negotiated". */
    const char *rx_desc_headless =
        "udpsrc name=rx port=0 address=127.0.0.1 "
        "  caps=\"application/x-rtp,media=video,clock-rate=90000,"
        "encoding-name=JPEG,payload=26\" ! "
        "rtpjitterbuffer latency=100 ! rtpjpegdepay ! jpegdec ! "
        "appsink name=out sync=false";
    const char *rx_desc_display =
        "udpsrc name=rx port=0 address=127.0.0.1 "
        "  caps=\"application/x-rtp,media=video,clock-rate=90000,"
        "encoding-name=JPEG,payload=26\" ! "
        "rtpjitterbuffer latency=100 ! rtpjpegdepay ! jpegdec ! tee name=t "
        "t. ! queue ! appsink name=out sync=false "
        "t. ! queue ! videoconvert ! autovideosink sync=false";
    GstElement *rx = gst_parse_launch(display ? rx_desc_display
                                              : rx_desc_headless, &err);
    CHECK(rx != NULL, err ? err->message : "rx parse");

    /* Going to PAUSED opens and binds the socket (in READY). A LIVE source
     * returns NO_PREROLL: there is nothing to preroll, data only comes
     * "in real time". That is normal, not an error. */
    GstStateChangeReturn r = gst_element_set_state(rx, GST_STATE_PLAYING);
    CHECK(r != GST_STATE_CHANGE_FAILURE, "receiver PLAYING");

    GstElement *udpsrc = gst_bin_get_by_name(GST_BIN(rx), "rx"); /* ref */
    gint port = 0;
    g_object_get(udpsrc, "port", &port, NULL); /* now the real bound port */
    gst_object_unref(udpsrc);
    CHECK(port > 0, "udpsrc did not report the bound port");
    printf("receiver listening on 127.0.0.1:%d\n", port);

    /* ---------------- sender -------------------------------------------- */
    /* format=I420 is REQUIRED: RFC 2435 (RTP/JPEG) only carries 4:2:0 or
     * 4:2:2 baseline JPEG. Leave it out and videotestsrc may pick a 4:4:4
     * format; rtpjpegpay then warns "Invalid component" and sends nothing.
     *
     * udpsink sync=true (default): packets leave at the frame rate, like a
     * real camera. Try sync=false: all 30 frames burst out at once, and a
     * small kernel socket buffer may drop some of them. */
    gchar *tx_desc = g_strdup_printf(
        "videotestsrc num-buffers=%d is-live=true ! "
        "video/x-raw,format=I420,width=320,height=240,framerate=30/1 ! "
        "jpegenc quality=70 ! rtpjpegpay mtu=1400 ! "
        "udpsink host=127.0.0.1 port=%d", N_FRAMES, port);
    GstElement *tx = gst_parse_launch(tx_desc, &err);     /* owned */
    CHECK(tx != NULL, err ? err->message : "tx parse");
    printf("sender  : %s\n", tx_desc);
    g_free(tx_desc);
    CHECK(gst_element_set_state(tx, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "sender PLAYING");

    /* ---------------- count frames at the receiver ---------------------- */
    GstElement *sink = gst_bin_get_by_name(GST_BIN(rx), "out"); /* ref */
    int got = 0, w = 0, h = 0;
    /* First frame may take a moment (jitterbuffer latency 100 ms); after
     * that, 1 s of silence means "the stream is over". */
    GstClockTime wait = 3 * GST_SECOND;
    GstSample *s;
    while ((s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), wait))) {
        if (got == 0) {
            GstStructure *st =
                gst_caps_get_structure(gst_sample_get_caps(s), 0);
            gst_structure_get_int(st, "width", &w);
            gst_structure_get_int(st, "height", &h);
        }
        got++;
        gst_sample_unref(s);
        wait = 1 * GST_SECOND;
    }
    gst_object_unref(sink);

    /* The sender DID reach EOS locally (its sink saw it) — check. */
    GstBus *txbus = gst_element_get_bus(tx);
    GstMessage *m = gst_bus_timed_pop_filtered(
        txbus, 2 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    gboolean tx_eos = m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
    if (m) gst_message_unref(m);
    gst_object_unref(txbus);

    gboolean rx_err = bus_has_error(rx);

    gst_element_set_state(tx, GST_STATE_NULL);
    gst_element_set_state(rx, GST_STATE_NULL);
    gst_object_unref(tx);
    gst_object_unref(rx);

    printf("sent %d frames, received %d frames of %dx%d\n", N_FRAMES, got, w,
           h);
    CHECK(tx_eos, "sender should reach EOS");
    CHECK(!rx_err, "receiver posted an error");
    /* UDP may lose packets; on loopback losses are rare, but a lost packet
     * makes the depayloader drop the whole frame. Accept 80%. */
    CHECK(got >= N_FRAMES * 8 / 10, "too many frames lost");
    CHECK(w == 320 && h == 240, "decoded size must match sender caps");

    printf("OK: RTP/JPEG over UDP loopback works\n");
    gst_deinit();
    return 0;
}
