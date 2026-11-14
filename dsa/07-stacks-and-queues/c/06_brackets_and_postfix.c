/*
 * 06_brackets_and_postfix.c — stack applications: balanced brackets,
 *                             infix -> postfix (shunting-yard), evaluation
 *
 * Concept : A stack remembers "what is still open" — the most recently
 *           opened thing must close first. That one idea checks brackets,
 *           converts infix to postfix (Dijkstra's shunting-yard), and
 *           evaluates postfix (how calculators, the JVM and Forth work).
 * Build   : make run P=dsa/07-stacks-and-queues/c/06_brackets_and_postfix.c
 * Read    : ../README.md  (section "Applications")
 */
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "CHECK failed: %s  (%s:%d)\n", #cond, __FILE__,  \
                    __LINE__);                                               \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

#define MAXS 128

/* Small fixed stacks, one per element type; inputs are bounded. */
typedef struct {
    char d[MAXS];
    int n;
} CharStack;
typedef struct {
    long d[MAXS];
    int n;
} LongStack;

/* ------------------------------------------------------------------------ */
/* 1. Balanced brackets — O(n) time, O(n) space                             */
/* ------------------------------------------------------------------------ */
/*
 *   input:  {  [  (  )  ]  }
 *   stack:  {  {[ {[( {[  {   (empty)   -> balanced
 *
 *   input:  (  [  )  ]
 *   stack:  (  ([  sees ')' but top is '['  -> mismatch
 *
 *  Three ways to fail: a closer with an empty stack, a closer that does not
 *  match the top, and openers left over at the end.
 */
static bool balanced(const char *s)
{
    CharStack st = {.n = 0};
    for (; *s; s++) {
        char c = *s;
        if (c == '(' || c == '[' || c == '{') {
            if (st.n == MAXS)
                return false;
            st.d[st.n++] = c;
        } else if (c == ')' || c == ']' || c == '}') {
            char want = c == ')' ? '(' : c == ']' ? '[' : '{';
            if (st.n == 0 || st.d[st.n - 1] != want)
                return false;
            st.n--;
        } /* other characters are ignored */
    }
    return st.n == 0; /* leftovers = unclosed openers */
}

/* ------------------------------------------------------------------------ */
/* 2. Infix -> postfix with the shunting-yard algorithm — O(n)              */
/* ------------------------------------------------------------------------ */
/*
 * Rules (operands are single digits or multi-digit numbers):
 *   number        -> straight to output
 *   '('           -> push
 *   ')'           -> pop to output until '(' ; discard the '('
 *   operator op   -> while top is an operator with
 *                       higher precedence, or equal precedence and op is
 *                       left-associative:  pop it to output
 *                    then push op
 *   end           -> pop everything to output
 *
 * Trace  3 + 4 * 2 - 1:
 *   tok  out        stack
 *   3    3
 *   +    3          +
 *   4    3 4        +
 *   *    3 4        + *     (* beats +, so + stays)
 *   2    3 4 2      + *
 *   -    3 4 2 * +  -       (* and + both >= -, pop both)
 *   1    3 4 2 * + 1 -
 *
 * '^' is right-associative: 2^3^2 = 2^(3^2), so equal precedence does NOT
 * pop for '^'.
 */
static int prec(char op)
{
    switch (op) {
    case '+':
    case '-':
        return 1;
    case '*':
    case '/':
        return 2;
    case '^':
        return 3;
    default:
        return 0; /* '(' has lowest priority: never popped by an operator */
    }
}

static bool right_assoc(char op) { return op == '^'; }

/* Writes tokens separated by single spaces into out. Returns false on a
 * syntax error (unbalanced parentheses). */
static bool to_postfix(const char *in, char *out, size_t outsz)
{
    CharStack ops = {.n = 0};
    size_t o = 0;
#define EMIT(ch)                                                             \
    do {                                                                     \
        if (o + 2 >= outsz)                                                  \
            return false;                                                    \
        out[o++] = (ch);                                                     \
    } while (0)

    for (const char *p = in; *p; p++) {
        char c = *p;
        if (isspace((unsigned char)c))
            continue;
        if (isdigit((unsigned char)c)) {
            while (isdigit((unsigned char)*p))
                EMIT(*p++);
            p--; /* the for loop will advance again */
            EMIT(' ');
        } else if (c == '(') {
            ops.d[ops.n++] = c;
        } else if (c == ')') {
            while (ops.n > 0 && ops.d[ops.n - 1] != '(') {
                EMIT(ops.d[--ops.n]);
                EMIT(' ');
            }
            if (ops.n == 0)
                return false; /* ')' without '(' */
            ops.n--;          /* drop the '(' */
        } else {
            while (ops.n > 0) {
                char top = ops.d[ops.n - 1];
                if (top == '(')
                    break;
                if (prec(top) > prec(c) ||
                    (prec(top) == prec(c) && !right_assoc(c))) {
                    EMIT(top);
                    EMIT(' ');
                    ops.n--;
                } else {
                    break;
                }
            }
            ops.d[ops.n++] = c;
        }
    }
    while (ops.n > 0) {
        char top = ops.d[--ops.n];
        if (top == '(')
            return false; /* '(' never closed */
        EMIT(top);
        EMIT(' ');
    }
    if (o > 0)
        o--; /* drop trailing space */
    out[o] = '\0';
#undef EMIT
    return true;
}

/* ------------------------------------------------------------------------ */
/* 3. Evaluate postfix — O(n)                                               */
/* ------------------------------------------------------------------------ */
/*
 *  "3 4 2 * + 1 -"
 *   3 -> [3]   4 -> [3 4]   2 -> [3 4 2]
 *   * -> pop 2, pop 4, push 8   -> [3 8]
 *   + -> pop 8, pop 3, push 11  -> [11]
 *   1 -> [11 1]
 *   - -> pop 1 (b), pop 11 (a), push a-b = 10
 *
 *  ORDER: the first pop is the RIGHT operand. Getting this backwards makes
 *  '-' and '/' silently wrong.
 */
static bool eval_postfix(const char *s, long *result)
{
    LongStack st = {.n = 0};
    for (const char *p = s; *p; p++) {
        if (*p == ' ')
            continue;
        if (isdigit((unsigned char)*p)) {
            long v = 0;
            while (isdigit((unsigned char)*p))
                v = v * 10 + (*p++ - '0');
            p--;
            st.d[st.n++] = v;
            continue;
        }
        if (st.n < 2)
            return false; /* operator with too few operands */
        long b = st.d[--st.n];
        long a = st.d[--st.n];
        long r;
        switch (*p) {
        case '+': r = a + b; break;
        case '-': r = a - b; break;
        case '*': r = a * b; break;
        case '/':
            if (b == 0)
                return false;
            r = a / b;
            break;
        case '^':
            r = 1;
            for (long i = 0; i < b; i++)
                r *= a;
            break;
        default:
            return false;
        }
        st.d[st.n++] = r;
    }
    if (st.n != 1)
        return false;
    *result = st.d[0];
    return true;
}

static void check_expr(const char *infix, const char *want_post, long want)
{
    char post[256];
    long v = 0;
    CHECK(to_postfix(infix, post, sizeof post));
    CHECK(eval_postfix(post, &v));
    printf("  %-22s -> %-22s = %ld\n", infix, post, v);
    CHECK(strcmp(post, want_post) == 0);
    CHECK(v == want);
}

int main(void)
{
    printf("balanced brackets:\n");
    const char *good[] = {"", "()", "{[()]}", "a(b[c]{d}e)f", "(()(()))"};
    const char *bad[] = {"(", ")", "(]", "([)]", "{{}", "}{"};
    for (size_t i = 0; i < sizeof good / sizeof *good; i++) {
        printf("  %-14s yes\n", good[i]);
        CHECK(balanced(good[i]));
    }
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        printf("  %-14s no\n", bad[i]);
        CHECK(!balanced(bad[i]));
    }

    printf("infix -> postfix -> value:\n");
    check_expr("3 + 4 * 2 - 1", "3 4 2 * + 1 -", 10);
    check_expr("(3 + 4) * 2", "3 4 + 2 *", 14);
    check_expr("100 / 10 / 2", "100 10 / 2 /", 5);     /* left-assoc */
    check_expr("2 ^ 3 ^ 2", "2 3 2 ^ ^", 512);         /* right-assoc */
    check_expr("((1+2)*(3+4))-5", "1 2 + 3 4 + * 5 -", 16);

    char post[64];
    CHECK(!to_postfix("(1+2", post, sizeof post)); /* unclosed */
    CHECK(!to_postfix("1+2)", post, sizeof post)); /* stray ')' */
    long v;
    CHECK(!eval_postfix("1 +", &v));
    CHECK(!eval_postfix("4 0 /", &v));

    printf("06_brackets_and_postfix: all checks passed\n");
    return 0;
}
