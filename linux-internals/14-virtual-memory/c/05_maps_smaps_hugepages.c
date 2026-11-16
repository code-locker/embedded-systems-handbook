/*
 * 05_maps_smaps_hugepages.c — reading your own address space: parsing
 *                             /proc/self/maps and smaps, VSZ vs RSS vs PSS
 *                             (with a page shared between two processes),
 *                             and huge pages (THP and hugetlbfs)
 *
 * Concept : /proc/self/maps lists every VMA: range, permissions, offset,
 *           device, inode, and what backs it. /proc/self/smaps adds, per
 *           VMA, how much of it is in RAM:
 *             Size  = virtual size (sum of all Size = VSZ)
 *             Rss   = pages resident in RAM, shared ones counted fully
 *             Pss   = proportional: a page shared by N processes counts 1/N
 *                     (the only number you can ADD across processes)
 *             AnonHugePages = how much is backed by 2 MiB transparent
 *                     huge pages (one TLB entry, one fault for 512 pages)
 *           dsa/02-memory-management shows the process layout itself; this
 *           program measures it.
 * Build   : make run P=linux-internals/14-virtual-memory/c/05_maps_smaps_hugepages.c
 * Read    : ../README.md  (sections "3.11 /proc/self/maps and smaps" and
 *           "3.12 Huge pages")
 */
#define _GNU_SOURCE /* MAP_ANONYMOUS, MAP_HUGETLB, MADV_HUGEPAGE */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fflush(stdout);                                                  \
            fprintf(stderr, "CHECK failed: %s  (%s:%d) errno=%d\n", #cond,   \
                    __FILE__, __LINE__, errno);                              \
            _exit(1);                                                        \
        }                                                                    \
    } while (0)

static long g_page;

/* One line of /proc/self/maps:
 *   7f12a4c00000-7f12a4c22000 r-xp 00000000 103:02 1312  /usr/lib/.../libc.so.6
 *   start        end          perm offset   dev    inode path (or [heap], [stack]) */
struct vma {
    uintptr_t lo, hi;
    char perms[5];
    char name[256];
};

static int find_vma(uintptr_t addr, struct vma *out)
{
    FILE *f = fopen("/proc/self/maps", "r");
    CHECK(f != NULL);
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof line, f)) {
        struct vma v;
        memset(&v, 0, sizeof v);
        unsigned long lo, hi;
        int pos = 0;
        if (sscanf(line, "%lx-%lx %4s %*s %*s %*s %n", &lo, &hi, v.perms,
                   &pos) < 3)
            continue;
        v.lo = lo;
        v.hi = hi;
        if (addr >= v.lo && addr < v.hi) {
            snprintf(v.name, sizeof v.name, "%s", pos > 0 ? line + pos : "");
            v.name[strcspn(v.name, "\n")] = '\0';
            *out = v;
            found = 1;
        }
    }
    fclose(f);
    return found;
}

/* Fields of the smaps block for the VMA that contains addr (in kB). */
struct smaps {
    long size, rss, pss, shared_dirty, private_dirty, anon_huge;
    int thp_eligible;
};

static int smaps_for(uintptr_t addr, struct smaps *s)
{
    FILE *f = fopen("/proc/self/smaps", "r");
    CHECK(f != NULL);
    char line[512];
    int in = 0, done = 0;
    memset(s, 0, sizeof *s);
    s->thp_eligible = -1;
    while (!done && fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char dash;
        /* a header line starts with "lo-hi " (hex digits, then '-') */
        if (sscanf(line, "%lx%c%lx ", &lo, &dash, &hi) == 3 && dash == '-') {
            if (in)
                done = 1;
            else
                in = addr >= lo && addr < hi;
            continue;
        }
        if (!in)
            continue;
        long v;
        if (sscanf(line, "Size: %ld", &v) == 1) s->size = v;
        else if (sscanf(line, "Rss: %ld", &v) == 1) s->rss = v;
        else if (sscanf(line, "Pss: %ld", &v) == 1) s->pss = v;
        else if (sscanf(line, "Shared_Dirty: %ld", &v) == 1) s->shared_dirty = v;
        else if (sscanf(line, "Private_Dirty: %ld", &v) == 1) s->private_dirty = v;
        else if (sscanf(line, "AnonHugePages: %ld", &v) == 1) s->anon_huge = v;
        else if (sscanf(line, "THPeligible: %ld", &v) == 1) s->thp_eligible = (int)v;
    }
    fclose(f);
    return in;
}

static long rollup(const char *key)
{
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (!f)
        return -1;
    char line[256];
    long v = -1;
    size_t kl = strlen(key);
    while (fgets(line, sizeof line, f))
        if (strncmp(line, key, kl) == 0 && line[kl] == ':')
            v = strtol(line + kl + 1, NULL, 10);
    fclose(f);
    return v;
}

/* ---- 1. which VMA holds what ---- */
static int g_global = 42; /* .data */

static void demo_maps(void)
{
    int local = 0;
    void *heap = malloc(64); /* owned here */
    CHECK(heap != NULL);
    void *anon = mmap(NULL, (size_t)g_page, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(anon != MAP_FAILED);
    struct {
        const char *what;
        uintptr_t addr;
    } q[] = {
        {"code  (demo_maps)", (uintptr_t)demo_maps},
        {"data  (g_global) ", (uintptr_t)&g_global},
        {"heap  (malloc 64)", (uintptr_t)heap},
        {"mmap  (anonymous)", (uintptr_t)anon},
        {"libc  (getpid)   ", (uintptr_t)getpid}, /* printf would show the
                                                    sanitizer interceptor */
        {"stack (local)    ", (uintptr_t)&local},
    };
    printf("1) which mapping holds what (from /proc/self/maps):\n");
    struct vma v[6];
    for (int i = 0; i < 6; i++) {
        CHECK(find_vma(q[i].addr, &v[i]));
        const char *slash = strrchr(v[i].name, '/');
        printf("     %s %#14lx  %s  %s\n", q[i].what, (unsigned long)q[i].addr,
               v[i].perms, v[i].name[0] ? (slash ? slash + 1 : v[i].name)
                                        : "(anonymous)");
    }
    CHECK(v[0].perms[2] == 'x' && v[0].perms[1] == '-'); /* W^X: r-xp */
    CHECK(v[1].perms[1] == 'w' && v[1].perms[2] == '-');
    CHECK(strcmp(v[5].name, "[stack]") == 0);
    CHECK(v[3].lo <= (uintptr_t)anon && v[3].name[0] == '\0');
    printf("   (under ASan/TSan malloc comes from the sanitizer's own "
           "mmap'd arena, not [heap])\n");
    free(heap);
    CHECK(munmap(anon, (size_t)g_page) == 0);
}

/* ---- 2. VSZ vs RSS vs PSS ---- */
static void demo_vsz_rss_pss(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    CHECK(f != NULL);
    long size = 0, res = 0, shared = 0;
    CHECK(fscanf(f, "%ld %ld %ld", &size, &res, &shared) == 3);
    fclose(f);
    long pss = rollup("Pss"), rss = rollup("Rss");
    printf("2) whole process: VSZ %ld MiB, RSS %ld KiB (statm) / %ld KiB "
           "(smaps_rollup), PSS %ld KiB\n", size * (g_page / 1024) / 1024,
           res * (g_page / 1024), rss, pss);
    printf("   VSZ includes everything reserved (huge under sanitizers: "
           "shadow memory); PSS < RSS because libc pages are shared\n");
    CHECK(pss > 0 && pss <= rss + 64);
}

/* ---- 3. PSS with one mapping shared by two processes ----
 *
 *   parent PTEs --+                    Rss(parent) = 256 pages
 *                 +--> [256 pages] <-- Pss(parent) = 256/2 = 128 pages
 *   child  PTEs --+                    (same in the child)
 */
static void demo_shared_pss(void)
{
    const size_t len = 1u << 20;
    char *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    memset(p, 'S', len); /* parent maps all pages */

    int ready[2], done[2];
    CHECK(pipe(ready) == 0 && pipe(done) == 0);
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        close(ready[0]);
        close(done[1]);
        unsigned sum = 0;
        for (size_t off = 0; off < len; off += (size_t)g_page)
            sum += (unsigned char)p[off]; /* map every page in the child */
        char c = sum ? 'r' : '0';
        if (write(ready[1], &c, 1) != 1)
            _exit(2);
        if (read(done[0], &c, 1) != 1) /* stay alive until measured */
            _exit(3);
        _exit(0);
    }
    close(ready[1]);
    close(done[0]);
    char c;
    CHECK(read(ready[0], &c, 1) == 1 && c == 'r');
    struct smaps s;
    CHECK(smaps_for((uintptr_t)p, &s));
    CHECK(write(done[1], "x", 1) == 1);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
          WEXITSTATUS(st) == 0);
    close(ready[0]);
    close(done[1]);

    printf("3) 1 MiB MAP_SHARED, touched by parent AND child: Size %ld kB, "
           "Rss %ld kB, Pss %ld kB, Shared_Dirty %ld kB\n", s.size, s.rss,
           s.pss, s.shared_dirty);
    CHECK(s.rss >= 1024 * 9 / 10);
    CHECK(s.pss * 10 <= s.rss * 6); /* about half */
    CHECK(s.shared_dirty >= 1024 * 9 / 10);
    printf("   -> adding RSS of both processes would count this MiB twice; "
           "adding PSS counts it once\n");
    CHECK(munmap(p, len) == 0);
}

/* ---- 4. huge pages ---- */
static void demo_huge(void)
{
    char mode[128] = "(unavailable)";
    FILE *f = fopen("/sys/kernel/mm/transparent_hugepage/enabled", "r");
    if (f) {
        if (!fgets(mode, sizeof mode, f))
            mode[0] = '\0';
        mode[strcspn(mode, "\n")] = '\0';
        fclose(f);
    }
    printf("4) THP mode: %s   ([x] = active)\n", mode);

    /* THP needs a 2 MiB-aligned, 2 MiB-sized piece of the VMA. Map 6 MiB
     * and use the aligned 4 MiB inside it. */
    const size_t huge = 2u << 20, len = 2 * huge;
    char *raw = mmap(NULL, len + huge, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(raw != MAP_FAILED);
    char *al = (char *)(((uintptr_t)raw + huge - 1) & ~(uintptr_t)(huge - 1));
    int adv = madvise(al, len, MADV_HUGEPAGE);
    memset(al, 1, len);
    struct smaps s;
    CHECK(smaps_for((uintptr_t)al, &s));
    printf("   4 MiB aligned + MADV_HUGEPAGE (%s): AnonHugePages %ld kB, "
           "THPeligible %d (printed, not asserted)\n",
           adv == 0 ? "accepted" : strerror(errno), s.anon_huge,
           s.thp_eligible);
    CHECK(munmap(raw, len + huge) == 0);

    /* hugetlbfs: pages reserved up front by the admin
     * (echo 16 > /proc/sys/vm/nr_hugepages). Usually 0 on a desktop. */
    void *h = mmap(NULL, huge, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
    if (h == MAP_FAILED) {
        printf("   MAP_HUGETLB: %s (no pages reserved in "
               "/proc/sys/vm/nr_hugepages): skipped\n", strerror(errno));
    } else {
        memset(h, 2, huge);
        printf("   MAP_HUGETLB: got a reserved 2 MiB page\n");
        CHECK(munmap(h, huge) == 0);
    }
}

int main(void)
{
    alarm(20);
    g_page = sysconf(_SC_PAGESIZE);
    demo_maps();
    demo_vsz_rss_pss();
    demo_shared_pss();
    demo_huge();
    printf("05_maps_smaps_hugepages: all checks passed\n");
    return 0;
}
