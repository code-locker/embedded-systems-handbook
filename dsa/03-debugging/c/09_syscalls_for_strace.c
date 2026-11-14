/*
 * 09_syscalls_for_strace.c — a CORRECT program that makes interesting system calls
 *
 * Concept : strace shows every system call (the program's conversation with
 *           the kernel) with arguments, return values and errno names; ltrace
 *           shows calls into shared libraries. Many "mysterious" failures are
 *           obvious in strace: a wrong path (ENOENT), a permission (EACCES), a
 *           blocking read, a missing config file, a device ioctl failing.
 * Build   : make run P=dsa/03-debugging/c/09_syscalls_for_strace.c
 *           strace -f -e trace=%file,%desc,%memory ./build/dsa/03-debugging/c/09_syscalls_for_strace
 *           strace -c ./build/dsa/03-debugging/c/09_syscalls_for_strace     (summary table)
 *           ltrace ./build/dsa/03-debugging/c/09_syscalls_for_strace         (sudo apt install ltrace)
 * Read    : ../README.md  (section "3.6 strace and ltrace")
 */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    /* 1. A failing open: strace shows  openat(AT_FDCWD, "/etc/does-not-exist.conf", O_RDONLY) = -1 ENOENT */
    int fd = open("/etc/does-not-exist.conf", O_RDONLY);
    assert(fd == -1 && errno == ENOENT);

    /* 2. A temp file: write, fsync, lseek, read back. */
    char path[] = "/tmp/strace-demo-XXXXXX";
    fd = mkstemp(path);                        /* openat(... O_RDWR|O_CREAT|O_EXCL, 0600) */
    assert(fd >= 0);
    const char msg[] = "hello from user space\n";
    assert(write(fd, msg, sizeof msg - 1) == (ssize_t)(sizeof msg - 1));
    assert(fsync(fd) == 0);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    char buf[64] = {0};
    assert(read(fd, buf, sizeof buf) == (ssize_t)(sizeof msg - 1));
    assert(strcmp(buf, msg) == 0);

    /* 3. fstat + mmap the file. */
    struct stat st;
    assert(fstat(fd, &st) == 0 && st.st_size == (off_t)(sizeof msg - 1));
    char *map = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    assert(map != MAP_FAILED && map[0] == 'h');
    munmap(map, (size_t)st.st_size);
    close(fd);
    unlink(path);

    /* 4. A big malloc -> mmap; a small one -> maybe brk (without ASan). */
    void *big = malloc(1 << 20);
    assert(big);
    free(big);

    /* 5. stdout write: printf is buffered; the write() appears at exit or when full. */
    printf("did open/mkstemp/write/fsync/lseek/read/fstat/mmap/munmap/unlink; pid %d\n", (int)getpid());
    printf("All checks passed.\n");
    return 0;
}
