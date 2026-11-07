/* C11 _Atomic and <stdatomic.h>, under contention from four threads */
#include <stdio.h>
#include <stdatomic.h>
#include <pthread.h>
static _Atomic int ai;
static _Atomic(long) al;
static atomic_uint au = ATOMIC_VAR_INIT(0);
static int *_Atomic ap;
static int arr[8];
static atomic_flag flag = ATOMIC_FLAG_INIT;
static long plain;
static atomic_char ac;
static void *work(void *arg) {
    int i;
    (void)arg;
    for (i = 0; i < 50000; i++) {
        ai++;
        al += 3;
        atomic_fetch_add(&au, 2);
        atomic_fetch_sub_explicit(&au, 1, memory_order_relaxed);
        --ac;
        while (atomic_flag_test_and_set(&flag)) {
        }
        plain++;
        atomic_flag_clear(&flag);
    }
    return NULL;
}
int main(void) {
    pthread_t t[4];
    int i, e;
    _Atomic short s = 5;
    for (i = 0; i < 4; i++) pthread_create(&t[i], NULL, work, NULL);
    for (i = 0; i < 4; i++) pthread_join(t[i], NULL);
    printf("%d %ld %u %ld %d\n", ai, al, atomic_load(&au), plain, (int)ac);
    ap = arr;
    ap++;
    ap += 2;
    printf("%d\n", (int)(atomic_load(&ap) - arr));
    atomic_store(&ai, 10);
    e = 10;
    printf("%d ", atomic_compare_exchange_strong(&ai, &e, 20));
    printf("%d ", atomic_compare_exchange_weak(&ai, &e, 30));
    printf("%d %d\n", e, atomic_exchange(&ai, 1));
    s *= 3;
    s <<= 1;
    i = s++;
    printf("%d %d %d %d\n", i, s, atomic_is_lock_free(&al), (int)sizeof(atomic_flag));
    atomic_thread_fence(memory_order_seq_cst);
    atomic_signal_fence(memory_order_acquire);
    return 0;
}
