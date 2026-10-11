#ifndef PARSE_H
#define PARSE_H


#include "ast.h"
#include "symtab.h"

/*
 * Parse a preprocessed translation unit (text with `# line "file"` markers) into a tree. Scopes and declared names are
 * recorded in `symtab`, which the caller creates and frees. Returns NULL
 * after a syntax error.
 */
AST *parse(const char *text, SymTab *symtab);
void parse_cleanup(void);   /* after a fatal error: free the parser's leftovers */

#endif /* PARSE_H */
