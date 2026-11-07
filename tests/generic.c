/* C11 _Generic: selection by the controlling expression's type after lvalue conversion, typedef names, default, no evaluation */
#include <stdio.h>
#include <stddef.h>

typedef unsigned long ulong_t;
struct P { int x; };

#define type_name(x) _Generic((x), \
    char: "char", signed char: "signed char", unsigned char: "unsigned char", \
    short: "short", unsigned short: "unsigned short", \
    int: "int", unsigned: "unsigned", long: "long", ulong_t: "unsigned long", \
    long long: "long long", unsigned long long: "unsigned long long", \
    float: "float", double: "double", long double: "long double", \
    char *: "char *", const char *: "const char *", int *: "int *", \
    struct P: "struct P", void (*)(void): "function pointer", \
    default: "other")

#define abs_of(x) _Generic((x), int: iabs, double: dabs, default: labs_)(x)

static int iabs(int x) { return x < 0 ? -x : x; }
static double dabs(double x) { return x < 0 ? -x : x; }
static long labs_(long x) { return x < 0 ? -x : x; }
static void f(void) { }

static int calls;
static int side(void) { return ++calls; }

/* at file scope, in a constant expression */
static const int width = _Generic(1.0f, float: 4, double: 8);
static char sized[_Generic((short)0, short: 3, default: 1)];

int main(void) {
    char c = 0;
    const int ci = 1;
    int arr[3];
    struct P p = { 0 };
    const char *s = "x";
    short sh = 0;

    puts(type_name(c));
    puts(type_name(ci));            /* qualifiers dropped: int */
    puts(type_name(arr));           /* array decays: int * */
    puts(type_name("lit"));         /* char * */
    puts(type_name(s));
    puts(type_name(p));
    puts(type_name(f));             /* function decays */
    puts(type_name(sh));
    puts(type_name(sh + sh));       /* promoted: int */
    puts(type_name(1u));
    puts(type_name(1ul));
    puts(type_name(1ll));
    puts(type_name(1ull));
    puts(type_name(1.0f));
    puts(type_name(1.0));
    puts(type_name(1.0L));
    puts(type_name((unsigned char)1));
    puts(type_name((signed char)1));
    puts(type_name((unsigned short)1));
    puts(type_name(&p));            /* struct P *: default */
    puts(type_name((size_t)0));

    printf("%d %g %ld\n", abs_of(-3), abs_of(-2.5), abs_of(-7L));
    printf("%d\n", _Generic(side(), int: 10, default: 20));
    printf("calls %d\n", calls);    /* the controlling expression is not evaluated */
    printf("%d %zu\n", width, sizeof sized);
    _Generic(c, char: arr, default: &p.x)[0] = 5;     /* the result is an lvalue */
    printf("%d\n", arr[0]);
    return 0;
}
