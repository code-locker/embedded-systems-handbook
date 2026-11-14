/*
 * 17_stack_smash_bug.c — INTENTIONAL BUG: a stack buffer overflow
 *
 * Concept : An unchecked copy of a 46-char string into a 16-byte local array
 *           overwrites whatever sits above it on the stack. Two tools catch
 *           it in different ways:
 *             - AddressSanitizer (default build) stops at the FIRST bad byte
 *               and reports "stack-buffer-overflow ... WRITE of size 1",
 *               with the line number.
 *             - The stack protector (no sanitizer; Ubuntu's gcc enables
 *               -fstack-protector-strong by default) only notices when the
 *               function RETURNS and the canary differs:
 *               "*** stack smashing detected ***: terminated" + SIGABRT.
 *           `make test` skips *_bug programs. This one is expected to crash.
 *
 * Which tool finds it:
 *   make run P=dsa/05-arrays-and-strings/c/17_stack_smash_bug.c   -> ASan report
 *   Canary instead (build by hand, without sanitizers):
 *     gcc -std=c11 -O0 -fstack-protector-strong \
 *         dsa/05-arrays-and-strings/c/17_stack_smash_bug.c -o /tmp/smash && /tmp/smash
 *
 * Build   : make run P=dsa/05-arrays-and-strings/c/17_stack_smash_bug.c
 * Read    : ../notes/strings.md  (section "Formatting and security")
 */
#include <stdio.h>

/* A hand-written strcpy, so the compiler cannot warn at compile time
 * (gcc would flag strcpy with a known-too-long literal). */
static void unchecked_copy(char *dst, const char *src)
{
    while ((*dst++ = *src++) != '\0')
        ;
}

static void vulnerable(const char *input)
{
    char buf[16];
    /*
     *   buf[0..15] | (padding) | CANARY | saved rbp | return address
     *   ^ copy starts here and keeps going for 47 bytes ------------>
     */
    printf("buf at %p, frame at %p (distance %td bytes)\n", (void *)buf,
           __builtin_frame_address(0),
           (char *)__builtin_frame_address(0) - buf);
    fflush(stdout); /* abort() would lose buffered output */
    unchecked_copy(buf, input); /* BUG: no size check */
    printf("copied: %s\n", buf); /* runs: the damage is not noticed yet */
    fflush(stdout);
}   /* <- canary check happens here, on return */

int main(void)
{
    const char *input = "0123456789ABCDEF<-16 bytes, the rest overflows";
    vulnerable(input);
    puts("if you see this, the overflow went undetected (no ASan, no canary)");
    return 0;
}
