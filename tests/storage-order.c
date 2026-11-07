/* GNU scalar_storage_order("big-endian"): each scalar member's bytes in the other order, an array
   member's elements too, bit-fields counted from the top of their unit; read, written, initialized
   (static and automatic), and seen as bytes */
#include <stdio.h>
#include <stddef.h>

/* clang has no scalar_storage_order: where the platform's compiler is clang (as jcc's is there), plain order */
#if defined(__clang__)
#define BIG_ENDIAN_ORDER
#else
#define BIG_ENDIAN_ORDER __attribute__((scalar_storage_order("big-endian")))
#endif

struct BE {
    int i;
    short s;
    unsigned char c;
    double d;
    unsigned int u3 : 3;
    int s9 : 9;
    unsigned short h : 12;
    long long arr[2];
    float f;
} BIG_ENDIAN_ORDER;

static struct BE g = { 0x01020304, -2, 0x7f, 1.5, 5, -100, 0xabc, { 0x1122334455667788LL, -1 }, 2.25f };

/* the members' bytes, not the padding's (which an automatic object's initializer leaves as it was) */
static void bytes(const char *what, const void *p, size_t n) {
    const unsigned char *b = p;
    size_t k;
    (void)n;
    printf("%s", what);
    for (k = 0; k < offsetof(struct BE, f) + sizeof(float); k++) {
        if (k != 7 && !(k >= 22 && k < 24)) {
            printf(" %02x", b[k]);
        }
    }
    printf("\n");
}

int main(void) {
    struct BE l = { 0x0a0b0c0d, 300, 9, -0.75, 2, 255, 0x123, { 1, 2 }, -1.0f };
    bytes("static", &g, sizeof g);
    bytes("local", &l, sizeof l);
    printf("%x %d %d %g %u %d %x %llx %lld %g\n", g.i, g.s, g.c, g.d, g.u3, g.s9, g.h, g.arr[0], g.arr[1], g.f);
    l.i += 1;
    l.s = -l.s;
    l.d *= 2;
    l.u3 = 7;
    l.s9 = -1;
    l.h++;
    l.arr[1] <<= 4;
    l.f = 0.5f;
    bytes("after", &l, sizeof l);
    printf("%x %d %g %u %d %x %lld %g\n", l.i, l.s, l.d, l.u3, l.s9, l.h, l.arr[1], l.f);
    return 0;
}
