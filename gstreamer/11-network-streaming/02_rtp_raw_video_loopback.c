/*
 * 02_rtp_raw_video_loopback.c — uncompressed video over RTP (RFC 4175),
 *                               and where the receiver's caps come from
 *
 * Concept : rtpvrawpay sends raw pixels, so the receiver must know EVERYTHING
 *           about the picture (sampling, depth, width, height, colorimetry):
 *           none of it is inside the packets. In real systems this travels
 *           out-of-band in an SDP description (RTSP DESCRIBE, a file, ...).
 *           Here we play "SDP": we start the sender, wait until its
 *           payloader has negotiated, read the payloader's src caps, and hand
 *           exactly those caps to the receiver's udpsrc.
 * Build   : make run P=gstreamer/11-network-streaming/02_rtp_raw_video_loopback.c
 *           ./build/gstreamer/11-network-streaming/02_rtp_raw_video_loopback --display
 * Read    : README.md  (section "Caps travel out of band (SDP)")
 *
 *  sender:  videotestsrc ─► I420 64x48@30 ─► rtpvrawpay ─► udpsink ──┐
 *                                              │                      │
 *                (1) once negotiated: read caps┘                      │
 *                         "application/x-rtp, encoding-name=RAW,      │
 *                          sampling=YCbCr-4:2:0, width=64, ..."       │
 *                                  │ (2) copy = our "SDP"             │
 *                                  ▼                                  │
 *  receiver: udpsrc caps=<that> ◄─────────────────────────────────────┘
 *              ─► rtpjitterbuffer ─► rtpvrawdepay ─► appsink (check 64x48)
 *
 *  Bandwidth: 64*48*1.5 = 4.6 KB/frame * 30 = 138 KB/s here; 1080p30 raw
 *  would be ~93 MB/s (~750 Mbit/s): that is why real streams are encoded.
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
#define W 64
#define H 48

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);
    GError *err = NULL;

    /* ---- receiver first, WITHOUT caps, so we know the port ----------
     * READY opens + binds the socket; data cannot flow before PLAYING. */
    const char *rx_desc = display
        ? "udpsrc name=src port=0 address=127.0.0.1 ! "
          "rtpjitterbuffer latency=50 ! rtpvrawdepay ! tee name=t "
          "t. ! queue ! appsink name=out sync=false "
          "t. ! queue ! videoconvert ! videoscale ! "
          "video/x-raw,width=320,height=240 ! autovideosink sync=false"
        : "udpsrc name=src port=0 address=127.0.0.1 ! "
          "rtpjitterbuffer latency=50 ! rtpvrawdepay ! "
          "appsink name=out sync=false";
    GstElement *rx = gst_parse_launch(rx_desc, &err);         /* owned */
    CHECK(rx != NULL, err ? err->message : "rx parse");
    GstElement *src = gst_bin_get_by_name(GST_BIN(rx), "src");  /* ref */
    GstElement *sink = gst_bin_get_by_name(GST_BIN(rx), "out"); /* ref */

    CHECK(gst_element_set_state(rx, GST_STATE_READY) ==
              GST_STATE_CHANGE_SUCCESS, "rx READY (bind)");
    gint port = 0;
    g_object_get(src, "port", &port, NULL);
    CHECK(port > 0, "no port");

    /* ---- sender ------------------------------------------------------ */
    gchar *tx_desc = g_strdup_printf(
        "videotestsrc num-buffers=%d is-live=true ! "
        "video/x-raw,format=I420,width=%d,height=%d,framerate=30/1 ! "
        "rtpvrawpay name=pay ! udpsink host=127.0.0.1 port=%d",
        N_FRAMES, W, H, port);
    GstElement *tx = gst_parse_launch(tx_desc, &err);         /* owned */
    g_free(tx_desc);
    CHECK(tx != NULL, err ? err->message : "tx parse");

    /* A live source in PAUSED does not produce data (NO_PREROLL), so caps
     * are not negotiated yet. Instead we ask the payloader which caps it
     * WOULD produce for the input caps: easiest is to go PLAYING and wait
     * for the caps to appear on its src pad. udpsink keeps sending to a
     * socket nobody reads yet; those first packets are simply lost (UDP). */
    GstElement *pay = gst_bin_get_by_name(GST_BIN(tx), "pay"); /* ref */
    GstPad *paysrc = gst_element_get_static_pad(pay, "src");  /* ref */
    CHECK(gst_element_set_state(tx, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "tx PLAYING");
    GstCaps *caps = NULL;
    for (int i = 0; i < 200 && !caps; i++) {      /* up to 2 s */
        caps = gst_pad_get_current_caps(paysrc);  /* ref or NULL */
        if (!caps) g_usleep(10 * 1000);
    }
    CHECK(caps != NULL, "payloader never negotiated");
    gchar *caps_str = gst_caps_to_string(caps);
    printf("\"SDP\" (payloader caps):\n  %s\n", caps_str);
    g_free(caps_str);

    /* ---- give the receiver its caps, start it --------------------------
     * Without this line: "not-negotiated" error at rtpvrawdepay. */
    g_object_set(src, "caps", caps, NULL);  /* udpsrc takes its own ref */
    gst_caps_unref(caps);
    gst_object_unref(paysrc);
    gst_object_unref(pay);
    CHECK(gst_element_set_state(rx, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE, "rx PLAYING");

    /* ---- count frames ----------------------------------------------- */
    int got = 0, w = 0, h = 0;
    GstClockTime wait = 3 * GST_SECOND;
    GstSample *s;
    while ((s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), wait))) {
        GstStructure *st = gst_caps_get_structure(gst_sample_get_caps(s), 0);
        gst_structure_get_int(st, "width", &w);
        gst_structure_get_int(st, "height", &h);
        const gchar *fmt = gst_structure_get_string(st, "format");
        if (got == 0)
            printf("receiver caps: format=%s %dx%d\n", fmt, w, h);
        CHECK(fmt && strcmp(fmt, "I420") == 0, "format must survive");
        got++;
        gst_sample_unref(s);
        wait = 1 * GST_SECOND;
    }

    gst_object_unref(sink);
    gst_object_unref(src);

    gst_element_set_state(tx, GST_STATE_NULL);
    gst_element_set_state(rx, GST_STATE_NULL);
    gst_object_unref(tx);
    gst_object_unref(rx);

    printf("received %d of %d frames\n", got, N_FRAMES);
    /* Packets sent before the receiver reached PLAYING are not lost: the
     * socket was already bound, so the KERNEL queued them in its receive
     * buffer. Had the socket been unbound, they would be gone (UDP). */
    CHECK(got >= N_FRAMES / 2, "too few frames");
    CHECK(w == W && h == H, "width/height must match the sender");
    printf("OK: raw RTP video round-trip, caps carried out of band\n");
    gst_deinit();
    return 0;
}
