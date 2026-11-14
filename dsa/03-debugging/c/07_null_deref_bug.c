/*
 * 07_null_deref_bug.c — INTENTIONAL BUG: segfault on a NULL pointer
 *
 * Bug     : find_sensor() returns NULL when the name is unknown; the caller
 *           does not check and dereferences it. Address 0x8 (the offset of
 *           'value' in struct sensor) is in the never-mapped page 0 -> SIGSEGV.
 * Finds it: gdb (run, it stops at the faulting line; 'bt', 'p s'),
 *           a core dump opened later with 'gdb prog core', UBSan ("runtime
 *           error: member access within null pointer of type 'struct sensor'",
 *           which stops the program with this repo's -fno-sanitize-recover),
 *           or ASan alone ("SEGV on unknown address 0x000000000008 ... READ
 *           memory access" + "Hint: address points to the zero page").
 * Build   : make run P=dsa/03-debugging/c/07_null_deref_bug.c   -> UBSan report, exit 1
 * Read    : ../README.md  (section "Bug 6: segfault on NULL" and "Core dumps")
 */
#include <stdio.h>
#include <string.h>

struct sensor {
    const char *name;
    int value;            /* offset 8 */
};

static struct sensor sensors[] = {{"cpu", 61}, {"gpu", 74}};

static struct sensor *find_sensor(const char *name)
{
    for (size_t i = 0; i < sizeof sensors / sizeof sensors[0]; i++)
        if (strcmp(sensors[i].name, name) == 0)
            return &sensors[i];
    return NULL;                                   /* documented: may return NULL */
}

static int read_temp(const char *name)
{
    struct sensor *s = find_sensor(name);
    return s->value;                               /* BUG: no NULL check */
}

int main(void)
{
    const char *wanted[] = {"cpu", "gpu", "ddr"};  /* "ddr" does not exist */
    for (int i = 0; i < 3; i++)
        printf("%s = %d\n", wanted[i], read_temp(wanted[i]));
    return 0;
}
