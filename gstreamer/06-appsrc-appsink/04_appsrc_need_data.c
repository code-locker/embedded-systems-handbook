/*
 * 04_appsrc_need_data.c — let the pipeline ASK for data (need-data / enough-data)
 *
 * Concept : Pull-style feeding. appsrc emits "need-data" when its internal
 *           queue runs low and "enough-data" when it is above max-bytes.
 *           A GMainLoop idle source produces frames only between the two,
 *           so memory stays bounded however fast or slow downstream is.
 *           This is the pattern for files, network or a decoder you own.
 *           Frames go appsrc ! videoconvert ! fakesink (or a window).
 * Build   : make run P=gstreamer/06-appsrc-appsink/04_appsrc_need_data.c
 *           ./build/gstreamer/06-appsrc-appsink/04_appsrc_need_data --display
 * Read    : README.md  (section "need-data / enough-data: flow control")
 *
 *   queue level of appsrc (bytes)
 *   max-bytes ┤······enough-data──┐ stop feeding      ┌── enough-data
 *             │        /\         │                  /
 *             │       /  \________▼__ consumer drains_/
 *         low ┤──need-data: start feeding (idle source added)
 *             └──────────────────────────────────────────► time
 */
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
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

#define W 320
#define H 240
#define FPS 30
#define NUM_FRAMES 90

typedef struct {
    GstAppSrc *src;     /* borrowed: pipeline owns it                     */
    GMainLoop *loop;
    GMutex lock;        /* guards idle_id and the two call counters:      */
    guint idle_id;      /*   0 = not feeding                              */
    int need_data_calls, enough_data_calls;
    gboolean eos_sent;  /*   no more feeding after EOS                    */
    int pushed;         /* main-loop thread only                          */
    gint sink_frames;   /* atomic: from the streaming thread              */
} App;

/* Idle callback in the MAIN LOOP thread: push one frame per call. */
static gboolean feed_one(gpointer data)
{
    App *app = data;
    if (app->pushed == NUM_FRAMES) {
        g_mutex_lock(&app->lock);
        app->idle_id = 0;            /* we remove ourselves by returning */
        app->eos_sent = TRUE;
        g_mutex_unlock(&app->lock);
        gst_app_src_end_of_stream(app->src);
        return G_SOURCE_REMOVE;
    }
    /* Grey level ramps with the frame number (visible fade in --display). */
    GstBuffer *buf = gst_buffer_new_allocate(NULL, W * H, NULL);
    gst_buffer_memset(buf, 0, (guint8)(app->pushed * 255 / NUM_FRAMES), W * H);
    GST_BUFFER_PTS(buf) = gst_util_uint64_scale(app->pushed, GST_SECOND, FPS);
    GST_BUFFER_DURATION(buf) = gst_util_uint64_scale(1, GST_SECOND, FPS);
    app->pushed++;
    /* push_buffer takes ownership. It may synchronously trigger
     * enough-data in THIS thread - that is why the callbacks below must not
     * hold the lock while calling back into appsrc. */
    gst_app_src_push_buffer(app->src, buf);
    return G_SOURCE_CONTINUE;
}

/* need-data / enough-data can be emitted from appsrc's STREAMING thread
 * (queue drained) or from the thread calling push_buffer (queue full).
 * Do no heavy work here: only switch the idle feeder on/off. Adding and
 * removing GSources is thread-safe in GLib; the mutex protects idle_id,
 * which both threads read and write. */
static void on_need_data(GstAppSrc *src, guint length, gpointer data)
{
    (void)src; (void)length;
    App *app = data;
    g_mutex_lock(&app->lock);
    app->need_data_calls++;
    if (app->idle_id == 0 && !app->eos_sent)
        app->idle_id = g_idle_add(feed_one, app);
    g_mutex_unlock(&app->lock);
}

static void on_enough_data(GstAppSrc *src, gpointer data)
{
    (void)src;
    App *app = data;
    g_mutex_lock(&app->lock);
    app->enough_data_calls++;
    if (app->idle_id != 0) {
        g_source_remove(app->idle_id);
        app->idle_id = 0;
    }
    g_mutex_unlock(&app->lock);
}

static void on_handoff(GstElement *e, GstBuffer *b, GstPad *p, gpointer data)
{
    (void)e; (void)b; (void)p;
    g_atomic_int_inc(&((App *)data)->sink_frames);
}

static gboolean on_bus(GstBus *bus, GstMessage *msg, gpointer data)
{
    (void)bus;
    App *app = data;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *e = NULL;
        gst_message_parse_error(msg, &e, NULL);
        fprintf(stderr, "ERROR: %s\n", e->message);
        g_clear_error(&e);
        g_main_loop_quit(app->loop);
    } else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
        g_main_loop_quit(app->loop);
    }
    return G_SOURCE_CONTINUE;
}

static gboolean on_timeout(gpointer data)
{
    fprintf(stderr, "timeout!\n");
    g_main_loop_quit(((App *)data)->loop);
    return G_SOURCE_REMOVE;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    GstElement *pipeline = gst_parse_launch(display
        ? "appsrc name=in ! videoconvert ! autovideosink"
        : "appsrc name=in ! videoconvert ! fakesink name=sink sync=false "
          "signal-handoffs=true", NULL);
    CHECK(pipeline != NULL, "parse");
    GstElement *in = gst_bin_get_by_name(GST_BIN(pipeline), "in");    /* own */

    App app;
    memset(&app, 0, sizeof app);
    app.src = GST_APP_SRC(in);
    app.loop = g_main_loop_new(NULL, FALSE);
    g_mutex_init(&app.lock);

    GstCaps *caps = gst_caps_new_simple("video/x-raw",
        "format", G_TYPE_STRING, "GRAY8", "width", G_TYPE_INT, W,
        "height", G_TYPE_INT, H, "framerate", GST_TYPE_FRACTION, FPS, 1, NULL);
    /* max-bytes = 3 frames: a small queue so need/enough fire often. */
    g_object_set(in, "caps", caps, "format", GST_FORMAT_TIME,
                 "max-bytes", (guint64)(3 * W * H), NULL);
    gst_caps_unref(caps);

    /* Callbacks struct (faster than signals); same idea as appsink. */
    GstAppSrcCallbacks cbs = { 0 };
    cbs.need_data = on_need_data;
    cbs.enough_data = on_enough_data;
    gst_app_src_set_callbacks(app.src, &cbs, &app, NULL);

    if (!display) {
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
        g_signal_connect(sink, "handoff", G_CALLBACK(on_handoff), &app);
        gst_object_unref(sink);
    }

    GstBus *bus = gst_element_get_bus(pipeline);
    guint bus_watch = gst_bus_add_watch(bus, on_bus, &app);
    gst_object_unref(bus);
    guint to = g_timeout_add_seconds(20, on_timeout, &app);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    g_main_loop_run(app.loop);

    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_source_remove(to);
    g_source_remove(bus_watch);
    if (app.idle_id)                 /* pipeline is NULL: no more callbacks */
        g_source_remove(app.idle_id);
    g_main_loop_unref(app.loop);
    g_mutex_clear(&app.lock);
    gst_object_unref(in);
    gst_object_unref(pipeline);

    printf("pushed=%d need-data=%d enough-data=%d frames at sink=%d\n",
           app.pushed, app.need_data_calls, app.enough_data_calls,
           app.sink_frames);
    CHECK(app.pushed == NUM_FRAMES, "all frames produced");
    CHECK(app.need_data_calls >= 1, "need-data drove the feeding");
    if (!display)
        CHECK(app.sink_frames == NUM_FRAMES, "all frames consumed");
    printf("OK: feeding was driven by need-data / enough-data\n");
    gst_deinit();
    return 0;
}
