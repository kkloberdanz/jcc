#ifndef LEX_H
#define LEX_H

#include "token.h"

/* Tokenize the whole input set with lex_set_text; ends with a TOK_EOF */
TokList *lex(void);
void lex_cleanup(void);     /* free the list a fatal error interrupted lex() on, if any */

/* the preprocessed translation unit, with `# line "file"` markers; the text is the caller's and is not kept */
void lex_set_text(const char *text);

#endif /* LEX_H */
