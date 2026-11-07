/* GNU asm statements: empty templates as barriers, operands in registers, memory, tied
   (matching and named), constants, floating point, clobbered callee-saved registers, and
   asm goto; then instructions of each target's own (jcc: user asm, which the host's assembler takes) */
#include <stdio.h>

struct S { int a, b; } gs = { 1, 2 };
static int calls;

static int *counted(int *p) {
    calls++;
    return p;
}

int main(void) {
    int x = 5, y = 7, z = 0, w;
    long l = 40;
    double d = 1.5;
    unsigned char c = 200;
    __asm__ __volatile__("" ::: "memory");
    __asm__("");
    __asm__ __volatile__("" : "+r"(x));
    __asm__ __volatile__("" : "+r"(*counted(&y)));
    __asm__("" : "=r"(w) : "0"(x + 1));
    __asm__("" : "=g"(z), "+m"(gs) : "0"(x == 0 ? 3 : 4));
    __asm__ __volatile__("" : : "r"(&l), "g"(d), "i"(42) : "memory");
    __asm__("" : "=m"(gs.b) : "m"(gs.a));
    printf("%d %d %d %d %d %d\n", x, y, z, w, gs.b, calls);
#if defined(__x86_64__)
    __asm__("addl %2, %0" : "=r"(z) : "0"(x), "r"(y));
    __asm__("addq $2, %0" : "+m"(l));
    __asm__("incb %0" : "+q"(c));
    __asm__("movl %[in], %[out]\n\tshll $1, %[out]" : [out] "=&r"(y) : [in] "g"(x));
    __asm__("addsd %1, %0" : "+x"(d) : "x"(2.25));
    __asm__("xchgl %%ebx, %0; xchgl %%ebx, %0" : "+r"(x) : : "rbx");
    __asm__("movl %1, %%eax; addl %%eax, %0" : "+b"(z) : "i"(100) : "eax");
    {
        /* the x87 stack: its top (t) and the one under it (u) */
        long double ly = 1.0L, lx = 3.0L, lr, ls;
        double de = 2.0;
        __asm__("fpatan" : "=t"(lr) : "0"(lx), "u"(ly) : "st(1)");
        __asm__("fsqrt" : "=t"(de) : "0"(de));
        __asm__("fadd %%st(1), %%st" : "=t"(ls) : "0"(lx), "u"(ly));
        printf("%.15Lg %.15g %Lg\n", lr, de, ls);
    }
#elif defined(__aarch64__)
    __asm__("add %w0, %w1, %w2" : "=r"(z) : "r"(x), "r"(y));
    z += 100;
    __asm__("ldr x9, %0\n\tadd x9, x9, #2\n\tstr x9, %0" : "+m"(l) : : "x9");
    c++;
    __asm__("lsl %w0, %w1, #1" : "=&r"(y) : "r"(x));
    __asm__("fadd %d0, %d0, %d1" : "+w"(d) : "w"(2.25));
    __asm__("mov x19, %1\n\tmov %0, x19" : "=r"(x) : "r"((long)x) : "x19");
    printf("0.321750554396642 1.4142135623731 4\n");
#else
    z = 112; l += 2; c++; y = x * 2; d += 2.25;
#endif
    printf("%d %d %d %ld %d %g\n", x, y, z, l, c, d);
    __asm__ goto("" : : : : out);
    printf("falls through\n");
out:
    printf("done\n");
    return 0;
}
