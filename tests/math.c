/* The original codegen test: constant folding at file scope. Exits 14. */
int x = 5 + 1 * 4 + 2 * 3 - 1;

int main(void) {
    return x;
}
