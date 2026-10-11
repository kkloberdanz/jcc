/*
 * ANSI C Yacc grammar
 * In 1985, Jeff Lee published his Yacc grammar (which is accompanied by a
 * matching Lex specification) for the April 30, 1985 draft version of the
 * ANSI C standard.  Tom Stockfisch reposted it to net.sources in 1987; that
 * original, as mentioned in the answer to question 17.25 of the comp.lang.c
 * FAQ, can be ftp'ed from
 * ftp.uu.net, file usenet/net.sources/ansi.c.grammar.Z.
 *
 * Jutta Degener, 1995
 */

%{
#define YYPARSER

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "src/lex.h"
#include "src/ast.h"
#include "src/symtab.h"
#include "src/parse.h"
#include "src/diag.h"

#define YYERROR_VERBOSE
#define YYSTYPE AST *

/* a location is the position of the first token of the construct */
#define YYLTYPE Loc
#define YYLTYPE_IS_DECLARED 1
#define YYLLOC_DEFAULT(Cur, Rhs, N) \
    do { \
        (Cur) = (N) ? YYRHSLOC(Rhs, 1) : YYRHSLOC(Rhs, 0); \
    } while (0)

static int yylex(void);
static void yyerror(const char *msg);
static Loc token_loc(void);
static void fatal_error(const char *msg);

static TokList *tokens = NULL;
static TokList *current_token = NULL;
static AST *tree = NULL;
static int parse_failed = 0;

/* scopes and declared names; owned by the caller of parse() */
static SymTab *symtab = NULL;

/*
 * `#pragma pack`: the maximum member alignment in effect, 0 for none, and
 * the stack behind pack(push) and pack(pop). Every struct body records
 * the value in effect when its `{` is read.
 */
static int cur_pack = 0;
static int pack_stack[64];
static int pack_depth = 0;

/* the lexer hands a `#pragma pack(...)` over as one token whose text starts at `pack` */
static void apply_pragma(AST *t) {
    const char *p = t->u.text + 4;
    int push = 0;
    int pop = 0;
    int value = 0;
    int has_value = 0;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '(') {
        return;
    }
    p++;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        if (*p == ')' || *p == '\0') {
            break;
        }
        if (strncmp(p, "push", 4) == 0 && !isalnum((unsigned char)p[4]) && p[4] != '_') {
            push = 1;
            p += 4;
        } else if (strncmp(p, "pop", 3) == 0 && !isalnum((unsigned char)p[3]) && p[3] != '_') {
            pop = 1;
            p += 3;
        } else if (isdigit((unsigned char)*p)) {
            value = (int)strtol(p, (char **)&p, 10);
            has_value = 1;
        } else if (isalpha((unsigned char)*p) || *p == '_') {
            /* a label for push/pop, or `show`: ignored */
            while (isalnum((unsigned char)*p) || *p == '_') {
                p++;
            }
        } else {
            return;
        }
    }
    if (has_value && value != 1 && value != 2 && value != 4 && value != 8 && value != 16) {
        return;
    }
    if (push) {
        if (pack_depth < (int)(sizeof pack_stack / sizeof pack_stack[0])) {
            pack_stack[pack_depth] = cur_pack;
        }
        pack_depth++;
        if (has_value) {
            cur_pack = value;
        }
    } else if (pop) {
        if (pack_depth > 0) {
            pack_depth--;
            cur_pack = pack_depth < (int)(sizeof pack_stack / sizeof pack_stack[0]) ? pack_stack[pack_depth] : 0;
        } else {
            cur_pack = 0;
        }
    } else {
        cur_pack = has_value ? value : 0;
    }
}

/*
 * Set while parsing a declaration that starts with `typedef`, so that the
 * names it declares can be recorded as type names.
 */
static int in_typedef = 0;

/*
 * Typedef names as identifiers.
 *
 * Once a declaration's specifiers contain a type specifier, a following
 * typedef name is the declarator's identifier, not a type: `typedef int T;
 * unsigned T;` declares a variable T. The lexer returns IDENTIFIER rather
 * than TYPE_NAME in that case. The flag is saved and cleared wherever a
 * nested expression or declaration starts inside a declaration (array
 * sizes, initializers, parameter lists, struct bodies) and restored after.
 */
static int spec_stack[256];
static int spec_sp = 0;
#define spec_seen_type (spec_stack[spec_sp])
static void spec_push(void);
static void spec_pop(void);
static void spec_reset(void);

/* the token most recently returned by yylex, see translate_tok */
static int prev_tok = 0;
static int bad_token_reported = 0;  /* yylex reported the invalid token itself; yyerror stays quiet */
static int abort_requested = 0;     /* a helper hit an error it cannot recover from: yylex ends the input */
static int after_kr_list = 0;   /* the `)` just closed a K&R identifier list: a declaration list may follow */

/*
 * Nodes built by mid-rule actions. Bison runs no %destructor on a
 * mid-rule value when a syntax error unwinds the rule, so each is kept
 * here until the rule's final action takes it over, and parse_cleanup()
 * frees what is left. A component the mid-rule action consumed is set
 * to NULL on the stack, or the destructor would free it a second time.
 */
static AST *pending[64];
static int npending = 0;

static AST *pending_push(AST *node) {
    if (npending < (int)(sizeof(pending) / sizeof(pending[0]))) {
        pending[npending++] = node;
    }
    return node;
}

static AST *pending_pop(AST *node) {
    if (npending > 0 && pending[npending - 1] == node) {
        npending--;
    }
    return node;
}

static void pending_free(void) {
    while (npending > 0) {
        ast_free(pending[--npending]);
    }
}

/* the symbol the most recent identifier token refers to, if any */
static Symbol *ident_sym = NULL;

/*
 * Set between the start of a function definition and its `{`, so that the
 * body reuses the parameters' scope instead of opening a new one.
 */
static int function_body_pending = 0;
static Symbol *current_function = NULL;

static void declare_declarator(AST *init_decl);
static void declare_param(AST *param);
static void declare_kr_param(AST *ident);
static void declare_member(AST *struct_decl);
static void declare_enumerator(AST *enumerator);
static void declare_label(AST *label);
static Symbol *declare_tag(AST *spec, int in_current_scope);
static void tag_defined(AST *spec);
static void params_begin(Loc loc);
static void params_end(AST *func);
static void func_begin(AST *declarator);
static void func_end(AST *fdef);
static void block_begin(Loc loc);
static void block_end(AST *block);
/* close a for's scope; with items, wrap them in a block that owns the scope */
static AST *for_end(ASTList *items);

/* steal the name out of an identifier token's node and free the node */
static char *take_name(AST *ident);

/* pull the expression (possibly NULL) out of an expression statement */
static AST *unwrap_expr_stmt(AST *stmt);

char *strdup(const char *s);

%}

%define parse.error verbose
%define parse.lac full
%locations

%token IDENTIFIER CONSTANT STRING_LITERAL SIZEOF
%token PTR_OP INC_OP DEC_OP LEFT_OP RIGHT_OP LE_OP GE_OP EQ_OP NE_OP
%token AND_OP OR_OP MUL_ASSIGN DIV_ASSIGN MOD_ASSIGN ADD_ASSIGN
%token SUB_ASSIGN LEFT_ASSIGN RIGHT_ASSIGN AND_ASSIGN
%token XOR_ASSIGN OR_ASSIGN TYPE_NAME

%token TYPEDEF EXTERN STATIC AUTO REGISTER
%token CHAR SHORT INT LONG SIGNED UNSIGNED FLOAT DOUBLE CONST VOLATILE VOID
%token STRUCT UNION ENUM ELLIPSIS ENUM_COLON

%token CASE DEFAULT IF ELSE SWITCH WHILE DO FOR GOTO CONTINUE BREAK RETURN

%token BUILTIN_VA_LIST BUILTIN_VA_ARG INLINE RESTRICT NULLABILITY INT128 UINT128 BOOL FLOAT16 BUILTIN_OFFSETOF COMPLEX
%token REAL_OP IMAG_OP
%token BAD_TOKEN

/*
 * A syntax error discards the values on the stack and the lookahead: free
 * what they own. The tree itself is also "discarded" when the parse
 * succeeds, and parse() owns it. Listed are the tokens that carry a value
 * and every nonterminal, so bison can also warn about a rule that drops
 * a value; the helper rules that only act on the parser's state (pragma,
 * param_lparen, lbrack, block_begin, for_begin) carry none.
 */
%destructor {
    if ($$ != tree) {
        ast_free($$);
    }
} IDENTIFIER CONSTANT STRING_LITERAL TYPE_NAME ATTRIBUTE ASM PRAGMA
    abstract_declarator additive_expression and_expression any_identifier argument_expression_list array_qualifiers assignment_expression assignment_operator
    attribute_list basic_type_specifier block_item block_item_list cast_expression compound_statement conditional_expression
    constant_expression declaration declaration_list declaration_specifiers declarator designated_initializer designator designator_list
    direct_abstract_declarator direct_declarator enum_keyword enum_specifier enum_underlying enum_underlying_list enumerator enumerator_list
    enumerators equality_expression exclusive_or_expression expression expression_statement external_declaration function_body
    function_definition gnu_attribute gnu_attribute_list identifier_list inclusive_or_expression init_declarator init_declarator_list initializer
    initializer_list iteration_statement jump_statement labeled_statement logical_and_expression logical_or_expression multiplicative_expression
    offsetof_member parameter_declaration parameter_list parameter_type_list pointer postfix_expression 
    primary_expression relational_expression selection_statement shift_expression specifier_qualifier_list statement storage_class_specifier string_literal
    struct_declaration struct_declaration_list struct_declarator struct_declarator_list struct_or_union struct_or_union_keyword struct_or_union_specifier translation_unit
    type_name type_qualifier type_qualifier_list type_specifier unary_expression unary_operator
%token ATTRIBUTE ASM PRAGMA

/*
 * After a declarator, prefer shifting a trailing __attribute__ or __asm
 * over reducing the declarator (which would only make sense for an old
 * style parameter declaration list starting with __attribute__).
 */
%nonassoc DECLARATOR_END
%nonassoc ATTRIBUTE ASM

%start translation_unit
%%

primary_expression
    : IDENTIFIER
        {
            $$ = $1;
        }
    | CONSTANT
        {
            $$ = $1;
        }
    | string_literal
        {
            $$ = $1;
        }
    | '(' expression ')'
        {
            $$ = $2;
        }
    | '(' compound_statement ')'
        {
            /* GNU statement expression: the value is the last expression statement's */
            $$ = ast_stmt(N_STMT_EXPR, @$, NULL, $2);
        }
    ;

/* adjacent string literals are concatenated */
string_literal
    : STRING_LITERAL
        {
            $$ = $1;
        }
    | string_literal STRING_LITERAL
        {
            $$ = ast_string_concat($1, $2);
        }
    ;

postfix_expression
    : primary_expression
        {
            $$ = $1;
        }
    | postfix_expression '[' expression ']'
        {
            $$ = ast_new(N_INDEX, @$);
            $$->u.index.array = $1;
            $$->u.index.index = $3;
        }
    | postfix_expression '(' ')'
        {
            $$ = ast_new(N_CALL, @$);
            $$->u.call.func = $1;
            $$->u.call.args = ast_list_new();
        }
    | postfix_expression '(' argument_expression_list ')'
        {
            $$ = ast_new(N_CALL, @$);
            $$->u.call.func = $1;
            $$->u.call.args = $3->u.list;
            $3->u.list = NULL;
            ast_free($3);
        }
    | postfix_expression '.' any_identifier
        {
            $$ = ast_new(N_MEMBER, @$);
            $$->u.member.object = $1;
            $$->u.member.name = take_name($3);
            $$->u.member.arrow = 0;
        }
    | postfix_expression PTR_OP any_identifier
        {
            $$ = ast_new(N_MEMBER, @$);
            $$->u.member.object = $1;
            $$->u.member.name = take_name($3);
            $$->u.member.arrow = 1;
        }
    | postfix_expression INC_OP
        {
            $$ = ast_unary(@$, OP_POSTINC, $1);
        }
    | postfix_expression DEC_OP
        {
            $$ = ast_unary(@$, OP_POSTDEC, $1);
        }
    ;

/* carried as a list node so the list has a home until the call takes it */
argument_expression_list
    : assignment_expression
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | argument_expression_list ',' assignment_expression
        {
            ast_list_push($1->u.list, $3);
            $$ = $1;
        }
    ;

unary_expression
    : postfix_expression
        {
            $$ = $1;
        }
    | INC_OP unary_expression
        {
            $$ = ast_unary(@$, OP_PREINC, $2);
        }
    | DEC_OP unary_expression
        {
            $$ = ast_unary(@$, OP_PREDEC, $2);
        }
    | unary_operator cast_expression
        {
            $1->u.unary.operand = $2;
            $$ = $1;
        }
    | SIZEOF unary_expression
        {
            $$ = ast_new(N_SIZEOF_EXPR, @$);
            $$->u.cast.expr = $2;
        }
    | SIZEOF '(' type_name ')'
        {
            $$ = ast_new(N_SIZEOF_TYPE, @$);
            $$->u.cast.type = $3;
        }
    | BUILTIN_VA_ARG '(' assignment_expression ',' type_name ')'
        {
            $$ = ast_new(N_VA_ARG, @$);
            $$->u.cast.expr = $3;
            $$->u.cast.type = $5;
        }
    | BUILTIN_OFFSETOF '(' type_name ',' offsetof_member ')'
        {
            /* sema folds this to an integer constant */
            $$ = ast_new(N_OFFSETOF, @$);
            $$->u.desig.init = $3;
            $$->u.desig.path = $5->u.list;
            $5->u.list = NULL;
            ast_free($5);
        }
    ;

/* the member designator of offsetof: a name, then `.name` and `[index]` steps */
offsetof_member
    : any_identifier
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, ast_name(N_DESIG_MEMBER, @1, take_name($1))));
        }
    | offsetof_member '.' any_identifier
        {
            ast_list_push($1->u.list, ast_name(N_DESIG_MEMBER, @3, take_name($3)));
            $$ = $1;
        }
    | offsetof_member '[' expression ']'
        {
            AST *d = ast_new(N_DESIG_INDEX, @3);
            d->u.cast.expr = $3;
            ast_list_push($1->u.list, d);
            $$ = $1;
        }
    ;

unary_operator
    : '&'
        {
            $$ = ast_unary(@$, OP_ADDR, NULL);
        }
    | '*'
        {
            $$ = ast_unary(@$, OP_DEREF, NULL);
        }
    | '+'
        {
            $$ = ast_unary(@$, OP_POS, NULL);
        }
    | '-'
        {
            $$ = ast_unary(@$, OP_NEG, NULL);
        }
    | '~'
        {
            $$ = ast_unary(@$, OP_BITNOT, NULL);
        }
    | '!'
        {
            $$ = ast_unary(@$, OP_NOT, NULL);
        }
    | REAL_OP
        {
            $$ = ast_unary(@$, OP_REAL, NULL);
        }
    | IMAG_OP
        {
            $$ = ast_unary(@$, OP_IMAG, NULL);
        }
    ;

cast_expression
    : unary_expression
        {
            $$ = $1;
        }
    | '(' type_name ')' cast_expression
        {
            $$ = ast_new(N_CAST, @$);
            $$->u.cast.type = $2;
            $$->u.cast.expr = $4;
        }
    ;

multiplicative_expression
    : cast_expression
        {
            $$ = $1;
        }
    | multiplicative_expression '*' cast_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_MUL, $1, $3);
        }
    | multiplicative_expression '/' cast_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_DIV, $1, $3);
        }
    | multiplicative_expression '%' cast_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_MOD, $1, $3);
        }
    ;

additive_expression
    : multiplicative_expression
        {
            $$ = $1;
        }
    | additive_expression '+' multiplicative_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_ADD, $1, $3);
        }
    | additive_expression '-' multiplicative_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_SUB, $1, $3);
        }
    ;

shift_expression
    : additive_expression
        {
            $$ = $1;
        }
    | shift_expression LEFT_OP additive_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_SHL, $1, $3);
        }
    | shift_expression RIGHT_OP additive_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_SHR, $1, $3);
        }
    ;

relational_expression
    : shift_expression
        {
            $$ = $1;
        }
    | relational_expression '<' shift_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_LT, $1, $3);
        }
    | relational_expression '>' shift_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_GT, $1, $3);
        }
    | relational_expression LE_OP shift_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_LE, $1, $3);
        }
    | relational_expression GE_OP shift_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_GE, $1, $3);
        }
    ;

equality_expression
    : relational_expression
        {
            $$ = $1;
        }
    | equality_expression EQ_OP relational_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_EQ, $1, $3);
        }
    | equality_expression NE_OP relational_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_NE, $1, $3);
        }
    ;

and_expression
    : equality_expression
        {
            $$ = $1;
        }
    | and_expression '&' equality_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_BITAND, $1, $3);
        }
    ;

exclusive_or_expression
    : and_expression
        {
            $$ = $1;
        }
    | exclusive_or_expression '^' and_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_BITXOR, $1, $3);
        }
    ;

inclusive_or_expression
    : exclusive_or_expression
        {
            $$ = $1;
        }
    | inclusive_or_expression '|' exclusive_or_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_BITOR, $1, $3);
        }
    ;

logical_and_expression
    : inclusive_or_expression
        {
            $$ = $1;
        }
    | logical_and_expression AND_OP inclusive_or_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_AND, $1, $3);
        }
    ;

logical_or_expression
    : logical_and_expression
        {
            $$ = $1;
        }
    | logical_or_expression OR_OP logical_and_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_OR, $1, $3);
        }
    ;

conditional_expression
    : logical_or_expression
        {
            $$ = $1;
        }
    | logical_or_expression '?' expression ':' conditional_expression
        {
            $$ = ast_cond(N_COND, @$, $1, $3, $5);
        }
    ;

assignment_expression
    : conditional_expression
        {
            $$ = $1;
        }
    | unary_expression assignment_operator assignment_expression
        {
            $2->u.binary.left = $1;
            $2->u.binary.right = $3;
            $2->loc = @$;
            $$ = $2;
        }
    ;

assignment_operator
    : '='
        {
            $$ = ast_binary(N_ASSIGN, @$, OP_ASSIGN, NULL, NULL);
        }
    | MUL_ASSIGN
        {
            $$ = ast_binary(N_ASSIGN, @$, OP_MUL_ASSIGN, NULL, NULL);
        }
    | DIV_ASSIGN
        {
            $$ = ast_binary(N_ASSIGN, @$, OP_DIV_ASSIGN, NULL, NULL);
        }
    | MOD_ASSIGN
        {
            $$ = ast_binary(N_ASSIGN, @$, OP_MOD_ASSIGN, NULL, NULL);
        }
    | ADD_ASSIGN
        {
            $$ = ast_binary(N_ASSIGN, @$, OP_ADD_ASSIGN, NULL, NULL);
        }
    | SUB_ASSIGN
        {
            $$ = ast_binary(N_ASSIGN, @$, OP_SUB_ASSIGN, NULL, NULL);
        }
    | LEFT_ASSIGN
        {
            $$ = ast_binary(N_ASSIGN, @$, OP_SHL_ASSIGN, NULL, NULL);
        }
    | RIGHT_ASSIGN
        {
            $$ = ast_binary(N_ASSIGN, @$, OP_SHR_ASSIGN, NULL, NULL);
        }
    | AND_ASSIGN
        {
            $$ = ast_binary(N_ASSIGN, @$, OP_AND_ASSIGN, NULL, NULL);
        }
    | XOR_ASSIGN
        {
            $$ = ast_binary(N_ASSIGN, @$, OP_XOR_ASSIGN, NULL, NULL);
        }
    | OR_ASSIGN
        {
            $$ = ast_binary(N_ASSIGN, @$, OP_OR_ASSIGN, NULL, NULL);
        }
    ;

expression
    : assignment_expression
        {
            $$ = $1;
        }
    | expression ',' assignment_expression
        {
            $$ = ast_binary(N_BINARY, @$, OP_COMMA, $1, $3);
        }
    ;

constant_expression
    : conditional_expression
        {
            $$ = $1;
        }
    ;

declaration
    : declaration_specifiers ';'
        {
            in_typedef = 0;
            spec_reset();
            $$ = ast_new(N_DECLARATION, @$);
            $$->u.decl.specs = $1->u.list;
            $$->u.decl.declarators = ast_list_new();
            $1->u.list = NULL;
            ast_free($1);
            /* `struct S;` declares a new tag in this scope, even if an
               outer scope has one */
            if ($$->u.decl.specs->len == 1) {
                AST *spec = $$->u.decl.specs->items[0];
                if ((spec->kind == N_STRUCT_SPEC || spec->kind == N_ENUM_SPEC)
                        && spec->u.record.tag && !spec->u.record.has_body) {
                    declare_tag(spec, 1);
                }
            }
        }
    | declaration_specifiers init_declarator_list ';'
        {
            in_typedef = 0;
            spec_reset();
            $$ = ast_new(N_DECLARATION, @$);
            $$->u.decl.specs = $1->u.list;
            $$->u.decl.declarators = $2->u.list;
            $1->u.list = NULL;
            $2->u.list = NULL;
            ast_free($1);
            ast_free($2);
        }
    ;

/*
 * Specifier lists are carried in a temporary list node (kind N_INIT_LIST,
 * never part of the final tree) until the declaration that owns them is
 * reduced. The rules are right recursive, so items are prepended.
 */
declaration_specifiers
    : storage_class_specifier
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | storage_class_specifier declaration_specifiers
        {
            ast_list_prepend($2->u.list, $1);
            $2->loc = @$;
            $$ = $2;
        }
    | type_specifier
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | type_specifier declaration_specifiers
        {
            ast_list_prepend($2->u.list, $1);
            $2->loc = @$;
            $$ = $2;
        }
    | type_qualifier
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | type_qualifier declaration_specifiers
        {
            ast_list_prepend($2->u.list, $1);
            $2->loc = @$;
            $$ = $2;
        }
    ;

init_declarator_list
    : init_declarator
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | init_declarator_list ',' init_declarator
        {
            ast_list_push($1->u.list, $3);
            $$ = $1;
        }
    ;

/*
 * A name is in scope from the end of its declarator, so it is declared
 * before the initializer is parsed.
 */
init_declarator
    : declarator
        {
            $$ = ast_new(N_INIT_DECLARATOR, @$);
            $$->u.init_decl.declarator = $1;
            declare_declarator($$);
        }
    | declarator '='
        {
            $$ = pending_push(ast_new(N_INIT_DECLARATOR, @1));
            $$->u.init_decl.declarator = $1;
            $1 = NULL;
            declare_declarator($$);
            spec_push();
        }
      initializer
        {
            $$ = pending_pop($3);
            $$->u.init_decl.init = $4;
            spec_pop();
        }
    ;

storage_class_specifier
    : TYPEDEF
        {
            in_typedef = 1;
            $$ = ast_new(N_STORAGE_CLASS, @$);
            $$->u.storage = STORAGE_TYPEDEF;
        }
    | INLINE
        {
            $$ = ast_new(N_STORAGE_CLASS, @$);
            $$->u.storage = STORAGE_INLINE;
        }
    | EXTERN
        {
            $$ = ast_new(N_STORAGE_CLASS, @$);
            $$->u.storage = STORAGE_EXTERN;
        }
    | STATIC
        {
            $$ = ast_new(N_STORAGE_CLASS, @$);
            $$->u.storage = STORAGE_STATIC;
        }
    | AUTO
        {
            $$ = ast_new(N_STORAGE_CLASS, @$);
            $$->u.storage = STORAGE_AUTO;
        }
    | REGISTER
        {
            $$ = ast_new(N_STORAGE_CLASS, @$);
            $$->u.storage = STORAGE_REGISTER;
        }
    ;

type_specifier
    : basic_type_specifier
        {
            $$ = $1;
        }
    | struct_or_union_specifier
        {
            $$ = $1;
        }
    | enum_specifier
        {
            $$ = $1;
        }
    ;

/* every type specifier but a struct, union or enum specifier */
basic_type_specifier
    : VOID
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_VOID;
        }
    | CHAR
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_CHAR;
        }
    | SHORT
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_SHORT;
        }
    | INT
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_INT;
        }
    | LONG
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_LONG;
        }
    | FLOAT
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_FLOAT;
        }
    | DOUBLE
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_DOUBLE;
        }
    | BUILTIN_VA_LIST
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_BUILTIN_VA_LIST;
        }
    | INT128
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_INT128;
        }
    | UINT128
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_UINT128;
        }
    | BOOL
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_BOOL;
        }
    | FLOAT16
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_FLOAT16;
        }
    | COMPLEX
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_COMPLEX;
        }
    | SIGNED
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_SIGNED;
        }
    | UNSIGNED
        {
            spec_seen_type = 1;
            $$ = ast_new(N_TYPE_SPEC, @$);
            $$->u.type_spec = TS_UNSIGNED;
        }
    | TYPE_NAME
        {
            spec_seen_type = 1;
            $1->kind = N_TYPEDEF_NAME;
            $$ = $1;
        }
    ;

/*
 * Struct tags, enum tags and member names live in their own namespaces, so
 * they may spell the same as a typedef name.
 */
any_identifier
    : IDENTIFIER
        {
            $$ = $1;
        }
    | TYPE_NAME
        {
            $$ = $1;
        }
    ;

/*
 * A tag is declared at the `{`, so members can refer to the struct. The
 * body opens a scope for the member name space.
 */
struct_or_union_specifier
    : struct_or_union any_identifier '{'
        {
            $1->u.record.tag = take_name($2);
            $2 = NULL;
            $1->u.record.pack = cur_pack;
            declare_tag($1, 1);
            spec_push();
            symtab_push(symtab, SCOPE_STRUCT, $1->u.record.tag, @3);
        }
      struct_declaration_list '}'
        {
            $$ = $1;
            $$->loc = @$;
            $$->u.record.has_body = 1;
            $$->u.record.members = $5->u.list;
            $5->u.list = NULL;
            ast_free($5);
            spec_pop();
            $$->u.record.scope = symtab_pop(symtab);
            tag_defined($$);
        }
    | struct_or_union '{'
        {
            $1->u.record.pack = cur_pack;
            spec_push();
            symtab_push(symtab, SCOPE_STRUCT, NULL, @2);
        }
      struct_declaration_list '}'
        {
            $$ = $1;
            $$->loc = @$;
            $$->u.record.has_body = 1;
            $$->u.record.members = $4->u.list;
            $4->u.list = NULL;
            ast_free($4);
            spec_pop();
            $$->u.record.scope = symtab_pop(symtab);
        }
    | struct_or_union any_identifier
        {
            $$ = $1;
            $$->loc = @$;
            $$->u.record.tag = take_name($2);
            declare_tag($$, 0);
        }
    ;

struct_or_union
    : struct_or_union_keyword
        {
            $$ = $1;
        }
    | struct_or_union_keyword attribute_list
        {
            $$ = $1;
            $$->u.record.attrs = $2->u.list;
            $2->u.list = NULL;
            ast_free($2);
        }
    ;

struct_or_union_keyword
    : STRUCT
        {
            spec_seen_type = 1;
            $$ = ast_new(N_STRUCT_SPEC, @$);
            $$->u.record.is_union = 0;
        }
    | UNION
        {
            spec_seen_type = 1;
            $$ = ast_new(N_STRUCT_SPEC, @$);
            $$->u.record.is_union = 1;
        }
    ;

attribute_list
    : ATTRIBUTE
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | attribute_list ATTRIBUTE
        {
            ast_list_push($1->u.list, $2);
            $$ = $1;
        }
    ;

struct_declaration_list
    : struct_declaration
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | pragma struct_declaration
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $2));
        }
    | struct_declaration_list struct_declaration
        {
            ast_list_push($1->u.list, $2);
            $$ = $1;
        }
    | struct_declaration_list pragma
        {
            $$ = $1;
        }
    ;

/* `#pragma pack(...)`, applied where it appears; produces nothing */
pragma
    : PRAGMA
        {
            apply_pragma($1);
            ast_free($1);
            $$ = NULL;
        }
    ;

struct_declaration
    : specifier_qualifier_list struct_declarator_list ';'
        {
            spec_reset();
            $$ = ast_new(N_STRUCT_DECLARATION, @$);
            $$->u.decl.specs = $1->u.list;
            $$->u.decl.declarators = $2->u.list;
            $1->u.list = NULL;
            $2->u.list = NULL;
            ast_free($1);
            ast_free($2);
        }
    | specifier_qualifier_list ';'
        {
            /* no declarator: an anonymous struct or union member, or a tag declaration */
            spec_reset();
            $$ = ast_new(N_STRUCT_DECLARATION, @$);
            $$->u.decl.specs = $1->u.list;
            $$->u.decl.declarators = ast_list_new();
            $1->u.list = NULL;
            ast_free($1);
        }
    ;

specifier_qualifier_list
    : type_specifier specifier_qualifier_list
        {
            ast_list_prepend($2->u.list, $1);
            $2->loc = @$;
            $$ = $2;
        }
    | type_specifier
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | type_qualifier specifier_qualifier_list
        {
            ast_list_prepend($2->u.list, $1);
            $2->loc = @$;
            $$ = $2;
        }
    | type_qualifier
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    ;

struct_declarator_list
    : struct_declarator
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | struct_declarator_list ',' struct_declarator
        {
            ast_list_push($1->u.list, $3);
            $$ = $1;
        }
    ;

struct_declarator
    : declarator
        {
            $$ = ast_new(N_STRUCT_DECLARATOR, @$);
            $$->u.init_decl.declarator = $1;
            declare_member($$);
        }
    | ':'
        {
            spec_push();
        }
      constant_expression
        {
            spec_pop();
            $$ = ast_new(N_STRUCT_DECLARATOR, @$);
            $$->u.init_decl.init = $3;
        }
    | declarator ':'
        {
            spec_push();
        }
      constant_expression
        {
            spec_pop();
            $$ = ast_new(N_STRUCT_DECLARATOR, @$);
            $$->u.init_decl.declarator = $1;
            $$->u.init_decl.init = $4;
            declare_member($$);
        }
    ;

enum_specifier
    : enum_keyword '{'
        {
            spec_push();
        }
      enumerator_list '}'
        {
            spec_pop();
            $$ = $1;
            $$->loc = @$;
            $$->u.record.has_body = 1;
            $$->u.record.members = $4->u.list;
            $4->u.list = NULL;
            ast_free($4);
        }
    | enum_keyword any_identifier '{'
        {
            $1->u.record.tag = take_name($2);
            $2 = NULL;
            declare_tag($1, 1);
            spec_push();
        }
      enumerator_list '}'
        {
            spec_pop();
            $$ = $1;
            $$->loc = @$;
            $$->u.record.has_body = 1;
            $$->u.record.members = $5->u.list;
            $5->u.list = NULL;
            ast_free($5);
            tag_defined($$);
        }
    | enum_keyword any_identifier
        {
            $$ = $1;
            $$->loc = @$;
            $$->u.record.tag = take_name($2);
            declare_tag($$, 0);
        }
    /* a fixed underlying type (C23, and clang's extension in older modes) */
    | enum_keyword enum_underlying '{'
        {
            spec_push();
        }
      enumerator_list '}'
        {
            spec_pop();
            $$ = $1;
            $$->loc = @$;
            $$->u.record.underlying = $2->u.list;
            $2->u.list = NULL;
            ast_free($2);
            $$->u.record.has_body = 1;
            $$->u.record.members = $5->u.list;
            $5->u.list = NULL;
            ast_free($5);
        }
    | enum_keyword any_identifier enum_underlying '{'
        {
            $1->u.record.tag = take_name($2);
            $2 = NULL;
            declare_tag($1, 1);
            spec_push();
        }
      enumerator_list '}'
        {
            spec_pop();
            $$ = $1;
            $$->loc = @$;
            $$->u.record.underlying = $3->u.list;
            $3->u.list = NULL;
            ast_free($3);
            $$->u.record.has_body = 1;
            $$->u.record.members = $6->u.list;
            $6->u.list = NULL;
            ast_free($6);
            tag_defined($$);
        }
    ;

/*
 * ENUM_COLON is a ':' that yylex saw after `enum [tag]` with a type behind
 * it; a struct or enum specifier is not allowed in the type. The list is
 * a fresh specifier frame, so a typedef name in it is still a type.
 */
enum_underlying
    : ENUM_COLON
        {
            spec_push();
        }
      enum_underlying_list
        {
            spec_pop();
            $$ = $3;
        }
    ;

enum_underlying_list
    : basic_type_specifier
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | type_qualifier
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | enum_underlying_list basic_type_specifier
        {
            ast_list_push($1->u.list, $2);
            $$ = $1;
        }
    | enum_underlying_list type_qualifier
        {
            ast_list_push($1->u.list, $2);
            $$ = $1;
        }
    ;

enum_keyword
    : ENUM
        {
            spec_seen_type = 1;
            $$ = ast_new(N_ENUM_SPEC, @$);
        }
    | ENUM attribute_list
        {
            spec_seen_type = 1;
            $$ = ast_new(N_ENUM_SPEC, @$);
            $$->u.record.attrs = $2->u.list;
            $2->u.list = NULL;
            ast_free($2);
        }
    ;

enumerator_list
    : enumerators
        {
            $$ = $1;
        }
    | enumerators ','
        {
            $$ = $1;
        }
    ;

enumerators
    : enumerator
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | enumerators ',' enumerator
        {
            ast_list_push($1->u.list, $3);
            $$ = $1;
        }
    ;

/* an enumerator may reuse the spelling of a typedef name from an outer scope */
enumerator
    : any_identifier
        {
            $$ = ast_name(N_ENUMERATOR, @$, take_name($1));
            declare_enumerator($$);
        }
    | any_identifier '=' constant_expression
        {
            $$ = ast_name(N_ENUMERATOR, @$, take_name($1));
            $$->u.enumerator.value = $3;
            declare_enumerator($$);
        }
    | any_identifier gnu_attribute_list
        {
            $$ = ast_name(N_ENUMERATOR, @$, take_name($1));
            $$->u.enumerator.attrs = $2->u.list;
            $2->u.list = NULL;
            ast_free($2);
            declare_enumerator($$);
        }
    | any_identifier gnu_attribute_list '=' constant_expression
        {
            $$ = ast_name(N_ENUMERATOR, @$, take_name($1));
            $$->u.enumerator.attrs = $2->u.list;
            $$->u.enumerator.value = $4;
            $2->u.list = NULL;
            ast_free($2);
            declare_enumerator($$);
        }
    ;

type_qualifier
    : CONST
        {
            $$ = ast_new(N_TYPE_QUAL, @$);
            $$->u.qual = QUAL_CONST;
        }
    | VOLATILE
        {
            $$ = ast_new(N_TYPE_QUAL, @$);
            $$->u.qual = QUAL_VOLATILE;
        }
    | RESTRICT
        {
            $$ = ast_new(N_TYPE_QUAL, @$);
            $$->u.qual = QUAL_RESTRICT;
        }
    | NULLABILITY
        {
            $$ = ast_new(N_TYPE_QUAL, @$);
            $$->u.qual = QUAL_NULLABILITY;
        }
    | ATTRIBUTE
        {
            $$ = $1;
        }
    ;

/* GNU extensions that may trail a declarator: __attribute__ and __asm */
gnu_attribute
    : ATTRIBUTE
        {
            $$ = $1;
        }
    | ASM
        {
            $$ = $1;
        }
    ;

gnu_attribute_list
    : gnu_attribute
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | gnu_attribute_list gnu_attribute
        {
            ast_list_push($1->u.list, $2);
            $$ = $1;
        }
    ;

declarator
    : pointer direct_declarator %prec DECLARATOR_END
        {
            $$ = ast_declarator_attach($1, $2);
        }
    | direct_declarator %prec DECLARATOR_END
        {
            $$ = $1;
        }
    | pointer direct_declarator gnu_attribute_list %prec DECLARATOR_END
        {
            $$ = ast_declarator_attach($1, $2);
            $$->u.declarator.attrs = $3->u.list;
            $3->u.list = NULL;
            ast_free($3);
        }
    | direct_declarator gnu_attribute_list %prec DECLARATOR_END
        {
            $$ = $1;
            $$->u.declarator.attrs = $2->u.list;
            $2->u.list = NULL;
            ast_free($2);
        }
    ;

direct_declarator
    : IDENTIFIER
        {
            $$ = ast_name(N_DECL_NAME, @$, take_name($1));
        }
    | '(' declarator ')'
        {
            $$ = $2;
        }
    | direct_declarator lbrack constant_expression ']'
        {
            spec_pop();
            $$ = ast_declarator(N_DECL_ARRAY, @$, $1);
            $$->u.declarator.size = $3;
        }
    | direct_declarator lbrack ']'
        {
            spec_pop();
            $$ = ast_declarator(N_DECL_ARRAY, @$, $1);
        }
    | direct_declarator lbrack array_qualifiers constant_expression ']'
        {
            /* C99 parameter qualifiers, which say nothing kcc acts on */
            spec_pop();
            ast_free($3);
            $$ = ast_declarator(N_DECL_ARRAY, @$, $1);
            $$->u.declarator.size = $4;
        }
    | direct_declarator lbrack array_qualifiers ']'
        {
            spec_pop();
            ast_free($3);
            $$ = ast_declarator(N_DECL_ARRAY, @$, $1);
        }
    | direct_declarator param_lparen parameter_type_list ')'
        {
            $$ = ast_declarator(N_DECL_FUNC, @$, $1);
            $$->u.declarator.params = $3->u.list;
            $$->u.declarator.variadic = $3->u.declarator.variadic;
            $3->u.list = NULL;
            ast_free($3);
            params_end($$);
        }
    | direct_declarator param_lparen identifier_list ')'
        {
            $$ = ast_declarator(N_DECL_FUNC, @$, $1);
            $$->u.declarator.params = $3->u.list;
            $$->u.declarator.kr = 1;
            $3->u.list = NULL;
            ast_free($3);
            params_end($$);
        }
    | direct_declarator param_lparen ')'
        {
            $$ = ast_declarator(N_DECL_FUNC, @$, $1);
            $$->u.declarator.params = ast_list_new();
            params_end($$);
        }
    ;

/*
 * Openers that must act before the parser reads the next token: a
 * parameter list opens a prototype scope, and both suspend the typedef
 * name rule (see spec_stack).
 */
param_lparen
    : '('
        {
            params_begin(@1);
        }
    ;

lbrack
    : '['
        {
            spec_push();
        }
    ;

/* `[static const 5]`: qualifiers and `static`, in any order */
array_qualifiers
    : type_qualifier
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | STATIC
        {
            $$ = ast_list_node(N_INIT_LIST, @$, NULL);
        }
    | array_qualifiers type_qualifier
        {
            ast_list_push($1->u.list, $2);
            $$ = $1;
        }
    | array_qualifiers STATIC
        {
            $$ = $1;
        }
    ;

/*
 * '^' declares a block (clang extension), it is treated like a pointer.
 * A pointer chain is built with a NULL innermost declarator, which the
 * enclosing rule fills in with ast_declarator_attach.
 */
pointer
    : '*'
        {
            $$ = ast_declarator(N_DECL_POINTER, @$, NULL);
        }
    | '*' type_qualifier_list
        {
            $$ = ast_declarator(N_DECL_POINTER, @$, NULL);
            $$->u.declarator.quals = $2->u.list;
            $2->u.list = NULL;
            ast_free($2);
        }
    | '*' pointer
        {
            $$ = ast_declarator(N_DECL_POINTER, @$, $2);
        }
    | '*' type_qualifier_list pointer
        {
            $$ = ast_declarator(N_DECL_POINTER, @$, $3);
            $$->u.declarator.quals = $2->u.list;
            $2->u.list = NULL;
            ast_free($2);
        }
    | '^'
        {
            $$ = ast_declarator(N_DECL_POINTER, @$, NULL);
            $$->u.declarator.block = 1;
        }
    | '^' type_qualifier_list
        {
            $$ = ast_declarator(N_DECL_POINTER, @$, NULL);
            $$->u.declarator.block = 1;
            $$->u.declarator.quals = $2->u.list;
            $2->u.list = NULL;
            ast_free($2);
        }
    | '^' pointer
        {
            $$ = ast_declarator(N_DECL_POINTER, @$, $2);
            $$->u.declarator.block = 1;
        }
    | '^' type_qualifier_list pointer
        {
            $$ = ast_declarator(N_DECL_POINTER, @$, $3);
            $$->u.declarator.block = 1;
            $$->u.declarator.quals = $2->u.list;
            $2->u.list = NULL;
            ast_free($2);
        }
    ;

type_qualifier_list
    : type_qualifier
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | type_qualifier_list type_qualifier
        {
            ast_list_push($1->u.list, $2);
            $$ = $1;
        }
    ;

/*
 * The temporary list node doubles as a carrier for the variadic flag,
 * which is stored in its (otherwise unused) declarator.variadic field.
 */
parameter_type_list
    : parameter_list
        {
            $$ = $1;
        }
    | parameter_list ',' ELLIPSIS
        {
            $$ = $1;
            $$->u.declarator.variadic = 1;
        }
    ;

parameter_list
    : parameter_declaration
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | parameter_list ',' parameter_declaration
        {
            ast_list_push($1->u.list, $3);
            $$ = $1;
        }
    ;

parameter_declaration
    : declaration_specifiers declarator
        {
            spec_reset();
            $$ = ast_typed(N_PARAM, @$, $1->u.list, $2);
            $1->u.list = NULL;
            ast_free($1);
            declare_param($$);
        }
    | declaration_specifiers abstract_declarator
        {
            spec_reset();
            $$ = ast_typed(N_PARAM, @$, $1->u.list, $2);
            $1->u.list = NULL;
            ast_free($1);
        }
    | declaration_specifiers
        {
            spec_reset();
            $$ = ast_typed(N_PARAM, @$, $1->u.list, NULL);
            $1->u.list = NULL;
            ast_free($1);
        }
    ;

identifier_list
    : IDENTIFIER
        {
            declare_kr_param($1);
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | identifier_list ',' IDENTIFIER
        {
            declare_kr_param($3);
            ast_list_push($1->u.list, $3);
            $$ = $1;
        }
    ;

/* a type name only appears inside an expression, where the flag was 0 */
type_name
    : specifier_qualifier_list
        {
            spec_reset();
            $$ = ast_typed(N_TYPE_NAME, @$, $1->u.list, NULL);
            $1->u.list = NULL;
            ast_free($1);
        }
    | specifier_qualifier_list abstract_declarator
        {
            spec_reset();
            $$ = ast_typed(N_TYPE_NAME, @$, $1->u.list, $2);
            $1->u.list = NULL;
            ast_free($1);
        }
    ;

abstract_declarator
    : pointer
        {
            $$ = $1;
        }
    | direct_abstract_declarator %prec DECLARATOR_END
        {
            $$ = $1;
        }
    | pointer direct_abstract_declarator %prec DECLARATOR_END
        {
            $$ = ast_declarator_attach($1, $2);
        }
    /* `int (^)(int) __attribute__((noescape))` in a parameter list */
    | direct_abstract_declarator gnu_attribute_list %prec DECLARATOR_END
        {
            $$ = $1;
            $$->u.declarator.attrs = $2->u.list;
            $2->u.list = NULL;
            ast_free($2);
        }
    | pointer direct_abstract_declarator gnu_attribute_list %prec DECLARATOR_END
        {
            $$ = ast_declarator_attach($1, $2);
            $$->u.declarator.attrs = $3->u.list;
            $3->u.list = NULL;
            ast_free($3);
        }
    ;

/*
 * A leading '(' may group or start a parameter list; the parser decides on
 * the token after it, so the parameter forms open their scope in a
 * mid-rule action instead of using param_lparen.
 */
direct_abstract_declarator
    : '(' abstract_declarator ')'
        {
            $$ = $2;
        }
    | lbrack ']'
        {
            spec_pop();
            $$ = ast_declarator(N_DECL_ARRAY, @$, NULL);
        }
    | lbrack constant_expression ']'
        {
            spec_pop();
            $$ = ast_declarator(N_DECL_ARRAY, @$, NULL);
            $$->u.declarator.size = $2;
        }
    | direct_abstract_declarator lbrack ']'
        {
            spec_pop();
            $$ = ast_declarator(N_DECL_ARRAY, @$, $1);
        }
    | direct_abstract_declarator lbrack constant_expression ']'
        {
            spec_pop();
            $$ = ast_declarator(N_DECL_ARRAY, @$, $1);
            $$->u.declarator.size = $3;
        }
    | '('
        {
            params_begin(@1);
        }
      ')'
        {
            $$ = ast_declarator(N_DECL_FUNC, @$, NULL);
            $$->u.declarator.params = ast_list_new();
            params_end($$);
        }
    | '('
        {
            params_begin(@1);
        }
      parameter_type_list ')'
        {
            $$ = ast_declarator(N_DECL_FUNC, @$, NULL);
            $$->u.declarator.params = $3->u.list;
            $$->u.declarator.variadic = $3->u.declarator.variadic;
            $3->u.list = NULL;
            ast_free($3);
            params_end($$);
        }
    | direct_abstract_declarator param_lparen ')'
        {
            $$ = ast_declarator(N_DECL_FUNC, @$, $1);
            $$->u.declarator.params = ast_list_new();
            params_end($$);
        }
    | direct_abstract_declarator param_lparen parameter_type_list ')'
        {
            $$ = ast_declarator(N_DECL_FUNC, @$, $1);
            $$->u.declarator.params = $3->u.list;
            $$->u.declarator.variadic = $3->u.declarator.variadic;
            $3->u.list = NULL;
            ast_free($3);
            params_end($$);
        }
    ;

initializer
    : assignment_expression
        {
            $$ = $1;
        }
    | '{' initializer_list '}'
        {
            $$ = $2;
            $$->loc = @$;
        }
    | '{' initializer_list ',' '}'
        {
            $$ = $2;
            $$->loc = @$;
        }
    ;

initializer_list
    : designated_initializer
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | initializer_list ',' designated_initializer
        {
            ast_list_push($1->u.list, $3);
            $$ = $1;
        }
    ;

/* C99 designators: `.member = x`, `[index] = x`, and paths of them */
designated_initializer
    : initializer
        {
            $$ = $1;
        }
    | designator_list '=' initializer
        {
            $$ = ast_new(N_DESIGNATED, @$);
            $$->u.desig.path = $1->u.list;
            $$->u.desig.init = $3;
            $1->u.list = NULL;
            ast_free($1);
        }
    ;

designator_list
    : designator
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | designator_list designator
        {
            ast_list_push($1->u.list, $2);
            $$ = $1;
        }
    ;

designator
    : '[' constant_expression ']'
        {
            $$ = ast_new(N_DESIG_INDEX, @$);
            $$->u.cast.expr = $2;
        }
    | '.' any_identifier
        {
            $$ = ast_name(N_DESIG_MEMBER, @$, take_name($2));
        }
    ;

statement
    : labeled_statement
        {
            $$ = $1;
        }
    | compound_statement
        {
            $$ = $1;
        }
    | expression_statement
        {
            $$ = $1;
        }
    | selection_statement
        {
            $$ = $1;
        }
    | iteration_statement
        {
            $$ = $1;
        }
    | jump_statement
        {
            $$ = $1;
        }
    ;

/* a label is declared at its colon, so that `x: x: ;` reports the second one */
labeled_statement
    : IDENTIFIER ':'
        {
            $$ = pending_push(ast_name(N_LABEL, @1, take_name($1)));
            $1 = NULL;
            declare_label($$);
        }
      statement
        {
            $$ = pending_pop($3);
            $$->u.stmt.body = $4;
        }
    | CASE constant_expression ':' statement
        {
            $$ = ast_stmt(N_CASE, @$, $2, $4);
        }
    | DEFAULT ':' statement
        {
            $$ = ast_stmt(N_DEFAULT, @$, NULL, $3);
        }
    ;

compound_statement
    : block_begin '}'
        {
            $$ = ast_new(N_COMPOUND, @$);
            $$->u.block.items = ast_list_new();
            block_end($$);
        }
    | block_begin block_item_list '}'
        {
            $$ = ast_new(N_COMPOUND, @$);
            $$->u.block.items = $2->u.list;
            $2->u.list = NULL;
            ast_free($2);
            block_end($$);
        }
    ;

/* declarations and statements in any order, as in C99 */
block_item_list
    : block_item
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | block_item_list block_item
        {
            ast_list_push($1->u.list, $2);
            $$ = $1;
        }
    ;

block_item
    : declaration
        {
            $$ = $1;
        }
    | statement
        {
            $$ = $1;
        }
    | pragma
        {
            $$ = ast_stmt(N_EXPR_STMT, @$, NULL, NULL);
        }
    ;

/* opens the block's scope before the first token inside it is read */
block_begin
    : '{'
        {
            block_begin(@1);
        }
    ;

declaration_list
    : declaration
        {
            $$ = ast_list_node(N_INIT_LIST, @$, ast_list_push(NULL, $1));
        }
    | declaration_list declaration
        {
            ast_list_push($1->u.list, $2);
            $$ = $1;
        }
    ;

expression_statement
    : ';'
        {
            $$ = ast_stmt(N_EXPR_STMT, @$, NULL, NULL);
        }
    | expression ';'
        {
            $$ = ast_stmt(N_EXPR_STMT, @$, $1, NULL);
        }
    ;

selection_statement
    : IF '(' expression ')' statement
        {
            $$ = ast_cond(N_IF, @$, $3, $5, NULL);
        }
    | IF '(' expression ')' statement ELSE statement
        {
            $$ = ast_cond(N_IF, @$, $3, $5, $7);
        }
    | SWITCH '(' expression ')' statement
        {
            $$ = ast_stmt(N_SWITCH, @$, $3, $5);
        }
    ;

iteration_statement
    : WHILE '(' expression ')' statement
        {
            $$ = ast_stmt(N_WHILE, @$, $3, $5);
        }
    | DO statement WHILE '(' expression ')' ';'
        {
            $$ = ast_stmt(N_DO, @$, $5, $2);
        }
    | for_begin expression_statement expression_statement ')' statement
        {
            $$ = ast_new(N_FOR, @$);
            $$->u.loop.init = unwrap_expr_stmt($2);
            $$->u.loop.cond = unwrap_expr_stmt($3);
            $$->u.loop.body = $5;
            for_end(NULL);
        }
    | for_begin expression_statement expression_statement expression ')' statement
        {
            $$ = ast_new(N_FOR, @$);
            $$->u.loop.init = unwrap_expr_stmt($2);
            $$->u.loop.cond = unwrap_expr_stmt($3);
            $$->u.loop.step = $4;
            $$->u.loop.body = $6;
            for_end(NULL);
        }
    | for_begin declaration expression_statement ')' statement
        {
            AST *loop = ast_new(N_FOR, @$);
            loop->u.loop.cond = unwrap_expr_stmt($3);
            loop->u.loop.body = $5;
            $$ = for_end(ast_list_push(ast_list_push(NULL, $2), loop));
            $$->loc = @$;
        }
    | for_begin declaration expression_statement expression ')' statement
        {
            AST *loop = ast_new(N_FOR, @$);
            loop->u.loop.cond = unwrap_expr_stmt($3);
            loop->u.loop.step = $4;
            loop->u.loop.body = $6;
            $$ = for_end(ast_list_push(ast_list_push(NULL, $2), loop));
            $$->loc = @$;
        }
    ;

/* every for gets a scope of its own, for a declaration in its first clause */
for_begin
    : FOR '('
        {
            spec_reset();
            symtab_push(symtab, SCOPE_BLOCK, NULL, @1);
        }
    ;

jump_statement
    : GOTO any_identifier ';'
        {
            $$ = ast_name(N_GOTO, @$, take_name($2));
        }
    | CONTINUE ';'
        {
            $$ = ast_new(N_CONTINUE, @$);
        }
    | BREAK ';'
        {
            $$ = ast_new(N_BREAK, @$);
        }
    | RETURN ';'
        {
            $$ = ast_stmt(N_RETURN, @$, NULL, NULL);
        }
    | RETURN expression ';'
        {
            $$ = ast_stmt(N_RETURN, @$, $2, NULL);
        }
    ;

translation_unit
    : external_declaration
        {
            $$ = ast_list_node(N_TRANSLATION_UNIT, @$, NULL);
            if ($1) {
                ast_list_push($$->u.list, $1);
            }
            tree = $$;
        }
    | translation_unit external_declaration
        {
            if ($2) {
                ast_list_push($1->u.list, $2);
            }
            $$ = $1;
        }
    ;

external_declaration
    : function_definition
        {
            $$ = $1;
        }
    | declaration
        {
            $$ = $1;
        }
    | ';'
        {
            /* stray semicolon, common in system headers */
            $$ = NULL;
        }
    | pragma
        {
            $$ = NULL;
        }
    ;

/*
 * The mid-rule action runs once the parser knows a definition follows the
 * declarator (the next token is `{` or starts a K&R declaration). It
 * declares the function and re-enters the parameters' scope so the K&R
 * declarations and the body land in it.
 */
function_definition
    : declaration_specifiers declarator
        {
            func_begin($2);
        }
      function_body
        {
            $$ = $4;
            $$->loc = @$;
            $$->u.func.specs = $1->u.list;
            $$->u.func.declarator = $2;
            $1->u.list = NULL;
            ast_free($1);
            func_end($$);
        }
    | declarator
        {
            func_begin($1);
        }
      function_body
        {
            $$ = $3;
            $$->loc = @$;
            $$->u.func.specs = ast_list_new();
            $$->u.func.declarator = $1;
            func_end($$);
        }
    ;

function_body
    : declaration_list compound_statement
        {
            $$ = ast_new(N_FUNCTION_DEF, @$);
            $$->u.func.kr_decls = $1->u.list;
            $$->u.func.body = $2;
            $1->u.list = NULL;
            ast_free($1);
        }
    | compound_statement
        {
            $$ = ast_new(N_FUNCTION_DEF, @$);
            $$->u.func.body = $1;
        }
    ;

%%

static int is_func_name(const char *name) {
    return strcmp(name, "__func__") == 0 || strcmp(name, "__FUNCTION__") == 0
        || strcmp(name, "__PRETTY_FUNCTION__") == 0;
}

/* does the token begin a type, as the underlying type of an enum needs? */
static int starts_type(const Token *tok) {
    switch (tok->kind) {
        case TOK_CHAR:
        case TOK_SHORT:
        case TOK_INT:
        case TOK_LONG:
        case TOK_SIGNED:
        case TOK_UNSIGNED:
        case TOK_CONST:
        case TOK_VOLATILE:
        case TOK_INT128:
        case TOK_UINT128:
        case TOK_BOOL:
            return 1;
        case TOK_ID: {
            Symbol *sym = symtab_lookup(symtab, NS_ORDINARY, tok->repr);
            return sym && sym->kind == SYM_TYPEDEF;
        }
        default:
            return 0;
    }
}

static int translate_tok(const Token *tok) {
    switch (tok->kind) {
        case TOK_EOF:
            return 0;

        case TOK_ID: {
            /*
             * A typedef name is a type unless the current declaration
             * already has a type specifier, in which case it is the name
             * being declared. The exception is right after '(': there a
             * lone typedef name is a parameter type (`int f(T)`), and in
             * an abstract declarator `int (T)` is a function type.
             */
            Symbol *sym = symtab_lookup(symtab, NS_ORDINARY, tok->repr);
            ident_sym = sym;
            if (!sym || sym->kind != SYM_TYPEDEF) {
                return IDENTIFIER;
            }
            /* after `T f(a, b)` a typedef name starts the declaration list, not a declarator */
            if (spec_seen_type && prev_tok != '(' && !(prev_tok == ')' && after_kr_list)) {
                return IDENTIFIER;
            }
            /*
             * `T:` outside a struct body is a label; inside one it is an
             * unnamed bit-field of type T.
             */
            if (current_token->next && current_token->next->tok.kind == TOK_COLON
                    && symtab->current->kind != SCOPE_STRUCT) {
                return IDENTIFIER;
            }
            return TYPE_NAME;
        }

        case TOK_INT_LIT:
        case TOK_FLOAT_LIT:
        case TOK_CHAR_LIT:
            return CONSTANT;

        case TOK_STRING:
            return STRING_LITERAL;

        case TOK_AUTO:
            return AUTO;
        case TOK_BREAK:
            return BREAK;
        case TOK_CASE:
            return CASE;
        case TOK_CHAR:
            return CHAR;
        case TOK_CONST:
            return CONST;
        case TOK_CONTINUE:
            return CONTINUE;
        case TOK_DEFAULT:
            return DEFAULT;
        case TOK_DO:
            return DO;
        case TOK_DOUBLE:
            return DOUBLE;
        case TOK_ELSE:
            return ELSE;
        case TOK_ENUM:
            return ENUM;
        case TOK_EXTERN:
            return EXTERN;
        case TOK_FLOAT:
            return FLOAT;
        case TOK_FOR:
            return FOR;
        case TOK_GOTO:
            return GOTO;
        case TOK_IF:
            return IF;
        case TOK_INT:
            return INT;
        case TOK_LONG:
            return LONG;
        case TOK_REGISTER:
            return REGISTER;
        case TOK_RETURN:
            return RETURN;
        case TOK_SHORT:
            return SHORT;
        case TOK_SIGNED:
            return SIGNED;
        case TOK_SIZEOF:
            return SIZEOF;
        case TOK_REAL:
            return REAL_OP;
        case TOK_IMAG:
            return IMAG_OP;
        case TOK_STATIC:
            return STATIC;
        case TOK_STRUCT:
            return STRUCT;
        case TOK_SWITCH:
            return SWITCH;
        case TOK_TYPEDEF:
            return TYPEDEF;
        case TOK_UNION:
            return UNION;
        case TOK_UNSIGNED:
            return UNSIGNED;
        case TOK_VOID:
            return VOID;
        case TOK_VOLATILE:
            return VOLATILE;
        case TOK_WHILE:
            return WHILE;

        case TOK_BUILTIN_VA_LIST:
            return BUILTIN_VA_LIST;
        case TOK_BUILTIN_VA_ARG:
            return BUILTIN_VA_ARG;
        case TOK_INLINE:
            return INLINE;
        case TOK_RESTRICT:
            return RESTRICT;
        case TOK_ATTRIBUTE:
            return ATTRIBUTE;
        case TOK_ASM:
            return ASM;
        case TOK_PRAGMA:
            return PRAGMA;
        case TOK_NULLABILITY:
            return NULLABILITY;
        case TOK_INT128:
            return INT128;
        case TOK_UINT128:
            return UINT128;
        case TOK_BOOL:
            return BOOL;
        case TOK_FLOAT16:
            return FLOAT16;
        case TOK_COMPLEX:
            return COMPLEX;
        case TOK_BUILTIN_OFFSETOF:
            return BUILTIN_OFFSETOF;
        case TOK_EXTENSION:
            /* dropped by the lexer */
            return -1;

        case TOK_ELLIPSIS:
            return ELLIPSIS;
        case TOK_RIGHT_ASSIGN:
            return RIGHT_ASSIGN;
        case TOK_LEFT_ASSIGN:
            return LEFT_ASSIGN;
        case TOK_PLUS_ASSIGN:
            return ADD_ASSIGN;
        case TOK_MINUS_ASSIGN:
            return SUB_ASSIGN;
        case TOK_MUL_ASSIGN:
            return MUL_ASSIGN;
        case TOK_DIV_ASSIGN:
            return DIV_ASSIGN;
        case TOK_MOD_ASSIGN:
            return MOD_ASSIGN;
        case TOK_AND_ASSIGN:
            return AND_ASSIGN;
        case TOK_XOR_ASSIGN:
            return XOR_ASSIGN;
        case TOK_OR_ASSIGN:
            return OR_ASSIGN;
        case TOK_RIGHT_SHIFT:
            return RIGHT_OP;
        case TOK_LEFT_SHIFT:
            return LEFT_OP;
        case TOK_INC:
            return INC_OP;
        case TOK_DEC:
            return DEC_OP;
        case TOK_ARROW:
            return PTR_OP;
        case TOK_AND_OP:
            return AND_OP;
        case TOK_OR_OP:
            return OR_OP;
        case TOK_LE:
            return LE_OP;
        case TOK_GE:
            return GE_OP;
        case TOK_EQ:
            return EQ_OP;
        case TOK_NE:
            return NE_OP;
        case TOK_SEMICOLON:
            return ';';
        case TOK_LBRACE:
            return '{';
        case TOK_RBRACE:
            return '}';
        case TOK_COMMA:
            return ',';
        case TOK_COLON:
            return ':';
        case TOK_ASSIGN:
            return '=';
        case TOK_LPAREN:
            return '(';
        case TOK_RPAREN:
            return ')';
        case TOK_LBRACK:
            return '[';
        case TOK_RBRACK:
            return ']';
        case TOK_DOT:
            return '.';
        case TOK_AMPERSAND:
            return '&';
        case TOK_NOT:
            return '!';
        case TOK_TILDE:
            return '~';
        case TOK_MINUS:
            return '-';
        case TOK_PLUS:
            return '+';
        case TOK_STAR:
            return '*';
        case TOK_SLASH:
            return '/';
        case TOK_PERCENT:
            return '%';
        case TOK_LT:
            return '<';
        case TOK_GT:
            return '>';
        case TOK_HAT:
            return '^';
        case TOK_PIPE:
            return '|';
        case TOK_QUESTION:
            return '?';

        case TOK_NKINDS:
            break;
    }
    return -1;
}

/* Build the semantic value handed to the parser for the current token */
static AST *token_value(const Token *tok, Loc loc) {
    switch (tok->kind) {
        case TOK_ID:
            return ast_name(N_IDENT, loc, strdup(tok->repr));
        case TOK_INT_LIT:
            return ast_text(N_INT, loc, strdup(tok->repr));
        case TOK_FLOAT_LIT:
            return ast_text(N_FLOAT, loc, strdup(tok->repr));
        case TOK_CHAR_LIT:
            return ast_text(N_CHAR, loc, strdup(tok->repr));
        case TOK_STRING:
            return ast_text(N_STRING, loc, strdup(tok->repr));
        case TOK_ATTRIBUTE:
        case TOK_ASM:
        case TOK_PRAGMA:
            return ast_text(N_ATTRIBUTE, loc, strdup(tok->repr));
        default:
            return NULL;
    }
}

static int yylex(void) {
    int tok;

    if (abort_requested) {
        return 0;   /* end of input: the parser unwinds from here */
    }
    if (!tokens) {
        tokens = lex();
        current_token = tokens;
    } else if (current_token->next) {
        current_token = current_token->next;
    }
    /* otherwise stay on the EOF token */

    tok = translate_tok(&current_token->tok);
    if (tok < 0) {
        /* a syntax error, so the parser unwinds and frees what it has built */
        diag_error(token_loc(), "invalid token '%s'", current_token->tok.repr);
        bad_token_reported = 1;
        tok = BAD_TOKEN;
    }
    /*
     * `enum [tag] : T {` gives the enum a fixed underlying type, while
     * `enum tag : 3` in a struct is an unnamed bit-field; like clang, tell
     * them apart by what follows the colon. (An attribute may come between
     * `enum` and the colon.)
     */
    if (tok == ':' && (prev_tok == ENUM || prev_tok == IDENTIFIER || prev_tok == TYPE_NAME || prev_tok == ATTRIBUTE)
            && current_token->next && starts_type(&current_token->next->tok)) {
        tok = ENUM_COLON;
    }
    yylloc.file = current_token->tok.file;
    yylloc.line = current_token->tok.lineno;
    yylloc.col = current_token->tok.col;
    yylval = token_value(&current_token->tok, yylloc);
    if (yylval && current_token->tok.kind == TOK_ID) {
        /* an identifier refers to whatever is in scope where it appears */
        yylval->sym = ident_sym;
    }
    if (tok == IDENTIFIER && !ident_sym && current_function && is_func_name(current_token->tok.repr)) {
        /* C99 `__func__` (and the GNU spellings): a string literal naming the function */
        size_t n = strlen(current_function->name);
        char *text = malloc(n + 3);
        if (!text) {
            diag_fatal(token_loc(), "out of memory");
        }
        text[0] = '"';
        memcpy(text + 1, current_function->name, n);
        text[n + 1] = '"';
        text[n + 2] = '\0';
        ast_free(yylval);
        yylval = ast_text(N_STRING, yylloc, text);
        tok = STRING_LITERAL;
    }
    prev_tok = tok;
    if (tok != ')') {
        after_kr_list = 0;
    }
    return tok;
}

static char *take_name(AST *ident) {
    char *name;
    if (!ident || ident->kind != N_IDENT) {
        fatal_error("internal error: expected an identifier");
        return strdup("?");
    }
    name = ident->u.name;
    ident->u.name = NULL;
    ast_free(ident);
    return name;
}

static AST *unwrap_expr_stmt(AST *stmt) {
    AST *expr;
    if (!stmt || stmt->kind != N_EXPR_STMT) {
        fatal_error("internal error: expected an expression statement");
        return NULL;
    }
    expr = stmt->u.stmt.expr;
    stmt->u.stmt.expr = NULL;
    ast_free(stmt);
    return expr;
}

/* typedef name flag, see the comment at spec_stack */

static void spec_push(void) {
    if (spec_sp + 1 >= (int)(sizeof(spec_stack) / sizeof(spec_stack[0]))) {
        fatal_error("declarations nested too deeply");
        return;
    }
    spec_sp++;
    spec_stack[spec_sp] = 0;
}

static void spec_pop(void) {
    if (spec_sp == 0) {
        fatal_error("internal error: specifier stack underflow");
        return;
    }
    spec_sp--;
}

static void spec_reset(void) {
    spec_stack[spec_sp] = 0;
}

/* declarations */

static void declare_declarator(AST *init_decl) {
    AST *declarator = init_decl->u.init_decl.declarator;
    const char *name = ast_declarator_name(declarator);
    AST *inner = ast_declarator_innermost(declarator);
    SymKind kind;
    Symbol *sym;

    if (!name) {
        fatal_error("declaration without a name");
        return;
    }
    if (in_typedef) {
        kind = SYM_TYPEDEF;
    } else if (inner && inner->kind == N_DECL_FUNC) {
        kind = SYM_FUNCTION;
    } else {
        kind = SYM_VARIABLE;
    }
    sym = symtab_declare(symtab, NS_ORDINARY, kind, name, init_decl, init_decl->loc);
    init_decl->sym = sym;
    /* a K&R parameter declaration completes its identifier list entry */
    if (sym->kind == SYM_PARAM && sym->decl && sym->decl->kind == N_IDENT) {
        sym->decl = init_decl;
        sym->loc = init_decl->loc;
    }
}

static void declare_param(AST *param) {
    const char *name = ast_declarator_name(param->u.typed.declarator);
    if (name) {
        param->sym = symtab_declare(symtab, NS_ORDINARY, SYM_PARAM, name, param, param->loc);
    }
}

static void declare_kr_param(AST *ident) {
    after_kr_list = 1;
    ident->sym = symtab_declare(symtab, NS_ORDINARY, SYM_PARAM, ident->u.name, ident, ident->loc);
}

static void declare_member(AST *struct_decl) {
    const char *name = ast_declarator_name(struct_decl->u.init_decl.declarator);
    if (name) {
        struct_decl->sym = symtab_declare(symtab, NS_MEMBER, SYM_MEMBER, name, struct_decl, struct_decl->loc);
    }
}

static void declare_enumerator(AST *enumerator) {
    enumerator->sym = symtab_declare(
        symtab, NS_ORDINARY, SYM_ENUMERATOR,
        enumerator->u.enumerator.name, enumerator, enumerator->loc
    );
}

static void declare_label(AST *label) {
    label->sym = symtab_declare(symtab, NS_LABEL, SYM_LABEL, label->u.stmt.name, label, label->loc);
}

/*
 * `struct S { ... }` and `struct S;` declare S in the current scope.
 * `struct S` elsewhere refers to an S from any enclosing scope, declaring
 * one here only if none exists.
 */
static Symbol *declare_tag(AST *spec, int in_current_scope) {
    const char *tag = spec->u.record.tag;
    SymKind kind;
    Symbol *sym;

    if (spec->kind == N_ENUM_SPEC) {
        kind = SYM_ENUM;
    } else if (spec->u.record.is_union) {
        kind = SYM_UNION;
    } else {
        kind = SYM_STRUCT;
    }
    if (!in_current_scope) {
        sym = symtab_lookup(symtab, NS_TAG, tag);
    } else {
        sym = NULL;
    }
    if (!sym) {
        sym = symtab_declare(symtab, NS_TAG, kind, tag, spec, spec->loc);
    }
    if (sym->kind != kind) {
        /* `struct s` and then `union s` in one scope */
        diag_error(spec->loc, "use of '%s' with a tag type that does not match its previous declaration", tag);
        diag_note(sym->loc, "previous declaration is here");
    }
    spec->sym = sym;
    return sym;
}

static void tag_defined(AST *spec) {
    Symbol *sym = symtab_lookup(symtab, NS_TAG, spec->u.record.tag);
    if (sym && !sym->def) {
        sym->def = spec;
    }
}

/* parameter lists */

static void params_begin(Loc loc) {
    spec_push();
    symtab_push(symtab, SCOPE_PROTOTYPE, NULL, loc);
}

static void params_end(AST *func) {
    spec_pop();
    func->u.declarator.scope = symtab_pop(symtab);
}

/* function definitions */

static void func_begin(AST *declarator) {
    const char *name = ast_declarator_name(declarator);
    AST *fn = ast_declarator_innermost(declarator);
    Scope *scope;

    if (!name) {
        fatal_error("function definition without a name");
        return;
    }
    spec_reset();
    in_typedef = 0;
    current_function = symtab_declare(
        symtab, NS_ORDINARY, SYM_FUNCTION, name, declarator, declarator->loc
    );
    if (fn && fn->kind == N_DECL_FUNC && fn->u.declarator.scope) {
        scope = fn->u.declarator.scope;
        symtab_enter(symtab, scope);
    } else {
        /* not a function declarator; let semantic analysis complain */
        scope = symtab_push(symtab, SCOPE_FUNCTION, NULL, declarator->loc);
    }
    scope->kind = SCOPE_FUNCTION;
    if (!scope->name) {
        scope->name = strdup(name);
    }
    function_body_pending = 1;
}

static void func_end(AST *fdef) {
    fdef->u.func.scope = fdef->u.func.body->u.block.scope;
    fdef->sym = current_function;
    if (current_function && !current_function->def) {
        current_function->def = fdef;
    }
    current_function = NULL;
    spec_reset();
}

/* blocks */

static void block_begin(Loc loc) {
    spec_reset();
    if (function_body_pending) {
        /* the function scope is already current, see func_begin */
        function_body_pending = 0;
    } else {
        symtab_push(symtab, SCOPE_BLOCK, NULL, loc);
    }
}

static void block_end(AST *block) {
    block->u.block.scope = symtab_pop(symtab);
}

static AST *for_end(ASTList *items) {
    Scope *scope = symtab_pop(symtab);
    AST *block;
    if (!items) {
        return NULL;
    }
    block = ast_new(N_COMPOUND, items->items[0]->loc);
    block->u.block.items = items;
    block->u.block.scope = scope;
    return block;
}

/* where the token the parser is looking at is */
static Loc token_loc(void) {
    Loc loc;
    loc.file = current_token ? current_token->tok.file : NULL;
    loc.line = current_token ? current_token->tok.lineno : 0;
    loc.col = current_token ? current_token->tok.col : 0;
    return loc;
}

/*
 * An error in a helper that leaves nothing sensible to parse. Not fatal:
 * the helper returns something harmless, yylex then reports end of input,
 * and the parser unwinds through its %destructor, freeing what it built,
 * rather than a longjmp losing it all.
 */
static void fatal_error(const char *msg) {
    if (!abort_requested) {
        diag_error(token_loc(), "%s", msg);
    }
    parse_failed = 1;
    abort_requested = 1;
}

static void yyerror(const char *msg) {
    if (bad_token_reported || abort_requested) {
        bad_token_reported = 0;
    } else {
        diag_error(token_loc(), "%s", msg);     /* bison's verbose messages name the unexpected token */
    }
    parse_failed = 1;
}

/*
 * Free what a fatal error (a longjmp out of the parser) left behind: the
 * token list, the tree so far and the lexer's partial list. The values on
 * the parser's own stack are lost in that case; a syntax error, the common
 * failure, unwinds normally and the %destructor frees them.
 */
void parse_cleanup(void) {
    toklist_free(tokens);
    tokens = NULL;
    current_token = NULL;
    ast_free(tree);
    tree = NULL;
    pending_free();
    lex_cleanup();
}

AST *parse(const char *text, SymTab *st) {
    AST *result;
    tree = NULL;
    parse_failed = 0;
    symtab = st;
    cur_pack = 0;
    pack_depth = 0;
    in_typedef = 0;
    spec_sp = 0;
    spec_stack[0] = 0;
    prev_tok = 0;
    after_kr_list = 0;
    bad_token_reported = 0;
    abort_requested = 0;
    function_body_pending = 0;
    current_function = NULL;
    parse_cleanup();    /* anything a fatal error left behind in the previous unit */
    lex_set_text(text);
    if (yyparse() != 0 || parse_failed) {
        ast_free(tree);
        tree = NULL;
    }
    symtab = NULL;
    result = tree;
    tree = NULL;
    toklist_free(tokens);
    tokens = NULL;
    current_token = NULL;
    return result;
}
