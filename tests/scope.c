/* Step 2 test: scopes, name spaces, and typedef names reused as identifiers. */

typedef int T;

struct S {
    T T;            /* member named T of type T */
    struct S *next;
};

enum E { A = sizeof(T), B };

int g(T x, int (*cb)(T));
int g(T x, int (*cb)(T)) { return cb(x); }

static int twice(T v) { return v * 2; }

T f(T a)
{
    int T;          /* variable shadows the typedef */
    T = a + 1;
    {
        struct S s;
        s.T = T;
        T = s.T;
    }
    goto done;
done:
    return T;
}

T h(void)
{
    return sizeof(T) * 0 + (T)1;    /* T is the typedef again */
}

int kr(a, b)
    int a;
    T b;
{
    return a + b;
}

int main(void)
{
    struct S;       /* new, incomplete struct S in this scope */
    unsigned T = 1; /* variable T of type unsigned */
    return f(2) + h() + g(1, twice) + kr(1, 1) + A + B + (int)T - 9;
}
