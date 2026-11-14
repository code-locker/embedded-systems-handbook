/*
 * 01_thread_boundaries.c — see which thread runs which part of a pipeline
 *
 * Concept : A source element owns a streaming thread that pushes buffers all
 *           the way downstream through every element, until it reaches a
 *           queue. A queue stores the buffer and has its OWN thread that
 *           pushes onward. So "a queue = a thread boundary". We record
 *           g_thread_self() in probes before and after the queue and check:
 *             - without queue: both probes run in the same thread
 *             - with queue:    they run in two different threads
 *             - neither is the application (main) thread
 * Build   : make run P=gstreamer/08-queues-threads-tee/01_thread_boundaries.c
 *           ./build/gstreamer/08-queues-threads-tee/01_thread_boundaries --display
 * Read    : README.md  (section "Streaming threads")
 *
 *   run 1:  videotestsrc ─► identity a ─► identity b ─► sink
 *           └────────────── thread T1 ──────────────────┘
 *   run 2:  videotestsrc ─► identity a ─► queue ─► identity b ─► sink
 *           └──────── T1 ─────────────┘  └──────── T2 ──────────┘
 */
#define _GNU_SOURCE           /* for pthread_getname_np (Linux) */
#include <pthread.h>
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

typedef struct {
    GThread *first;      /* thread seen on the first buffer */
    gboolean always_same;/* did every buffer come from that same thread? */
    int n;
} ThreadSeen;

static GstPadProbeReturn record_thread(GstPad *pad, GstPadProbeInfo *info,
                                       gpointer user_data)
{
    (void)info;
    ThreadSeen *t = user_data;
    GThread *self = g_thread_self();   /* borrowed, no ref needed to compare */
    if (t->n == 0) {
        t->first = self;
        /* Thread names come from GstTask: "<element>:src", cut to 15
         * chars by Linux (that is why you see "videotestsrc0:s"). */
        char name[32] = "?";
        pthread_getname_np(pthread_self(), name, sizeof name);
        printf("    %-8s runs in thread '%s'\n", GST_OBJECT_NAME(GST_PAD_PARENT(pad)), name);
    } else if (self != t->first) {
        t->always_same = FALSE;
    }
    t->n++;
    return GST_PAD_PROBE_OK;
}

static void probe(GstElement *pipeline, const char *name, ThreadSeen *t)
{
    GstElement *e = gst_bin_get_by_name(GST_BIN(pipeline), name);   /* +1 */
    GstPad *p = gst_element_get_static_pad(e, "src");               /* +1 */
    gst_pad_add_probe(p, GST_PAD_PROBE_TYPE_BUFFER, record_thread, t, NULL);
    gst_object_unref(p);
    gst_object_unref(e);
}

/* Runs one pipeline, returns whether a and b ran in the same thread. */
static gboolean run(const char *middle, gboolean display)
{
    gchar *desc = g_strdup_printf(
        "videotestsrc num-buffers=20 ! identity name=a ! %s identity name=b"
        " ! videoconvert ! %s",
        middle, display ? "autovideosink" : "fakesink sync=false");
    printf("  %s\n", desc);
    GError *err = NULL;
    GstElement *pipeline = gst_parse_launch(desc, &err);       /* owned */
    g_free(desc);
    CHECK(pipeline != NULL, "parse failed");

    ThreadSeen a = { NULL, TRUE, 0 }, b = { NULL, TRUE, 0 };
    probe(pipeline, "a", &a);
    probe(pipeline, "b", &b);

    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus *bus = gst_element_get_bus(pipeline);               /* +1 */
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, 10 * GST_SECOND, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
    CHECK(msg && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS, "no EOS");
    gst_message_unref(msg);
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    CHECK(a.n == 20 && b.n == 20, "frame count");
    /* A streaming thread is a long-lived task: every buffer of one segment
     * comes from the same thread. */
    CHECK(a.always_same && b.always_same, "thread changed mid-stream");
    GThread *app = g_thread_self();
    CHECK(a.first != app && b.first != app,
          "data must not flow in the application thread");
    return a.first == b.first;
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    gboolean display = (argc > 1 && strcmp(argv[1], "--display") == 0);

    printf("run 1: no queue\n");
    gboolean same1 = run("", display);
    printf("run 2: with queue\n");
    gboolean same2 = run("queue !", display);

    CHECK(same1, "without a queue, a and b share one streaming thread");
    CHECK(!same2, "a queue must start a new streaming thread");
    printf("OK: queue = thread boundary; app thread never touches buffers\n");
    gst_deinit();
    return 0;
}
