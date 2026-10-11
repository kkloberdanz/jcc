/*
 * Tables of function pointers in structs, as a compiler with several
 * backends uses them: static const tables naming static functions, an
 * array of their addresses, a pointer selected at run time, and calls
 * through the members.
 */
#include <stdio.h>

struct Ops {
    const char *name;
    int (*add)(int, int);
    long (*width)(void);
    void (*show)(const char *, int);
};

static int add1(int a, int b) { return a + b; }
static int add2(int a, int b) { return 2 * a + b; }
static long width1(void) { return 32; }
static long width2(void) { return 64; }
static void show1(const char *s, int n) { printf("one %s %d\n", s, n); }
static void show2(const char *s, int n) { printf("two %s %d\n", s, n); }

static const struct Ops fast = { "fast", add1, width1, show1 };
static const struct Ops slow = { "slow", add2, width2, show2 };
static const struct Ops *const all[] = { &fast, &slow, 0 };
static const struct Ops *cur;

static int call_through(const struct Ops *o, int x) {
    return o->add(x, x) + (int)o->width();
}

int main(void) {
    int i;
    int total = 0;
    for (i = 0; all[i]; i++) {
        cur = all[i];
        printf("%s %d %ld\n", cur->name, cur->add(2, 3), cur->width());
        cur->show(cur->name, (*cur->add)(1, 1));
        all[i]->show("direct", all[i]->add(3, 3));
        total += call_through(all[i], i);
    }
    cur = all[1];
    printf("total %d\n", total);
    return cur->add(40, 2) + total;
}
