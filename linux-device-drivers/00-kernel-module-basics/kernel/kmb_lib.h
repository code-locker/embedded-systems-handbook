/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kmb_lib.h — the "API" kmb_lib.ko offers to other modules
 *
 * Concept : A header only gives the compiler the prototypes. The actual
 *           linking happens at insmod time: the module loader looks every
 *           undefined symbol of kmb_consumer.ko up in the kernel's export
 *           table, which kmb_lib.ko filled with EXPORT_SYMBOL_GPL().
 * Read    : ../README.md  (section 3.6 "Exporting symbols between modules")
 */
#ifndef KMB_LIB_H
#define KMB_LIB_H

#include <linux/types.h>
#include <linux/version.h>

/*
 * Symbol namespace of kmb_lib_version(). Since 6.13 EXPORT_SYMBOL_NS_GPL()
 * and MODULE_IMPORT_NS() take a string literal ("KMB"); before that they
 * took a bare token and stringified it. The host (6.8) and the board (6.18)
 * sit on opposite sides of that change, so the one version check lives here.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
#define KMB_NS "KMB"
#else
#define KMB_NS KMB	/* __stringify(KMB_NS) -> "KMB" */
#endif

/* Fletcher-16 checksum of buf[0..len). Exported GPL-only. */
u16 kmb_lib_fletcher16(const void *buf, size_t len);

/* How many checksums have been computed since kmb_lib was loaded. */
unsigned long kmb_lib_calls(void);

/* Version string; exported in symbol namespace "KMB" (needs an import). */
const char *kmb_lib_version(void);

#endif /* KMB_LIB_H */
