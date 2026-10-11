#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ast.h"
#include "diag.h"
#include "util.h"

/* lists */

ASTList *ast_list_new(void) {
    ASTList *list = xmalloc(sizeof(*list));
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
    return list;
}

static void list_reserve(ASTList *list, size_t n) {
    if (list->len + n > list->cap) {
        size_t cap = list->cap ? list->cap * 2 : 4;
        while (cap < list->len + n) {
            cap *= 2;
        }
        list->items = realloc(list->items, cap * sizeof(*list->items));
        if (!list->items) {
            fatal("out of memory");
        }
        list->cap = cap;
    }
}

ASTList *ast_list_push(ASTList *list, AST *node) {
    if (!list) {
        list = ast_list_new();
    }
    list_reserve(list, 1);
    list->items[list->len++] = node;
    return list;
}

ASTList *ast_list_prepend(ASTList *list, AST *node) {
    if (!list) {
        list = ast_list_new();
    }
    list_reserve(list, 1);
    memmove(list->items + 1, list->items, list->len * sizeof(*list->items));
    list->items[0] = node;
    list->len++;
    return list;
}

ASTList *ast_list_concat(ASTList *head, ASTList *tail) {
    if (!head) {
        return tail;
    }
    if (tail) {
        list_reserve(head, tail->len);
        memcpy(
            head->items + head->len,
            tail->items,
            tail->len * sizeof(*tail->items)
        );
        head->len += tail->len;
        free(tail->items);
        free(tail);
    }
    return head;
}

void ast_list_free(ASTList *list) {
    size_t i;
    if (!list) {
        return;
    }
    for (i = 0; i < list->len; i++) {
        ast_free(list->items[i]);
    }
    free(list->items);
    free(list);
}

/* constructors */

AST *ast_new(NodeKind kind, Loc loc) {
    AST *ast = xmalloc(sizeof(*ast));
    memset(ast, 0, sizeof(*ast));
    ast->kind = kind;
    ast->loc = loc;
    return ast;
}

AST *ast_name(NodeKind kind, Loc loc, char *name) {
    AST *ast = ast_new(kind, loc);
    if (kind == N_DECL_NAME) {
        ast->u.declarator.name = name;
    } else if (kind == N_GOTO || kind == N_LABEL) {
        ast->u.stmt.name = name;
    } else if (kind == N_ENUMERATOR) {
        ast->u.enumerator.name = name;
    } else {
        ast->u.name = name;
    }
    return ast;
}

AST *ast_text(NodeKind kind, Loc loc, char *text) {
    AST *ast = ast_new(kind, loc);
    ast->u.text = text;
    return ast;
}

AST *ast_unary(Loc loc, OpKind op, AST *operand) {
    AST *ast = ast_new(N_UNARY, loc);
    ast->u.unary.op = op;
    ast->u.unary.operand = operand;
    return ast;
}

AST *ast_binary(NodeKind kind, Loc loc, OpKind op, AST *left, AST *right) {
    AST *ast = ast_new(kind, loc);
    ast->u.binary.op = op;
    ast->u.binary.left = left;
    ast->u.binary.right = right;
    return ast;
}

AST *ast_cond(NodeKind kind, Loc loc, AST *cond, AST *then, AST *otherwise) {
    AST *ast = ast_new(kind, loc);
    ast->u.cond.cond = cond;
    ast->u.cond.then = then;
    ast->u.cond.otherwise = otherwise;
    return ast;
}

AST *ast_stmt(NodeKind kind, Loc loc, AST *expr, AST *body) {
    AST *ast = ast_new(kind, loc);
    ast->u.stmt.expr = expr;
    ast->u.stmt.body = body;
    return ast;
}

AST *ast_list_node(NodeKind kind, Loc loc, ASTList *list) {
    AST *ast = ast_new(kind, loc);
    ast->u.list = list ? list : ast_list_new();
    return ast;
}

AST *ast_declarator(NodeKind kind, Loc loc, AST *inner) {
    AST *ast = ast_new(kind, loc);
    ast->u.declarator.inner = inner;
    return ast;
}

AST *ast_typed(NodeKind kind, Loc loc, ASTList *specs, AST *declarator) {
    AST *ast = ast_new(kind, loc);
    ast->u.typed.specs = specs ? specs : ast_list_new();
    ast->u.typed.declarator = declarator;
    return ast;
}

void ast_implicit_cast(AST **slot, Type *type) {
    AST *cast = ast_new(N_IMPLICIT_CAST, (*slot)->loc);
    cast->u.cast.expr = *slot;
    cast->type = type;
    *slot = cast;
}

AST *ast_string_concat(AST *left, AST *right) {
    size_t llen = strlen(left->u.text);
    size_t rlen = strlen(right->u.text);
    char *text = xmalloc(llen + rlen + 2);

    memcpy(text, left->u.text, llen);
    text[llen] = ' ';
    memcpy(text + llen + 1, right->u.text, rlen + 1);
    free(left->u.text);
    left->u.text = text;
    ast_free(right);
    return left;
}

AST *ast_declarator_innermost(AST *declarator) {
    AST *p = declarator;
    AST *last = NULL;
    while (p && p->kind != N_DECL_NAME) {
        last = p;
        p = p->u.declarator.inner;
    }
    return last;
}

static int is_declarator(const AST *ast) {
    return ast && (ast->kind == N_DECL_NAME || ast->kind == N_DECL_POINTER ||
                   ast->kind == N_DECL_ARRAY || ast->kind == N_DECL_FUNC);
}

AST *ast_declarator_attach(AST *chain, AST *inner) {
    AST *p;
    if (!chain) {
        return inner;
    }
    p = chain;
    while (p->kind != N_DECL_NAME && p->u.declarator.inner) {
        p = p->u.declarator.inner;
    }
    if (p->kind == N_DECL_NAME) {
        fatal("internal error: declarator chain already ends in a name");
    }
    p->u.declarator.inner = inner;
    return chain;
}

const char *ast_declarator_name(const AST *declarator) {
    const AST *p = declarator;
    while (is_declarator(p)) {
        if (p->kind == N_DECL_NAME) {
            return p->u.declarator.name;
        }
        p = p->u.declarator.inner;
    }
    return NULL;
}

/* free */

void ast_free(AST *ast) {
    if (!ast) {
        return;
    }
    switch (ast->kind) {
        case N_IDENT:
        case N_TYPEDEF_NAME:
            free(ast->u.name);
            break;

        case N_INT:
        case N_FLOAT:
        case N_CHAR:
        case N_STRING:
        case N_ATTRIBUTE:
            free(ast->u.text);
            break;

        case N_UNARY:
            ast_free(ast->u.unary.operand);
            break;

        case N_BINARY:
        case N_ASSIGN:
            ast_free(ast->u.binary.left);
            ast_free(ast->u.binary.right);
            break;

        case N_COND:
        case N_IF:
            ast_free(ast->u.cond.cond);
            ast_free(ast->u.cond.then);
            ast_free(ast->u.cond.otherwise);
            break;

        case N_CALL:
            ast_free(ast->u.call.func);
            ast_list_free(ast->u.call.args);
            break;

        case N_INDEX:
            ast_free(ast->u.index.array);
            ast_free(ast->u.index.index);
            break;

        case N_MEMBER:
            ast_free(ast->u.member.object);
            free(ast->u.member.name);
            break;

        case N_CAST:
        case N_SIZEOF_EXPR:
        case N_SIZEOF_TYPE:
        case N_VA_ARG:
        case N_IMPLICIT_CAST:
            ast_free(ast->u.cast.type);
            ast_free(ast->u.cast.expr);
            break;

        case N_LABEL:
        case N_CASE:
        case N_DEFAULT:
        case N_EXPR_STMT:
        case N_SWITCH:
        case N_WHILE:
        case N_DO:
        case N_GOTO:
        case N_RETURN:
        case N_STMT_EXPR:
            free(ast->u.stmt.name);
            ast_free(ast->u.stmt.expr);
            ast_free(ast->u.stmt.body);
            break;

        case N_FOR:
            ast_free(ast->u.loop.init);
            ast_free(ast->u.loop.cond);
            ast_free(ast->u.loop.step);
            ast_free(ast->u.loop.body);
            break;

        case N_CONTINUE:
        case N_BREAK:
            break;

        case N_TRANSLATION_UNIT:
        case N_INIT_LIST:
            ast_list_free(ast->u.list);
            break;

        case N_DESIGNATED:
        case N_OFFSETOF:
            ast_list_free(ast->u.desig.path);
            ast_free(ast->u.desig.init);
            break;

        case N_DESIG_MEMBER:
            free(ast->u.name);
            break;

        case N_DESIG_INDEX:
            ast_free(ast->u.cast.expr);
            break;

        case N_COMPOUND:
            ast_list_free(ast->u.block.items);
            break;

        case N_FUNCTION_DEF:
            ast_list_free(ast->u.func.specs);
            ast_free(ast->u.func.declarator);
            ast_list_free(ast->u.func.kr_decls);
            ast_free(ast->u.func.body);
            break;

        case N_DECLARATION:
        case N_STRUCT_DECLARATION:
            ast_list_free(ast->u.decl.specs);
            ast_list_free(ast->u.decl.declarators);
            break;

        case N_INIT_DECLARATOR:
        case N_STRUCT_DECLARATOR:
            ast_free(ast->u.init_decl.declarator);
            ast_free(ast->u.init_decl.init);
            break;

        case N_DECL_NAME:
        case N_DECL_POINTER:
        case N_DECL_ARRAY:
        case N_DECL_FUNC:
            free(ast->u.declarator.name);
            ast_free(ast->u.declarator.inner);
            ast_free(ast->u.declarator.size);
            ast_list_free(ast->u.declarator.params);
            ast_list_free(ast->u.declarator.quals);
            ast_list_free(ast->u.declarator.attrs);
            break;

        case N_STORAGE_CLASS:
        case N_TYPE_SPEC:
        case N_TYPE_QUAL:
            break;

        case N_STRUCT_SPEC:
        case N_ENUM_SPEC:
            free(ast->u.record.tag);
            ast_list_free(ast->u.record.members);
            ast_list_free(ast->u.record.attrs);
            ast_list_free(ast->u.record.underlying);
            break;

        case N_ENUMERATOR:
            free(ast->u.enumerator.name);
            ast_free(ast->u.enumerator.value);
            ast_list_free(ast->u.enumerator.attrs);
            break;

        case N_PARAM:
        case N_TYPE_NAME:
            ast_list_free(ast->u.typed.specs);
            ast_free(ast->u.typed.declarator);
            break;

        case N_NKINDS:
            break;
    }
    free(ast);
}

/* names */

const char *node_kind_name(NodeKind kind) {
    static const char *names[] = {
#define X(name, repr) repr,
        NODE_KINDS(X)
#undef X
    };
    if (kind < 0 || kind >= N_NKINDS) {
        return "?";
    }
    return names[kind];
}

const char *op_name(OpKind op) {
    static const char *names[] = {
#define X(name, repr) repr,
        OP_KINDS(X)
#undef X
    };
    if (op < 0 || op >= OP_NKINDS) {
        return "?";
    }
    return names[op];
}

const char *storage_name(StorageKind storage) {
    switch (storage) {
        case STORAGE_TYPEDEF: return "typedef";
        case STORAGE_EXTERN: return "extern";
        case STORAGE_STATIC: return "static";
        case STORAGE_AUTO: return "auto";
        case STORAGE_REGISTER: return "register";
        case STORAGE_INLINE: return "inline";
        case STORAGE_NONE: return "none";
    }
    return "?";
}

const char *qual_name(QualKind qual) {
    switch (qual) {
        case QUAL_CONST: return "const";
        case QUAL_VOLATILE: return "volatile";
        case QUAL_RESTRICT: return "restrict";
        case QUAL_NULLABILITY: return "nullability";
    }
    return "?";
}

const char *type_spec_name(TypeSpecKind type) {
    switch (type) {
        case TS_VOID: return "void";
        case TS_INT: return "int";
        case TS_FLOAT: return "float";
        case TS_CHAR: return "char";
        case TS_SHORT: return "short";
        case TS_LONG: return "long";
        case TS_DOUBLE: return "double";
        case TS_SIGNED: return "signed";
        case TS_UNSIGNED: return "unsigned";
        case TS_BUILTIN_VA_LIST: return "__builtin_va_list";
        case TS_INT128: return "__int128";
        case TS_BOOL: return "_Bool";
        case TS_FLOAT16: return "_Float16";
        case TS_COMPLEX: return "_Complex";
        case TS_UINT128: return "__uint128_t";
    }
    return "?";
}

/*
 * dump
 *
 * One node per line, children indented by two spaces and prefixed with the
 * name of the field they fill. The file name is printed only when it
 * changes from the previous node, line and column always.
 */

static const char *ast_dump_file = NULL;

static void dump_node(FILE *out, int depth, const char *label, const AST *ast);

static void indent(FILE *out, int depth) {
    int i;
    for (i = 0; i < depth; i++) {
        fputs("  ", out);
    }
}

static void dump_list(FILE *out, int depth, const char *label, const ASTList *list) {
    size_t i;
    indent(out, depth);
    fprintf(out, "%s:", label);
    if (!list || list->len == 0) {
        fputs(" (empty)\n", out);
        return;
    }
    fputc('\n', out);
    for (i = 0; i < list->len; i++) {
        dump_node(out, depth + 1, NULL, list->items[i]);
    }
}

static void dump_header(FILE *out, int depth, const char *label, const AST *ast) {
    indent(out, depth);
    if (label) {
        fprintf(out, "%s: ", label);
    }
    if (!ast) {
        fputs("(none)\n", out);
        return;
    }
    fputs(node_kind_name(ast->kind), out);

    switch (ast->kind) {
        case N_DESIG_MEMBER:
        case N_IDENT:
        case N_TYPEDEF_NAME:
            fprintf(out, " %s", ast->u.name);
            break;
        case N_INT:
        case N_FLOAT:
        case N_CHAR:
        case N_STRING:
        case N_ATTRIBUTE:
            fprintf(out, " %s", ast->u.text);
            break;
        case N_UNARY:
            fprintf(out, " %s", op_name(ast->u.unary.op));
            break;
        case N_BINARY:
        case N_ASSIGN:
            fprintf(out, " %s", op_name(ast->u.binary.op));
            break;
        case N_MEMBER:
            fprintf(out, " %s%s", ast->u.member.arrow ? "->" : ".", ast->u.member.name);
            break;
        case N_LABEL:
        case N_GOTO:
            fprintf(out, " %s", ast->u.stmt.name);
            break;
        case N_DECL_NAME:
            fprintf(out, " %s", ast->u.declarator.name);
            break;
        case N_DECL_POINTER:
            if (ast->u.declarator.block) {
                fputs(" ^", out);
            }
            break;
        case N_DECL_FUNC:
            if (ast->u.declarator.kr) {
                fputs(" K&R", out);
            }
            if (ast->u.declarator.variadic) {
                fputs(" ...", out);
            }
            break;
        case N_STORAGE_CLASS:
            fprintf(out, " %s", storage_name(ast->u.storage));
            break;
        case N_TYPE_SPEC:
            fprintf(out, " %s", type_spec_name(ast->u.type_spec));
            break;
        case N_TYPE_QUAL:
            fprintf(out, " %s", qual_name(ast->u.qual));
            break;
        case N_STRUCT_SPEC:
            fprintf(
                out,
                " %s %s",
                ast->u.record.is_union ? "union" : "struct",
                ast->u.record.tag ? ast->u.record.tag : "(anonymous)"
            );
            break;
        case N_ENUM_SPEC:
            fprintf(out, " %s", ast->u.record.tag ? ast->u.record.tag : "(anonymous)");
            break;
        case N_ENUMERATOR:
            fprintf(out, " %s", ast->u.enumerator.name);
            break;
        default:
            break;
    }

    if (ast->loc.file && (!ast_dump_file || strcmp(ast_dump_file, ast->loc.file) != 0)) {
        ast_dump_file = ast->loc.file;
        fprintf(out, " <%s:%zu:%zu>", ast->loc.file, ast->loc.line, ast->loc.col);
    } else {
        fprintf(out, " <%zu:%zu>", ast->loc.line, ast->loc.col);
    }
    if (ast->type) {
        fputs(" : ", out);
        type_print(ast->type, out);
        if (ast->lvalue > 0) {
            fputs(" lvalue", out);
        }
    }
    fputc('\n', out);
}

static void dump_node(FILE *out, int depth, const char *label, const AST *ast) {
    int d = depth + 1;

    dump_header(out, depth, label, ast);
    if (!ast) {
        return;
    }

    switch (ast->kind) {
        case N_DESIG_MEMBER:
        case N_IDENT:
        case N_INT:
        case N_FLOAT:
        case N_CHAR:
        case N_STRING:
        case N_ATTRIBUTE:
        case N_TYPEDEF_NAME:
        case N_GOTO:
        case N_CONTINUE:
        case N_BREAK:
        case N_STORAGE_CLASS:
        case N_TYPE_SPEC:
        case N_TYPE_QUAL:
            break;

        case N_UNARY:
            dump_node(out, d, NULL, ast->u.unary.operand);
            break;

        case N_BINARY:
        case N_ASSIGN:
            dump_node(out, d, NULL, ast->u.binary.left);
            dump_node(out, d, NULL, ast->u.binary.right);
            break;

        case N_COND:
        case N_IF:
            dump_node(out, d, "cond", ast->u.cond.cond);
            dump_node(out, d, "then", ast->u.cond.then);
            if (ast->u.cond.otherwise || ast->kind == N_COND) {
                dump_node(out, d, "else", ast->u.cond.otherwise);
            }
            break;

        case N_CALL:
            dump_node(out, d, "func", ast->u.call.func);
            dump_list(out, d, "args", ast->u.call.args);
            break;

        case N_INDEX:
            dump_node(out, d, NULL, ast->u.index.array);
            dump_node(out, d, "index", ast->u.index.index);
            break;

        case N_MEMBER:
            dump_node(out, d, NULL, ast->u.member.object);
            break;

        case N_CAST:
        case N_VA_ARG:
            dump_node(out, d, "type", ast->u.cast.type);
            dump_node(out, d, NULL, ast->u.cast.expr);
            break;

        case N_SIZEOF_EXPR:
        case N_IMPLICIT_CAST:
            dump_node(out, d, NULL, ast->u.cast.expr);
            break;

        case N_SIZEOF_TYPE:
            dump_node(out, d, NULL, ast->u.cast.type);
            break;

        case N_LABEL:
        case N_DEFAULT:
            dump_node(out, d, NULL, ast->u.stmt.body);
            break;

        case N_CASE:
        case N_SWITCH:
        case N_WHILE:
            dump_node(out, d, "expr", ast->u.stmt.expr);
            dump_node(out, d, "body", ast->u.stmt.body);
            break;

        case N_DO:
            dump_node(out, d, "body", ast->u.stmt.body);
            dump_node(out, d, "expr", ast->u.stmt.expr);
            break;

        case N_STMT_EXPR:
            dump_node(out, d, NULL, ast->u.stmt.body);
            break;

        case N_EXPR_STMT:
        case N_RETURN:
            if (ast->u.stmt.expr) {
                dump_node(out, d, NULL, ast->u.stmt.expr);
            }
            break;

        case N_FOR:
            dump_node(out, d, "init", ast->u.loop.init);
            dump_node(out, d, "cond", ast->u.loop.cond);
            dump_node(out, d, "step", ast->u.loop.step);
            dump_node(out, d, "body", ast->u.loop.body);
            break;

        case N_DESIGNATED:
        case N_OFFSETOF:
            dump_list(out, d, "path", ast->u.desig.path);
            dump_node(out, d, "init", ast->u.desig.init);
            break;

        case N_DESIG_INDEX:
            dump_node(out, d, NULL, ast->u.cast.expr);
            break;

        case N_TRANSLATION_UNIT:
        case N_INIT_LIST: {
            size_t i;
            for (i = 0; i < ast->u.list->len; i++) {
                dump_node(out, d, NULL, ast->u.list->items[i]);
            }
            break;
        }

        case N_COMPOUND: {
            size_t i;
            for (i = 0; i < ast->u.block.items->len; i++) {
                dump_node(out, d, NULL, ast->u.block.items->items[i]);
            }
            break;
        }

        case N_FUNCTION_DEF:
            dump_list(out, d, "specs", ast->u.func.specs);
            dump_node(out, d, "declarator", ast->u.func.declarator);
            if (ast->u.func.kr_decls) {
                dump_list(out, d, "kr_decls", ast->u.func.kr_decls);
            }
            dump_node(out, d, "body", ast->u.func.body);
            break;

        case N_DECLARATION:
        case N_STRUCT_DECLARATION:
            dump_list(out, d, "specs", ast->u.decl.specs);
            dump_list(out, d, "declarators", ast->u.decl.declarators);
            break;

        case N_INIT_DECLARATOR:
            dump_node(out, d, NULL, ast->u.init_decl.declarator);
            if (ast->u.init_decl.init) {
                dump_node(out, d, "init", ast->u.init_decl.init);
            }
            break;

        case N_STRUCT_DECLARATOR:
            if (ast->u.init_decl.declarator) {
                dump_node(out, d, NULL, ast->u.init_decl.declarator);
            }
            if (ast->u.init_decl.init) {
                dump_node(out, d, "bits", ast->u.init_decl.init);
            }
            break;

        case N_DECL_NAME:
        case N_DECL_POINTER:
        case N_DECL_ARRAY:
        case N_DECL_FUNC:
            if (ast->u.declarator.quals) {
                dump_list(out, d, "quals", ast->u.declarator.quals);
            }
            if (ast->u.declarator.attrs) {
                dump_list(out, d, "attrs", ast->u.declarator.attrs);
            }
            if (ast->kind == N_DECL_ARRAY) {
                dump_node(out, d, "size", ast->u.declarator.size);
            }
            if (ast->kind == N_DECL_FUNC) {
                dump_list(out, d, "params", ast->u.declarator.params);
            }
            if (ast->kind != N_DECL_NAME) {
                dump_node(out, d, "inner", ast->u.declarator.inner);
            }
            break;

        case N_STRUCT_SPEC:
        case N_ENUM_SPEC:
            if (ast->u.record.underlying) {
                dump_list(out, d, "underlying", ast->u.record.underlying);
            }
            if (ast->u.record.has_body) {
                dump_list(out, d, "members", ast->u.record.members);
            }
            break;

        case N_ENUMERATOR:
            if (ast->u.enumerator.attrs) {
                dump_list(out, d, "attrs", ast->u.enumerator.attrs);
            }
            if (ast->u.enumerator.value) {
                dump_node(out, d, "value", ast->u.enumerator.value);
            }
            break;

        case N_PARAM:
        case N_TYPE_NAME:
            dump_list(out, d, "specs", ast->u.typed.specs);
            if (ast->u.typed.declarator) {
                dump_node(out, d, "declarator", ast->u.typed.declarator);
            }
            break;

        case N_NKINDS:
            break;
    }
}

void ast_dump(const AST *ast, FILE *out) {
    ast_dump_file = NULL;
    dump_node(out, 0, NULL, ast);
}
