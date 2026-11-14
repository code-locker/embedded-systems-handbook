/*
 * 04_playbin.c — the whole player in one element
 *
 * Concept : playbin = uridecodebin + automatic audio and video sink branches
 *           (with queues, converters, volume, subtitle overlay). You give it
 *           a URI and optional sinks; it does all the pad-added work for you.
 *           We replace its sinks with fakesinks to stay headless, then read
 *           stream info (n-video, n-audio) and the duration.
 * Build   : make run P=gstreamer/04-dynamic-pads/04_playbin.c
 *           ./build/gstreamer/04-dynamic-pads/04_playbin --display
 * Read    : README.md  (section "decodebin / uridecodebin / playbin")
 *
 *   ┌───────────────────────── playbin ───────────────────────────┐
 *   │  uridecodebin ─┬─► video: queue ► convert ► scale ► video-sink │
 *   │                └─► audio: queue ► convert ► resample ► volume ► audio-sink │
 *   └──────────────────────────────────────────────────────────────┘
 */
#include <gst/gst.h>
#include <glib/gstdio.h>
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

#define NUM_FRAMES 50      /* 50 frames at 25 fps = 2 s */

static GstMessage *wait_for(GstElement *pipe, GstMessageType types)
{
    GstBus *bus = gst_element_get_bus(pipe);
    GstMessage *m = gst_bus_timed_pop_filtered(bus, 10 * GST_SECOND, types);
    gst_object_unref(bus);
    return m;                                    /* caller owns (or NULL) */
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    /* Test file with video (MJPEG) AND audio (raw PCM in Matroska). */
    gchar *path = g_build_filename(g_get_tmp_dir(), "gst_lesson04_pb.mkv", NULL);
    gchar *desc = g_strdup_printf(
        "matroskamux name=mux ! filesink location=\"%s\" "
        "videotestsrc num-buffers=%d ! video/x-raw,width=320,height=240,"
        "framerate=25/1 ! jpegenc ! queue ! mux. "
        "audiotestsrc num-buffers=86 samplesperbuffer=1024 ! "   /* ~2 s @44.1k */
        "audio/x-raw,rate=44100,channels=1 ! audioconvert ! queue ! mux.",
        path, NUM_FRAMES);
    GstElement *writer = gst_parse_launch(desc, NULL);
    CHECK(writer != NULL, "parse writer");
    gst_element_set_state(writer, GST_STATE_PLAYING);
    GstMessage *m = wait_for(writer, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS, "writer EOS");
    gst_message_unref(m);
    gst_element_set_state(writer, GST_STATE_NULL);
    gst_object_unref(writer);
    g_free(desc);

    gchar *uri = gst_filename_to_uri(path, NULL);
    GstElement *playbin = gst_element_factory_make("playbin", "player");
    CHECK(playbin != NULL, "playbin");
    gst_object_ref_sink(playbin);       /* floating -> we own it (no parent) */
    g_object_set(playbin, "uri", uri, NULL);

    if (!display) {
        /* "video-sink"/"audio-sink" take the element; playbin sinks the
         * floating ref, so we do not unref them. */
        GstElement *vs = gst_element_factory_make("fakesink", "vsink");
        GstElement *as = gst_element_factory_make("fakesink", "asink");
        g_object_set(vs, "sync", FALSE, NULL);
        g_object_set(as, "sync", FALSE, NULL);
        g_object_set(playbin, "video-sink", vs, "audio-sink", as, NULL);
    }

    /* PAUSED = preroll: streams are discovered and the first frame waits
     * in the sink. Now stream info and duration are known. */
    gst_element_set_state(playbin, GST_STATE_PAUSED);
    GstState st;
    CHECK(gst_element_get_state(playbin, &st, NULL, 10 * GST_SECOND) ==
              GST_STATE_CHANGE_SUCCESS && st == GST_STATE_PAUSED,
          "prerolled into PAUSED");

    gint n_video = 0, n_audio = 0;
    g_object_get(playbin, "n-video", &n_video, "n-audio", &n_audio, NULL);
    gint64 dur = -1;
    gst_element_query_duration(playbin, GST_FORMAT_TIME, &dur);
    printf("n-video=%d n-audio=%d duration=%" GST_TIME_FORMAT "\n",
           n_video, n_audio, GST_TIME_ARGS(dur));
    CHECK(n_video == 1 && n_audio == 1, "one video and one audio stream");
    CHECK(dur > 1900 * GST_MSECOND && dur < 2100 * GST_MSECOND,
          "duration about 2 s");

    gst_element_set_state(playbin, GST_STATE_PLAYING);
    m = wait_for(playbin, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    gboolean ok = m && GST_MESSAGE_TYPE(m) == GST_MESSAGE_EOS;
    if (m)
        gst_message_unref(m);
    gst_element_set_state(playbin, GST_STATE_NULL);
    gst_object_unref(playbin);
    g_remove(path);
    g_free(path);
    g_free(uri);

    CHECK(ok, "played to EOS");
    printf("OK: playbin found both streams and played them\n");
    gst_deinit();
    return 0;
}
