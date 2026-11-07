/* GNU label addresses (&&label), computed goto (goto *p), label differences in a static
   table, and local labels (__label__) in a statement expression a macro expands twice */
#include <stdio.h>

enum { PUSH, ADD, MUL, DUP, PRINT, HALT };

static int run(const int *code) {
    static void *const dispatch[] = { &&op_push, &&op_add, &&op_mul, &&op_dup, &&op_print, &&op_halt };
    int stack[16], sp = 0, steps = 0;
#define NEXT goto *dispatch[*code++]
    NEXT;
op_push:
    stack[sp++] = *code++; steps++; NEXT;
op_add:
    sp--; stack[sp - 1] += stack[sp]; steps++; NEXT;
op_mul:
    sp--; stack[sp - 1] *= stack[sp]; steps++; NEXT;
op_dup:
    stack[sp] = stack[sp - 1]; sp++; steps++; NEXT;
op_print:
    printf("%d\n", stack[sp - 1]); steps++; NEXT;
op_halt:
    return steps * 1000 + stack[sp - 1];
#undef NEXT
}

static int relative(int which) {
    static const int offsets[] = { &&one - &&base, &&two - &&base, &&three - &&base };
    int r = 0;
    goto *(&&base + offsets[which]);
base:
    return -1;
one:
    r += 1;
two:
    r += 10;
three:
    r += 100;
    return r;
}

#define CLAMP(x, lo) ({ __label__ done; int v_ = (x); if (v_ >= (lo)) goto done; v_ = (lo); done: v_; })

static int pick(int n) {
    void *where = n ? &&nonzero : &&zero;
    if (where == &&zero) {
        n = -1;
    }
    goto *where;
zero:
    return n * 100;
nonzero:
    return n;
}

int main(void) {
    static const int prog[] = { PUSH, 6, DUP, MUL, PUSH, 6, ADD, PRINT, PUSH, 2, MUL, PRINT, HALT };
    printf("%d\n", run(prog));
    printf("%d %d %d\n", relative(0), relative(1), relative(2));
    printf("%d %d\n", CLAMP(3, 5), CLAMP(9, 5));
    printf("%d %d\n", pick(0), pick(7));
    return 0;
}
