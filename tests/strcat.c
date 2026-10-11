/* adjacent string literals are one literal, also inside sizeof and initializers */
#include <stdio.h>
#define NAME "kcc"
static const char banner[] = "usage: " NAME " [-h]\n" "  -h   help";
static char fixed[8] = "ab" "cd";
int main(void) {
    puts("a" "b" "c");
    puts(banner);
    printf("%d %d %s %c\n", (int)sizeof("ab" "cd"), (int)sizeof(banner), fixed, "x" "\0y"[0]);
    return (int)sizeof "a" "" "b";
}
