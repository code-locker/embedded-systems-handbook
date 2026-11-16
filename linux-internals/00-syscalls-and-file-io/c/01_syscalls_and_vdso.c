/*
 * 01_syscalls_and_vdso.c — what a system call really is: the libc wrapper,
 *                          syscall(2), the raw svc/syscall instruction, the
 *                          vDSO shortcut, errno, and the EINTR retry loop
 *
 * Concept : Your program runs in user mode (EL0 on ARM64, ring 3 on x86).
 *           It cannot touch hardware or other processes; it ASKS the kernel
 *           (EL1 / ring 0) with one special instruction: `svc #0` on ARM64,
 *           `syscall` on x86-64. The syscall NUMBER goes in a register (x8 /
 *           rax), arguments in x0..x5 / rdi,rsi,rdx,r10,r8,r9, the result
 *           comes back in x0 / rax: >= 0 is success, -4095..-1 is -errno.
 *           libc turns that into "return -1 and set errno". Some calls
 *           (clock_gettime, gettimeofday) are answered by the vDSO, kernel
 *           code mapped into your process, with no mode switch at all.
 * Build   : make run P=linux-internals/00-syscalls-and-file-io/c/01_syscalls_and_vdso.c
 * Read    : ../README.md  (sections "3.1 User space, kernel space, system
 *           calls", "3.2 The vDSO" and "3.3 errno and EINTR")
 */
#define _GNU_SOURCE /* syscall(), SYS_* numbers, getauxval */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
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

static double now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ---- the raw instruction, no libc in between ----------------------------
 *
 *   ARM64 (BeagleBone AI-64, Cortex-A72)      x86-64 (the host PC)
 *     x8  = syscall number (getpid = 172)       rax = number (getpid = 39)
 *     x0..x5 = arguments                        rdi rsi rdx r10 r8 r9
 *     svc #0      -> exception to EL1           syscall -> ring 0
 *     x0  = result (or -errno)                  rax = result (or -errno)
 *
 * The CPU saves the user PC and PSTATE, switches to the kernel stack and
 * jumps to the exception vector (el0_svc on arm64, entry_SYSCALL_64 on
 * x86). The kernel indexes sys_call_table[number], runs the handler, and
 * `eret` / `sysret` drops back to user mode right after the instruction.
 */
static long raw_getpid(void)
{
#if defined(__aarch64__)
    register long x8 __asm__("x8") = SYS_getpid;
    register long x0 __asm__("x0");
    __asm__ volatile("svc #0" : "=r"(x0) : "r"(x8) : "memory");
    return x0;
#elif defined(__x86_64__)
    long ret;
    /* `syscall` clobbers rcx (saved RIP) and r11 (saved RFLAGS). */
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"((long)SYS_getpid)
                     : "rcx", "r11", "memory");
    return ret;
#else
    return syscall(SYS_getpid);
#endif
}

/* Raw open of a missing file: the kernel's answer is -ENOENT, in the
 * register, with no errno involved. errno is a libc invention. */
static long raw_open_missing(const char *path)
{
#if defined(__aarch64__)
    /* arm64 has no open(); only openat (56). AT_FDCWD = -100. */
    register long x8 __asm__("x8") = SYS_openat;
    register long x0 __asm__("x0") = AT_FDCWD;
    register long x1 __asm__("x1") = (long)path;
    register long x2 __asm__("x2") = O_RDONLY;
    __asm__ volatile("svc #0"
                     : "+r"(x0)
                     : "r"(x8), "r"(x1), "r"(x2)
                     : "memory");
    return x0;
#elif defined(__x86_64__)
    long ret;
    register long r10 __asm__("r10") = 0;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"((long)SYS_openat), "D"((long)AT_FDCWD),
                       "S"(path), "d"((long)O_RDONLY), "r"(r10)
                     : "rcx", "r11", "memory");
    return ret;
#else
    long r = syscall(SYS_openat, AT_FDCWD, path, O_RDONLY);
    return r < 0 ? -errno : r;
#endif
}

/* ---- 1. three ways to make the same system call ---- */
static void demo_three_ways(void)
{
    pid_t a = getpid();                  /* libc wrapper                */
    long b = syscall(SYS_getpid);        /* generic wrapper, by number  */
    long c = raw_getpid();               /* the instruction itself      */
    printf("1) getpid() = %d, syscall(SYS_getpid=%d) = %ld, raw %s = %ld\n",
           (int)a, (int)SYS_getpid, b,
#if defined(__aarch64__)
           "svc #0",
#else
           "syscall insn",
#endif
           c);
    CHECK(a == b && b == c);
}

/* ---- 2. errno: the libc convention on top of "-errno in a register" ---- */
static void demo_errno(void)
{
    const char *missing = "/nonexistent/dir/file";

    long raw = raw_open_missing(missing);
    printf("2) raw openat(\"%s\") returned %ld  (= -ENOENT, -%d)\n", missing,
           raw, ENOENT);
    CHECK(raw == -ENOENT);

    errno = 0;
    int fd = open(missing, O_RDONLY);
    int saved = errno; /* save it NOW: the next libc call may change it */
    printf("   open() returned %d, errno=%d (%s)\n", fd, saved,
           strerror(saved));
    CHECK(fd == -1 && saved == ENOENT);

    /* errno is NOT cleared on success. Only look at it after a failure. */
    errno = 12345;
    CHECK(getpid() > 0);
    CHECK(errno == 12345);
    printf("   errno is untouched by a successful call (still %d): test the "
           "RETURN value, then read errno\n", errno);

    /* close() on a bad fd: EBADF. A close() error on a real file can mean
     * a lost write (NFS, some FUSE): check it for files you wrote. */
    errno = 0;
    CHECK(close(-1) == -1 && errno == EBADF);
    printf("   close(-1) -> EBADF (%s)\n", strerror(EBADF));
}

/* ---- 3. cost: real syscall vs vDSO ----
 *
 *   getpid()/syscall()                 clock_gettime(CLOCK_MONOTONIC)
 *   user --svc--> kernel --eret--> user    user: read vvar page + counter
 *        (~100-400 ns with mitigations)        (~15-30 ns, no mode switch)
 */
static void demo_cost(void)
{
    enum { N = 100000 };
    struct timespec ts;
    volatile long sink = 0;

    double t0 = now_ns();
    for (int i = 0; i < N; i++)
        sink += syscall(SYS_getpid);
    double t_sys = (now_ns() - t0) / N;

    t0 = now_ns();
    for (int i = 0; i < N; i++) {
        syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
        sink += ts.tv_nsec;
    }
    double t_sys_clock = (now_ns() - t0) / N;

    t0 = now_ns();
    for (int i = 0; i < N; i++) {
        clock_gettime(CLOCK_MONOTONIC, &ts); /* vDSO: no kernel entry */
        sink += ts.tv_nsec;
    }
    double t_vdso = (now_ns() - t0) / N;
    (void)sink;

    printf("3) cost per call (%d calls each, printed not asserted):\n", N);
    printf("     syscall(SYS_getpid)              %7.1f ns\n", t_sys);
    printf("     syscall(SYS_clock_gettime)       %7.1f ns  (forced kernel "
           "entry)\n", t_sys_clock);
    printf("     clock_gettime() via vDSO         %7.1f ns  (%.1fx faster)\n",
           t_vdso, t_vdso > 0 ? t_sys_clock / t_vdso : 0.0);
}

/* ---- 4. where the vDSO lives ---- */
static void demo_vdso_map(void)
{
    unsigned long ehdr = getauxval(AT_SYSINFO_EHDR); /* given by the kernel
                                                        at exec time */
    CHECK(ehdr != 0);
    FILE *f = fopen("/proc/self/maps", "r");
    CHECK(f != NULL);
    char line[512];
    unsigned long lo = 0, hi = 0;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "[vdso]")) {
            CHECK(sscanf(line, "%lx-%lx", &lo, &hi) == 2);
            found = 1;
        }
    }
    fclose(f);
    CHECK(found);
    printf("4) vDSO ELF image at 0x%lx (auxv AT_SYSINFO_EHDR); /proc/self/maps "
           "[vdso] = %lx-%lx (%lu KiB)\n", ehdr, lo, hi, (hi - lo) / 1024);
    CHECK(ehdr >= lo && ehdr < hi);
}

/* ---- 5. EINTR: a signal interrupts a blocking call ----
 *
 *   parent: read(pipe) ........ blocked in the kernel ......... returns
 *   child :   +100 ms kill(SIGUSR1)       +300 ms write("data")
 *                    |
 *                    v  handler runs; without SA_RESTART read() -> -1 EINTR
 *
 * The rule: a slow call (read on a pipe/socket/tty, wait, accept, sem_wait,
 * nanosleep...) can fail with EINTR when a handler runs. Retry it. */
static volatile sig_atomic_t g_signals;
static void on_usr1(int sig)
{
    (void)sig;
    g_signals++; /* async-signal-safe: only a sig_atomic_t store */
}

static ssize_t read_retry(int fd, void *buf, size_t n, int *eintr_count)
{
    ssize_t r;
    for (;;) {
        r = read(fd, buf, n);
        if (r >= 0 || errno != EINTR)
            return r;
        (*eintr_count)++; /* interrupted before any data: just try again */
    }
}

static void demo_eintr(int use_restart)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = use_restart ? SA_RESTART : 0;
    CHECK(sigaction(SIGUSR1, &sa, NULL) == 0);
    g_signals = 0;

    int p[2];
    CHECK(pipe(p) == 0);
    pid_t parent = getpid();
    fflush(stdout);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        alarm(5);
        close(p[0]);
        struct timespec d = {0, 100 * 1000 * 1000};
        nanosleep(&d, NULL);
        kill(parent, SIGUSR1);
        d.tv_nsec = 300 * 1000 * 1000;
        nanosleep(&d, NULL);
        _exit(write(p[1], "data", 4) == 4 ? 0 : 1);
    }
    close(p[1]);
    char buf[8] = {0};
    int eintr = 0;
    ssize_t r = read_retry(p[0], buf, sizeof buf - 1, &eintr);
    CHECK(r == 4 && memcmp(buf, "data", 4) == 0);
    int st;
    CHECK(waitpid(pid, &st, 0) == pid && WIFEXITED(st) &&
          WEXITSTATUS(st) == 0);
    close(p[0]);
    /* How many EINTRs we saw depends on timing (and on sanitizers, which
     * defer handlers): print it, assert only that the data arrived. */
    printf("5) %s: signal handled %d time(s), read() failed with EINTR %d "
           "time(s), retry loop got \"%s\"\n",
           use_restart ? "SA_RESTART   " : "no SA_RESTART", (int)g_signals,
           eintr, buf);
    signal(SIGUSR1, SIG_DFL);
}

int main(void)
{
    alarm(20); /* watchdog: never hang the test run */
    demo_three_ways();
    demo_errno();
    demo_cost();
    demo_vdso_map();
    demo_eintr(0);
    demo_eintr(1);
    printf("01_syscalls_and_vdso: all checks passed\n");
    return 0;
}
