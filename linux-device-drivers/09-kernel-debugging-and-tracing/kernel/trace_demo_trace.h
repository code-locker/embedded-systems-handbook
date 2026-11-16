/* SPDX-License-Identifier: GPL-2.0 */
/*
 * trace_demo_trace.h — tracepoint definitions for trace_demo.c
 *
 * Concept : TRACE_EVENT() describes one event three ways at once: its C
 *           prototype (TP_PROTO/TP_ARGS), the binary record stored in the
 *           ring buffer (TP_STRUCT__entry + TP_fast_assign) and how to print
 *           it (TP_printk). This header is included TWICE by trace_demo.c:
 *           normally (declares trace_esh_tick() etc.) and, because of
 *           CREATE_TRACE_POINTS, again by <trace/define_trace.h>, which
 *           re-reads it with different macro definitions to generate the
 *           code. That is why the guard allows TRACE_HEADER_MULTI_READ and
 *           why the define_trace include sits OUTSIDE the guard.
 * Read    : ../README.md  (section 3.6 "Tracepoints and your own TRACE_EVENT")
 */
#undef TRACE_SYSTEM
#define TRACE_SYSTEM esh_trace	/* -> /sys/kernel/tracing/events/esh_trace/ */

#if !defined(_TRACE_DEMO_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define _TRACE_DEMO_TRACE_H

#include <linux/tracepoint.h>

/*
 * One event per tick of the kthread.
 * Fixed-size fields only (no __string): the __assign_str() macro changed
 * signature in 6.10, and this lesson builds for both 6.8 and 6.18.
 */
TRACE_EVENT(esh_tick,

	TP_PROTO(u64 seq, unsigned int period_ms, s64 late_us),

	TP_ARGS(seq, period_ms, late_us),

	TP_STRUCT__entry(
		__field(u64, seq)
		__field(unsigned int, period_ms)
		__field(s64, late_us)
	),

	TP_fast_assign(
		__entry->seq = seq;
		__entry->period_ms = period_ms;
		__entry->late_us = late_us;
	),

	TP_printk("seq=%llu period_ms=%u late_us=%lld",
		  __entry->seq, __entry->period_ms, __entry->late_us)
);

/*
 * A conditional event: the condition is evaluated only when the event is
 * enabled, and nothing is recorded when it is false. Cheaper than a filter.
 */
TRACE_EVENT_CONDITION(esh_late,

	TP_PROTO(u64 seq, s64 late_us, const char *who),

	TP_ARGS(seq, late_us, who),

	TP_CONDITION(late_us > 1000),

	TP_STRUCT__entry(
		__field(u64, seq)
		__field(s64, late_us)
		__array(char, who, 16)
	),

	TP_fast_assign(
		__entry->seq = seq;
		__entry->late_us = late_us;
		strscpy(__entry->who, who, sizeof(__entry->who));
	),

	TP_printk("seq=%llu woke %lld us late (%s)",
		  __entry->seq, __entry->late_us, __entry->who)
);

#endif /* _TRACE_DEMO_TRACE_H */

/*
 * Outside the guard. define_trace.h includes TRACE_INCLUDE_PATH/
 * TRACE_INCLUDE_FILE.h relative to the kernel's include/trace/ directory,
 * so "." only works because the Makefile adds -I$(src) for trace_demo.o.
 */
#undef TRACE_INCLUDE_PATH
#define TRACE_INCLUDE_PATH .
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_FILE trace_demo_trace
#include <trace/define_trace.h>
