/* the GNU __atomic_* and __sync_* builtins on objects of every size, generic and _n forms */
#include <stdio.h>
struct S { short a, b; };
int main(void) {
    int x = 9, r;
    char *p = "abcdef", *q;
    struct S s = { 1, 2 }, s2 = { 3, 4 }, s3;
    unsigned u = 0xf0;
    double d = 1.5, dv = 2.25;
    r = __sync_val_compare_and_swap(&x, 9, 7); printf("%d ", r);
    r = __sync_bool_compare_and_swap(&x, 7, 8); printf("%d %d\n", r, x);
    r = __atomic_exchange_n(&x, 3, 5); printf("%d ", r);
    r = __atomic_load_n(&x, 5); printf("%d ", r);
    r = __sync_lock_test_and_set(&x, 11); printf("%d %d\n", r, x);
    __atomic_store(&d, &dv, 5); dv = 0; __atomic_load(&d, &dv, 5);
    q = __atomic_fetch_add(&p, 2, 5);
    printf("%g %g %s %s\n", d, dv, q, p);
    __atomic_exchange(&s, &s2, &s3, 5);
    printf("%d %d %d %d\n", s.a, s.b, s3.a, s3.b);
    __atomic_store(&s2, &s3, 5);
    r = __atomic_compare_exchange(&s, &s2, &s3, 0, 5, 5); printf("%d %d %d\n", r, s.a, s2.a);
    printf("%u ", __atomic_fetch_and(&u, 0x3c, 5));
    printf("%u ", __atomic_or_fetch(&u, 1, 5));
    printf("%u ", __atomic_xor_fetch(&u, 0xff, 5));
    printf("%u ", __atomic_nand_fetch(&u, 0xf, 5));
    printf("%u ", __atomic_fetch_nand(&u, 0x3, 5)); printf("%u\n", u);
    return 0;
}
