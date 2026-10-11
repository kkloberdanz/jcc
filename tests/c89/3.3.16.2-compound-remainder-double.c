/* C89 3.3.16.2 compound assignment: %= needs integral operands like % */
int main(void) { double d = 5; d %= 2; /* violation */ return (int)d; }
