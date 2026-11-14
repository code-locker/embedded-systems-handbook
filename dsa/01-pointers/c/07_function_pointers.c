/*
 * 07_function_pointers.c — functions have addresses too: callbacks, qsort, dispatch tables
 *
 * Concept : A function's name decays to its address (in .text). A function
 *           pointer stores that address and calls through it — an indirect
 *           call. This is how C does polymorphism: qsort's comparator, the
 *           kernel's struct file_operations, interrupt vector tables,
 *           state machines and command dispatch tables.
 * Build   : make run P=dsa/01-pointers/c/07_function_pointers.c
 * Read    : ../README.md  (section "3.7 Function pointers")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 1. Basic syntax ------------------------------------------------------- */
static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }

/* Raw syntax: int (*op)(int, int)  — "op is a pointer to a function (int,int)->int".
 * The parentheses matter: int *op(int,int) declares a FUNCTION returning int*.
 * Always typedef it: */
typedef int (*binop_fn)(int, int);

static int apply(binop_fn op, int a, int b) { return op(a, b); }  /* callback */

/* ---- 2. qsort: comparator returns <0, 0, >0 -------------------------------- */
static int cmp_int_asc(const void *pa, const void *pb)
{
    int a = *(const int *)pa, b = *(const int *)pb;
    return (a > b) - (a < b);    /* NOT a - b: that overflows for big values */
}

struct sensor { const char *name; int temp_mc; };

static int cmp_sensor_temp_desc(const void *pa, const void *pb)
{
    const struct sensor *a = pa, *b = pb;
    return (b->temp_mc > a->temp_mc) - (b->temp_mc < a->temp_mc);
}

/* ---- 3. Dispatch table: command name -> handler (no giant if/else chain) --- */
struct command {
    const char *name;
    int (*handler)(int arg);
};

static int cmd_double(int x) { return 2 * x; }
static int cmd_negate(int x) { return -x; }
static int cmd_square(int x) { return x * x; }

static const struct command commands[] = {
    {"double", cmd_double},
    {"negate", cmd_negate},
    {"square", cmd_square},
};

static int run_command(const char *name, int arg, int *out)
{
    for (size_t i = 0; i < sizeof commands / sizeof commands[0]; i++)
        if (strcmp(commands[i].name, name) == 0) {
            *out = commands[i].handler(arg);      /* indirect call */
            return 0;
        }
    return -1;
}

/* ---- 4. "Object" with an ops table — exactly the Linux driver pattern ------- */
struct device;
struct device_ops {
    int (*open)(struct device *dev);
    int (*read)(struct device *dev, int *val);
};
struct device {
    const char *name;
    const struct device_ops *ops;   /* shared, read-only vtable */
    int reg;
};
static int fake_open(struct device *d) { d->reg = 100; return 0; }
static int fake_read(struct device *d, int *val) { *val = d->reg++; return 0; }
static const struct device_ops fake_ops = { .open = fake_open, .read = fake_read };

/* ---- 5. State machine as an array of function pointers ---------------------- */
enum state { ST_IDLE, ST_RUN, ST_DONE, ST_COUNT };
typedef enum state (*state_fn)(int *ticks);
static enum state on_idle(int *t) { (*t)++; return ST_RUN; }
static enum state on_run(int *t)  { (*t)++; return *t >= 3 ? ST_DONE : ST_RUN; }
static enum state on_done(int *t) { (void)t; return ST_DONE; }
static const state_fn state_table[ST_COUNT] = { on_idle, on_run, on_done };

int main(void)
{
    binop_fn f = add;                  /* same as &add */
    assert(f(2, 3) == 5);
    assert((*f)(2, 3) == 5);           /* explicit deref: identical */
    assert(apply(mul, 4, 5) == 20);

    int nums[] = {42, -7, 19, 0, 1000000, -2000000000};
    size_t n = sizeof nums / sizeof nums[0];
    qsort(nums, n, sizeof nums[0], cmp_int_asc);
    printf("qsort ints: ");
    for (size_t i = 0; i < n; i++) printf("%d ", nums[i]);
    printf("\n");
    for (size_t i = 1; i < n; i++) assert(nums[i - 1] <= nums[i]);

    struct sensor s[] = {{"cpu", 61000}, {"gpu", 74000}, {"ddr", 48000}};
    qsort(s, 3, sizeof s[0], cmp_sensor_temp_desc);
    assert(strcmp(s[0].name, "gpu") == 0 && strcmp(s[2].name, "ddr") == 0);
    printf("hottest sensor: %s (%d m°C)\n", s[0].name, s[0].temp_mc);

    int key = 19, *found = bsearch(&key, nums, n, sizeof nums[0], cmp_int_asc);
    assert(found && *found == 19);

    int out = 0;
    assert(run_command("square", 7, &out) == 0 && out == 49);
    assert(run_command("negate", 7, &out) == 0 && out == -7);
    assert(run_command("reboot", 7, &out) == -1);
    printf("dispatch: square(7) = 49, unknown command rejected\n");

    struct device dev = {"fake0", &fake_ops, 0};
    int v = 0;
    assert(dev.ops->open(&dev) == 0);
    dev.ops->read(&dev, &v);
    assert(v == 100);
    dev.ops->read(&dev, &v);
    assert(v == 101);
    printf("device ops table: read %d after open\n", v);

    enum state st = ST_IDLE;
    int ticks = 0;
    while (st != ST_DONE)
        st = state_table[st](&ticks);
    assert(ticks == 3);
    printf("state machine finished after %d ticks\n", ticks);

    printf("All checks passed.\n");
    return 0;
}
