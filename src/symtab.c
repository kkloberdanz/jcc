#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "symtab.h"
#include "diag.h"
#include "util.h"

/* FNV-1a */
static size_t hash_name(const char *name) {
    size_t h = 2166136261u;
    while (*name) {
        h ^= (unsigned char)*name++;
        h *= 16777619u;
    }
    return h;
}

/* scopes */

static Scope *scope_new(ScopeKind kind, const char *name, Loc loc, Scope *parent) {
    Scope *scope = xcalloc(1, sizeof(*scope));
    scope->kind = kind;
    scope->name = name ? xstrdup(name) : NULL;
    scope->loc = loc;
    scope->parent = parent;
    if (parent) {
        if (parent->last_child) {
            parent->last_child->next_sibling = scope;
        } else {
            parent->first_child = scope;
        }
        parent->last_child = scope;
    }
    return scope;
}

static void scope_free(Scope *scope) {
    Scope *child;
    Symbol *sym;

    if (!scope) {
        return;
    }
    child = scope->first_child;
    while (child) {
        Scope *next = child->next_sibling;
        scope_free(child);
        child = next;
    }
    sym = scope->first;
    while (sym) {
        Symbol *next = sym->next;
        free(sym->name);
        free(sym);
        sym = next;
    }
    free(scope->buckets);
    free(scope->name);
    free(scope);
}

static void scope_grow(Scope *scope) {
    size_t nbuckets = scope->nbuckets ? scope->nbuckets * 2 : 16;
    Symbol **buckets = xcalloc(nbuckets, sizeof(*buckets));
    Symbol *sym;

    for (sym = scope->first; sym; sym = sym->next) {
        size_t i = hash_name(sym->name) & (nbuckets - 1);
        sym->chain = buckets[i];
        buckets[i] = sym;
    }
    free(scope->buckets);
    scope->buckets = buckets;
    scope->nbuckets = nbuckets;
}

Symbol *scope_lookup(const Scope *scope, Namespace ns, const char *name) {
    Symbol *sym;
    if (!scope->nbuckets) {
        return NULL;
    }
    sym = scope->buckets[hash_name(name) & (scope->nbuckets - 1)];
    for (; sym; sym = sym->chain) {
        if (sym->ns == ns && strcmp(sym->name, name) == 0) {
            return sym;
        }
    }
    return NULL;
}

static Symbol *scope_add(Scope *scope, Namespace ns, SymKind kind,
                         const char *name, AST *decl, Loc loc) {
    Symbol *sym = xcalloc(1, sizeof(*sym));
    size_t i;

    sym->name = xstrdup(name);
    sym->ns = ns;
    sym->kind = kind;
    sym->decl = decl;
    sym->def = NULL;
    sym->loc = loc;
    sym->scope = scope;
    sym->type = NULL;
    sym->storage = STORAGE_NONE;
    sym->value = 0;
    sym->offset = 0;
    sym->vreg = 0;
    sym->label = 0;

    if (scope->last) {
        scope->last->next = sym;
    } else {
        scope->first = sym;
    }
    scope->last = sym;
    scope->count++;

    if (scope->count > scope->nbuckets) {
        scope_grow(scope);  /* rehashes everything, including sym */
    } else {
        i = hash_name(name) & (scope->nbuckets - 1);
        sym->chain = scope->buckets[i];
        scope->buckets[i] = sym;
    }
    return sym;
}

/* the table */

SymTab *symtab_new(void) {
    SymTab *st = xcalloc(1, sizeof(*st));
    Loc loc;
    loc.file = NULL;
    loc.line = 0;
    loc.col = 0;
    st->file = scope_new(SCOPE_FILE, NULL, loc, NULL);
    st->current = st->file;
    return st;
}

void symtab_free(SymTab *st) {
    if (!st) {
        return;
    }
    scope_free(st->file);
    free(st);
}

Scope *symtab_push(SymTab *st, ScopeKind kind, const char *name, Loc loc) {
    st->current = scope_new(kind, name, loc, st->current);
    return st->current;
}

Scope *symtab_pop(SymTab *st) {
    Scope *scope = st->current;
    if (!scope->parent) {
        fatal("internal error: popping the file scope");
    }
    st->current = scope->parent;
    return scope;
}

void symtab_enter(SymTab *st, Scope *scope) {
    if (scope->parent != st->current) {
        fatal("internal error: entering a scope that is not a child of the current one");
    }
    st->current = scope;
}

void symtab_discard(SymTab *st, Scope *scope) {
    Scope *parent = scope->parent;
    Scope **link;

    if (scope->count || scope->first_child || scope == st->current || !parent) {
        fatal("internal error: discarding a scope that is in use");
    }
    for (link = &parent->first_child; *link != scope; link = &(*link)->next_sibling) {
        ;
    }
    *link = scope->next_sibling;
    if (parent->last_child == scope) {
        parent->last_child = NULL;
        if (parent->first_child) {
            Scope *s = parent->first_child;
            while (s->next_sibling) {
                s = s->next_sibling;
            }
            parent->last_child = s;
        }
    }
    scope->parent = NULL;
    scope_free(scope);
}

Scope *symtab_function_scope(const SymTab *st) {
    Scope *scope = st->current;
    while (scope && scope->kind != SCOPE_FUNCTION) {
        scope = scope->parent;
    }
    return scope;
}

/* the scope a declaration in name space `ns` lands in */
static Scope *declaring_scope(const SymTab *st, Namespace ns) {
    Scope *scope = st->current;
    switch (ns) {
        case NS_MEMBER:
            if (scope->kind != SCOPE_STRUCT) {
                fatal("internal error: member declared outside a struct");
            }
            return scope;

        case NS_LABEL: {
            Scope *fn = symtab_function_scope(st);
            return fn ? fn : scope;
        }

        case NS_ORDINARY:
        case NS_TAG:
            while (scope->kind == SCOPE_STRUCT) {
                scope = scope->parent;
            }
            return scope;
    }
    return scope;
}

Symbol *symtab_declare(SymTab *st, Namespace ns, SymKind kind,
                       const char *name, AST *decl, Loc loc) {
    Scope *scope = declaring_scope(st, ns);
    Symbol *sym = scope_lookup(scope, ns, name);
    if (sym) {
        return sym;
    }
    return scope_add(scope, ns, kind, name, decl, loc);
}

Symbol *symtab_lookup(const SymTab *st, Namespace ns, const char *name) {
    const Scope *scope;
    for (scope = st->current; scope; scope = scope->parent) {
        Symbol *sym = scope_lookup(scope, ns, name);
        if (sym) {
            return sym;
        }
    }
    return NULL;
}

/* names */

const char *sym_kind_name(SymKind kind) {
    static const char *const names[] = {
#define X(name, repr) repr,
        SYM_KINDS(X)
#undef X
    };
    return (size_t)kind < SYM_NKINDS ? names[kind] : "?";
}

const char *scope_kind_name(ScopeKind kind) {
    static const char *const names[] = {
#define X(name, repr) repr,
        SCOPE_KINDS(X)
#undef X
    };
    return (size_t)kind < SCOPE_NKINDS ? names[kind] : "?";
}

const char *namespace_name(Namespace ns) {
    switch (ns) {
        case NS_ORDINARY:
            return "ordinary";
        case NS_TAG:
            return "tag";
        case NS_MEMBER:
            return "member";
        case NS_LABEL:
            return "label";
    }
    return "?";
}

/* dump */

static const char *symtab_dump_file;

static void dump_loc(FILE *out, Loc loc) {
    if (loc.file && loc.file != symtab_dump_file) {
        symtab_dump_file = loc.file;
        fprintf(out, " <%s:%lu:%lu>", loc.file, (unsigned long)loc.line, (unsigned long)loc.col);
    } else {
        fprintf(out, " <%lu:%lu>", (unsigned long)loc.line, (unsigned long)loc.col);
    }
}

static void dump_scope(FILE *out, const Scope *scope, int depth) {
    const Symbol *sym;
    const Scope *child;

    fprintf(out, "%*s%s scope", depth * 2, "", scope_kind_name(scope->kind));
    if (scope->name) {
        fprintf(out, " %s", scope->name);
    }
    if (scope->kind != SCOPE_FILE) {
        dump_loc(out, scope->loc);
    }
    fputc('\n', out);

    for (sym = scope->first; sym; sym = sym->next) {
        fprintf(out, "%*s%s: %s", depth * 2 + 2, "", sym->name, sym_kind_name(sym->kind));
        if (sym->type) {
            fputs(" = ", out);
            type_print(sym->type, out);
        }
        if (sym->kind == SYM_ENUMERATOR && sym->type) {
            fprintf(out, " %ld", sym->value);
        }
        dump_loc(out, sym->loc);
        if (sym->storage != STORAGE_NONE) {
            fprintf(out, " %s", storage_name(sym->storage));
        }
        if (sym->def) {
            fputs(" defined", out);
        }
        fputc('\n', out);
    }
    for (child = scope->first_child; child; child = child->next_sibling) {
        dump_scope(out, child, depth + 1);
    }
}

void symtab_dump(const SymTab *st, FILE *out) {
    symtab_dump_file = NULL;
    dump_scope(out, st->file, 0);
}
