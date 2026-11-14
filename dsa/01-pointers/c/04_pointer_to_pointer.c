/*
 * 04_pointer_to_pointer.c — int **pp: when a function must change the caller's POINTER
 *
 * Concept : To modify an int in the caller you pass int*. To modify an int*
 *           in the caller (allocate it, re-point it, unlink a node) you pass
 *           int**. Also: argv is char**, an array of strings. And the
 *           "pointer to the link" trick removes special cases in linked lists.
 * Build   : make run P=dsa/01-pointers/c/04_pointer_to_pointer.c
 * Read    : ../README.md  (section "3.4 Pointer to pointer")
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* WRONG: p is a copy; the caller's pointer does not change, and the
 * allocation leaks. Kept here (not called) as the classic mistake.     */
static void alloc_wrong(int *p, int v)
{
    p = malloc(sizeof *p);
    if (p) *p = v;          /* writes the copy; caller never sees it -> leak */
    free(p);                /* (freed here only so this function is not a leak) */
}

/* RIGHT: receive the address of the caller's pointer. Caller frees. */
static int alloc_right(int **pp, int v)
{
    *pp = malloc(sizeof **pp);    /* *pp IS the caller's pointer variable */
    if (!*pp)
        return -1;
    **pp = v;                     /* **pp is the int it now points to */
    return 0;
}

/* ---- Linked list removal with a pointer to the link ------------------------ */
struct node { int val; struct node *next; };

static void push_front(struct node **head, int v)
{
    struct node *n = malloc(sizeof *n);
    assert(n);
    n->val = v;
    n->next = *head;
    *head = n;            /* changes the caller's head: needs node** */
}

/* Linus Torvalds' "good taste" example: no special case for the head.
 *
 *   head ──► [1|●]──► [2|●]──► [3|NULL]
 *   ^link     ^link=&(node1->next)
 *   Walk `link` along the CHAIN OF next-FIELDS; when *link is the victim,
 *   overwrite *link with victim->next. Works whether that field is `head`
 *   itself or some node's `next`.                                          */
static int remove_value(struct node **head, int v)
{
    for (struct node **link = head; *link; link = &(*link)->next) {
        if ((*link)->val == v) {
            struct node *victim = *link;
            *link = victim->next;   /* unlink: rewires head OR prev->next */
            free(victim);
            return 1;
        }
    }
    return 0;
}

static void free_list(struct node *n)
{
    while (n) { struct node *next = n->next; free(n); n = next; }
}

int main(int argc, char **argv)
{
    int *p = NULL;
    alloc_wrong(p, 5);
    assert(p == NULL);                       /* unchanged: the classic bug */

    assert(alloc_right(&p, 5) == 0 && p && *p == 5);
    int **pp = &p;
    /*
     *   pp                 p                  heap int
     *  +------------+     +------------+     +-----+
     *  | &p         |───► | 0x5020...  |───► |  5  |
     *  +------------+     +------------+     +-----+
     *   *pp == p,  **pp == 5
     */
    printf("pp=%p  *pp=%p  **pp=%d\n", (void *)pp, (void *)*pp, **pp);
    assert(*pp == p && **pp == 5);
    free(p);          /* owner: main allocated via alloc_right, main frees */
    p = NULL;

    /* argv: char ** = pointer to an array of char* (each a string), NULL-terminated. */
    printf("argc=%d, argv[0]=\"%s\", argv[argc]=%p (always NULL)\n", argc, argv[0], (void *)argv[argc]);
    assert(argv[argc] == NULL);

    /* An array of strings by hand. */
    const char *names[] = {"tidss", "vpac", "dmpac", NULL};
    const char **it = names;
    size_t total = 0;
    while (*it) total += strlen(*it++);     /* *it is a const char*, **it a char */
    assert(total == 5 + 4 + 5);

    /* Linked list with node** */
    struct node *head = NULL;
    for (int v = 5; v >= 1; v--)
        push_front(&head, v);             /* list: 1 2 3 4 5 */
    assert(remove_value(&head, 1));       /* removing the HEAD: no special case */
    assert(remove_value(&head, 3));       /* middle */
    assert(remove_value(&head, 5));       /* tail */
    assert(!remove_value(&head, 42));
    printf("after removals: ");
    for (struct node *n = head; n; n = n->next) printf("%d ", n->val);
    printf("\n");
    assert(head->val == 2 && head->next->val == 4 && head->next->next == NULL);
    free_list(head);

    printf("All checks passed.\n");
    return 0;
}
