/* C89 3.3.2.1 array subscripting: one operand shall be a pointer to an object type, the other integral */
int main(void) { int a[2], *p = a, *q = a; return p[q]; /* violation */ }
