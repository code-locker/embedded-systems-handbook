/*
 * 01_compilation_pipeline.c — what the preprocessor does before the compiler sees your code
 *
 * Concept : C source goes through 4 stages: preprocess (.i) -> compile (.s)
 *           -> assemble (.o) -> link (executable). This program is built to be
 *           pushed through each stage by hand so you can SEE every stage.
 * Build   : make run P=dsa/00-foundations/c/01_compilation_pipeline.c
 * Read    : ../README.md  (section "How it works -> 1. The compilation pipeline")
 *
 * Try every stage yourself (from the repo root, in a scratch dir):
 *   gcc -E  dsa/00-foundations/c/01_compilation_pipeline.c -o /tmp/p.i   # 1. preprocess
 *   gcc -S  /tmp/p.i -o /tmp/p.s                                         # 2. compile -> asm
 *   gcc -c  /tmp/p.s -o /tmp/p.o                                         # 3. assemble -> object
 *   gcc     /tmp/p.o -o /tmp/p                                           # 4. link -> executable
 *   tail -40 /tmp/p.i      # macros are GONE, headers are pasted in
 *   grep -n call /tmp/p.s  # see the calls to printf/puts
 *   nm /tmp/p.o            # 'U printf' = undefined, the linker must find it
 *   gcc -save-temps -c dsa/00-foundations/c/01_compilation_pipeline.c   # keeps .i .s .o
 */
#include <assert.h>
#include <stdio.h>

/* ---- object-like macro: pure text substitution, no type, no memory ------- */
#define BUFFER_LEN 16

/* ---- function-like macros: the classic trap --------------------------------
 * The preprocessor pastes TEXT. It knows nothing about C precedence.
 *   BAD_SQUARE(1 + 2)  ->  1 + 2 * 1 + 2  = 5     (not 9!)
 *   GOOD_SQUARE(1 + 2) -> ((1 + 2) * (1 + 2)) = 9
 * Rule: parenthesise every parameter AND the whole body.                     */
#define BAD_SQUARE(x)  x * x
#define GOOD_SQUARE(x) ((x) * (x))

/* ---- # (stringize) and ## (token paste) -------------------------------------
 * #x   turns the argument into a string literal: SHOW(a+b) -> "a+b"
 * a##b glues two tokens into one identifier:   CAT(reg_, ctrl) -> reg_ctrl  */
#define STR(x)    #x
#define CAT(a, b) a##b
#define SHOW_INT(expr) printf("  %-22s = %d\n", #expr, (expr))

/* ---- conditional compilation: code that is removed before compiling ------- */
#ifndef BOARD
#define BOARD "host-pc"      /* override with: gcc -DBOARD='"bbai64"' ...    */
#endif

#if defined(__aarch64__)
#define ARCH_NAME "aarch64 (like the TDA4VM A72 cores)"
#elif defined(__x86_64__)
#define ARCH_NAME "x86_64"
#else
#define ARCH_NAME "other"
#endif

int main(void)
{
    int reg_ctrl = 0x42;          /* used through CAT(reg_, ctrl) below */
    char buf[BUFFER_LEN];         /* the compiler only ever sees char buf[16] */

    printf("Stage 1 (preprocessor) results, as seen at run time:\n");
    SHOW_INT(BAD_SQUARE(1 + 2));
    SHOW_INT(GOOD_SQUARE(1 + 2));
    SHOW_INT(CAT(reg_, ctrl));
    SHOW_INT((int)sizeof buf);

    /* Why 5? Because the text became 1 + 2 * 1 + 2. */
    assert(BAD_SQUARE(1 + 2) == 5);
    assert(GOOD_SQUARE(1 + 2) == 9);
    assert(CAT(reg_, ctrl) == 0x42);
    assert(sizeof buf == 16);

    /* Stringize happens before compilation: the expression never runs. */
    const char *s = STR(GOOD_SQUARE(y));
    printf("  STR(GOOD_SQUARE(y))    = \"%s\"  (args of # are NOT expanded)\n", s);
    assert(s[0] == 'G');

    /* Predefined macros: filled in by the preprocessor, per file. */
    printf("\nPredefined macros:\n");
    printf("  __FILE__     = %s\n", __FILE__);
    printf("  __LINE__     = %d\n", __LINE__);
    printf("  __func__     = %s   (not a macro: a static const char[] made by the compiler)\n", __func__);
    printf("  __STDC_VERSION__ = %ldL (201112L = C11)\n", (long)__STDC_VERSION__);
    printf("  BOARD        = %s\n", BOARD);
    printf("  architecture = %s\n", ARCH_NAME);
    assert(__STDC_VERSION__ >= 201112L);

    printf("\nAll checks passed. Now run the gcc -E/-S/-c commands in the header.\n");
    return 0;
}
