/* C11 _Thread_local and GNU __thread: each thread has its own object, initialized from the declaration */
#include <pthread.h>
#include <stdio.h>

_Thread_local int counter = 100;
__thread long big[64];
static __thread const char *who = "main";
extern __thread int counter;

struct S { int a; double d; char s[8]; };
static _Thread_local struct S rec = { 7, 2.5, "rec" };

static int bump(void) {
    static __thread int calls;     /* a thread-local static at block scope */
    return ++calls;
}

static int *counter_addr(void) { return &counter; }

static void *worker(void *arg) {
    long id = (long)arg;
    int i;
    who = id == 1 ? "one" : "two";
    for (i = 0; i < 1000; i++) {
        counter++;
        big[i % 64] += id;
    }
    bump();
    bump();
    rec.a += (int)id;
    printf("%s %d %ld %d %d %s %d\n", who, counter, big[5], bump(), rec.a, rec.s, *counter_addr() == counter);
    return counter_addr();
}

int main(void) {
    pthread_t t1, t2;
    void *r1, *r2;
    counter = 5;
    pthread_create(&t1, NULL, worker, (void *)1);
    pthread_join(t1, &r1);
    pthread_create(&t2, NULL, worker, (void *)2);
    pthread_join(t2, &r2);
    printf("%s %d %ld %d %d %g %d\n", who, counter, big[5], bump(), rec.a, rec.d, r1 != (void *)&counter && r2 != (void *)&counter);
    return 0;
}
