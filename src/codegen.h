#ifndef CODEGEN_H
#define CODEGEN_H

#include <stdio.h>

#include "ast.h"
#include "symtab.h"

/*
 * Emit assembly for a translation unit that passed semantic analysis.
 * Diagnostics go to stderr (src/diag.h); returns the number of errors,
 * which are all "not supported yet" at this stage.
 */
int codegen(AST *tu, SymTab *symtab, FILE *output);

#endif /* CODEGEN_H */
