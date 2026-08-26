/* sema.c — name resolution, type checking, monomorphisation.
 *
 * Pipeline:
 *   1. collect   — register every top-level name of every module
 *   2. resolve   — turn TypeExprs into Types; wire `use` to modules
 *   3. check     — type-check function bodies, creating FnInsts on demand.
 *                  Generic functions are monomorphised: each distinct type
 *                  argument tuple clones the AST and checks it afresh, so
 *                  diagnostics point at both the body and the instantiation.
 */
#include "vela.h"
#include <stdarg.h>

Unit g_unit;

/* ------------------------------------------------------------------ */
/* substitutions                                                        */
/* ------------------------------------------------------------------ */

typedef struct Subst {
    Vec names;    /* const char* */
    Vec types;    /* Type* */
} Subst;

static Type *subst_get(Subst *s, const char *n) {
    if (!s) return NULL;
    for (int i = 0; i < s->names.len; i++)
        if ((const char *)s->names.data[i] == n) return VEC_AT(&s->types, Type, i);
    return NULL;
}
static void subst_put(Subst *s, const char *n, Type *t) {
    for (int i = 0; i < s->names.len; i++)
        if ((const char *)s->names.data[i] == n) { s->types.data[i] = t; return; }
    vec_push(&s->names, (void *)n);
    vec_push(&s->types, t);
}

/* ------------------------------------------------------------------ */
/* function context                                                     */
/* ------------------------------------------------------------------ */

typedef struct FnCtx {
    struct FnCtx *parent;
    FnInst  *inst;
    Scope   *scope;
    Type    *ret;
    Module  *mod;
    Subst   *subst;
    int      nslots;
    int      loop_depth;
    int      returns;      /* saw a return on all paths (approximate) */
} FnCtx;

static Module *cur_mod;
static FnCtx  *cur_fn;
static Vec     work_queue;      /* FnInst* pending body check */
static Vec     inst_stack;      /* Span* instantiation trace */
static int     sema_errors;

/* method table: (typekey, name) -> Decl* */
typedef struct MEnt { const char *key; const char *name; Decl *d; struct MEnt *next; } MEnt;
#define MTAB_N 1024
static MEnt *mtab[MTAB_N];

static uint32_t hash2(const void *a, const void *b) {
    uintptr_t x = (uintptr_t)a * 31 + (uintptr_t)b;
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 29;
    return (uint32_t)x;
}

static void mtab_put(const char *key, const char *name, Decl *d) {
    uint32_t h = hash2(key, name) % MTAB_N;
    MEnt *e = NEW(MEnt);
    e->key = key; e->name = name; e->d = d;
    e->next = mtab[h]; mtab[h] = e;
}
static Decl *mtab_get(const char *key, const char *name) {
    uint32_t h = hash2(key, name) % MTAB_N;
    for (MEnt *e = mtab[h]; e; e = e->next)
        if (e->key == key && e->name == name) return e->d;
    return NULL;
}

static const char *typekey(Type *t) {
    if (!t) return NULL;
    switch (t->kind) {
        case TY_INT: return intern("Int");
        case TY_FLOAT: return intern("Float");
        case TY_BOOL: return intern("Bool");
        case TY_BYTE: return intern("Byte");
        case TY_STR: return intern("Str");
        case TY_LIST: return intern("List");
        case TY_MAP: return intern("Map");
        case TY_RANGE: return intern("Range");
        case TY_OPT: return intern("Option");
        case TY_RES: return intern("Result");
        case TY_ERRTYPE: return intern("Error");
        case TY_STRUCT: case TY_ENUM: {
            if (!t->decl) return NULL;
            char buf[512];
            snprintf(buf, sizeof buf, "%s.%s",
                     t->decl->mod ? t->decl->mod->modpath : "?", t->decl->name);
            return intern(buf);
        }
        default: return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* diagnostics helpers                                                  */
/* ------------------------------------------------------------------ */

static Diag *serr(Span sp, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    Diag *d = diag_add(DIAG_ERROR, sp, "%s", msg);
    sema_errors++;
    /* attach the instantiation trace so generic errors are debuggable */
    for (int i = inst_stack.len - 1; i >= 0 && d; i--) {
        Span *s = VEC_AT(&inst_stack, Span, i);
        diag_note(d, *s, "in this instantiation");
    }
    return d;
}

static void swarn(Span sp, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char msg[1024];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_add(DIAG_WARN, sp, "%s", msg);
}

/* Levenshtein-ish suggestion for unknown names. */
static int edit_dist(const char *a, const char *b) {
    int la = (int)strlen(a), lb = (int)strlen(b);
    if (la > 40 || lb > 40) return 99;
    int prev[64], cur[64];
    for (int j = 0; j <= lb; j++) prev[j] = j;
    for (int i = 1; i <= la; i++) {
        cur[0] = i;
        for (int j = 1; j <= lb; j++) {
            int c = (a[i-1] == b[j-1]) ? 0 : 1;
            int m = prev[j] + 1;
            if (cur[j-1] + 1 < m) m = cur[j-1] + 1;
            if (prev[j-1] + c < m) m = prev[j-1] + c;
            cur[j] = m;
        }
        memcpy(prev, cur, sizeof(int) * (size_t)(lb + 1));
    }
    return prev[lb];
}

static const char *suggest_in_scope(Scope *s, const char *name) {
    const char *best = NULL; int bd = 99;
    for (; s; s = s->parent) {
        for (int i = 0; i < s->nbuckets; i++)
            for (Sym *sym = s->buckets[i]; sym; sym = sym->next) {
                int d = edit_dist(name, sym->name);
                if (d < bd && d <= 2) { bd = d; best = sym->name; }
            }
    }
    return best;
}

/* ------------------------------------------------------------------ */
/* type resolution                                                      */
/* ------------------------------------------------------------------ */

static Type *struct_instance(Decl *d, Vec *targs, Span sp);
static Type *resolve_type(TypeExpr *te, Module *m, Subst *sub);
static Module *find_used_module(Module *m, const char *alias);
static Decl *find_type_decl(Module *m, const char *name);

static Decl *find_type_decl(Module *m, const char *name) {
    for (int i = 0; i < m->decls.len; i++) {
        Decl *d = VEC_AT(&m->decls, Decl, i);
        if ((d->kind == D_STRUCT || d->kind == D_ENUM || d->kind == D_ALIAS) && d->name == name)
            return d;
    }
    /* also search public types of imported modules by bare name (prelude-ish
       for core types like Error) */
    return NULL;
}

static Module *find_used_module(Module *m, const char *alias) {
    for (int i = 0; i < m->decls.len; i++) {
        Decl *d = VEC_AT(&m->decls, Decl, i);
        if (d->kind == D_USE && d->alias == alias) return d->target_mod;
    }
    return NULL;
}

static Type *builtin_named(const char *n) {
    if (n == intern("Int"))   return ty_int;
    if (n == intern("Float")) return ty_float;
    if (n == intern("Bool"))  return ty_bool;
    if (n == intern("Byte"))  return ty_byte;
    if (n == intern("Str"))   return ty_str;
    if (n == intern("Void"))  return ty_void;
    if (n == intern("Range")) return ty_range;
    if (n == intern("Error")) return ty_error;
    return NULL;
}

static Type *resolve_type(TypeExpr *te, Module *m, Subst *sub) {
    if (!te) return ty_void;
    switch (te->kind) {
        case TE_OPT:  return ty_opt(resolve_type(te->sub, m, sub));
        case TE_RES:  return ty_res(resolve_type(te->sub, m, sub));
        case TE_LIST: return ty_list(resolve_type(te->sub, m, sub));
        case TE_MAP:  return ty_map(resolve_type(te->sub, m, sub),
                                    resolve_type(te->sub2, m, sub));
        case TE_FN: {
            Vec ps; memset(&ps, 0, sizeof ps);
            for (int i = 0; i < te->args.len; i++)
                vec_push(&ps, resolve_type(VEC_AT(&te->args, TypeExpr, i), m, sub));
            return ty_fn(ps, te->sub ? resolve_type(te->sub, m, sub) : ty_void);
        }
        case TE_NAME: {
            if (te->modname) {
                Module *om = find_used_module(m, te->modname);
                if (!om) {
                    serr(te->span, "no module `%s` is in scope", te->modname);
                    return ty_int;
                }
                Decl *d = find_type_decl(om, te->name);
                if (!d) {
                    serr(te->span, "module `%s` has no type `%s`", te->modname, te->name);
                    return ty_int;
                }
                if (!d->is_pub && om != m) {
                    serr(te->span, "type `%s.%s` is private", te->modname, te->name);
                }
                if (d->kind == D_ALIAS) return resolve_type(d->texpr, d->mod, NULL);
                Vec args; memset(&args, 0, sizeof args);
                for (int i = 0; i < te->args.len; i++)
                    vec_push(&args, resolve_type(VEC_AT(&te->args, TypeExpr, i), m, sub));
                if (args.len != d->generics.len) {
                    serr(te->span, "`%s.%s` takes %d type argument%s, found %d",
                         te->modname, te->name, d->generics.len,
                         d->generics.len == 1 ? "" : "s", args.len);
                    while (args.len < d->generics.len) vec_push(&args, ty_int);
                    args.len = d->generics.len;
                }
                return struct_instance(d, &args, te->span);
            }
            Type *g = subst_get(sub, te->name);
            if (g) return g;
            if (sub) {
                for (int i = 0; i < sub->names.len; i++)
                    if ((const char *)sub->names.data[i] == te->name) return ty_any;
            }
            if (te->name == intern("List") && te->args.len == 1)
                return ty_list(resolve_type(VEC_AT(&te->args, TypeExpr, 0), m, sub));
            if (te->name == intern("Map") && te->args.len == 2)
                return ty_map(resolve_type(VEC_AT(&te->args, TypeExpr, 0), m, sub),
                              resolve_type(VEC_AT(&te->args, TypeExpr, 1), m, sub));
            Type *b = builtin_named(te->name);
            if (b) {
                if (te->args.len)
                    serr(te->span, "`%s` does not take type arguments", te->name);
                return b;
            }
            Decl *d = find_type_decl(m, te->name);
            if (!d) {
                /* look in used modules for a public type of that name */
                for (int i = 0; i < m->decls.len && !d; i++) {
                    Decl *u = VEC_AT(&m->decls, Decl, i);
                    if (u->kind == D_USE && u->target_mod) {
                        Decl *c = find_type_decl(u->target_mod, te->name);
                        if (c && c->is_pub) d = c;
                    }
                }
            }
            if (!d) {
                Module *core = load_module("core", NULL, te->span);
                if (core) { Decl *c = find_type_decl(core, te->name); if (c && c->is_pub) d = c; }
            }
            if (!d) {
                Diag *dg = serr(te->span, "unknown type `%s`", te->name);
                const char *cands[] = {"Int","Float","Bool","Byte","Str","Void","Range","Error"};
                for (unsigned i = 0; i < sizeof cands / sizeof *cands; i++)
                    if (edit_dist(te->name, cands[i]) <= 2) {
                        diag_note(dg, NOSPAN, "did you mean `%s`?", cands[i]);
                        break;
                    }
                return ty_int;
            }
            if (d->kind == D_ALIAS) return resolve_type(d->texpr, d->mod, NULL);
            Vec args; memset(&args, 0, sizeof args);
            for (int i = 0; i < te->args.len; i++)
                vec_push(&args, resolve_type(VEC_AT(&te->args, TypeExpr, i), m, sub));
            if (args.len != d->generics.len) {
                if (d->generics.len == 0)
                    serr(te->span, "`%s` does not take type arguments", te->name);
                else
                    serr(te->span, "`%s` takes %d type argument%s, found %d",
                         te->name, d->generics.len, d->generics.len == 1 ? "" : "s", args.len);
                while (args.len < d->generics.len) vec_push(&args, ty_int);
                args.len = d->generics.len;
            }
            return struct_instance(d, &args, te->span);
        }
    }
    return ty_int;
}

/* Intern struct/enum instances so ty_eq is cheap and descriptors are unique. */
static Vec all_insts;   /* Type* */

static Type *struct_instance(Decl *d, Vec *targs, Span sp) {
    for (int i = 0; i < all_insts.len; i++) {
        Type *t = VEC_AT(&all_insts, Type, i);
        if (t->decl != d || t->targs.len != targs->len) continue;
        int same = 1;
        for (int j = 0; j < targs->len; j++)
            if (!ty_eq(VEC_AT(&t->targs, Type, j), VEC_AT(targs, Type, j))) { same = 0; break; }
        if (same) return t;
    }
    Type *t = NEW(Type);
    t->kind = (d->kind == D_ENUM) ? TY_ENUM : TY_STRUCT;
    t->decl = d;
    t->size = 8;
    t->name = d->name;
    for (int j = 0; j < targs->len; j++) vec_push(&t->targs, targs->data[j]);
    vec_push(&all_insts, t);

    Subst sub; memset(&sub, 0, sizeof sub);
    for (int j = 0; j < d->generics.len && j < targs->len; j++)
        subst_put(&sub, (const char *)d->generics.data[j], VEC_AT(targs, Type, j));

    if (d->kind == D_STRUCT) {
        for (int i = 0; i < d->fields.len; i++) {
            StructField *f = VEC_AT(&d->fields, StructField, i);
            vec_push(&t->fields, resolve_type(f->type, d->mod, &sub));
        }
    } else {
        int prim = 1;
        for (int i = 0; i < d->variants.len; i++) {
            EnumVariant *v = VEC_AT(&d->variants, EnumVariant, i);
            Vec *pl = NEW(Vec);
            memset(pl, 0, sizeof(Vec));
            for (int j = 0; j < v->types.len; j++)
                vec_push(pl, resolve_type(VEC_AT(&v->types, TypeExpr, j), d->mod, &sub));
            if (pl->len) prim = 0;
            vec_push(&t->variants, pl);
        }
        t->is_prim = prim;
    }
    return t;
}

Type *struct_field_type(Type *st, int i) {
    if (!st || i < 0 || i >= st->fields.len) return ty_int;
    return VEC_AT(&st->fields, Type, i);
}
int struct_field_index(Type *st, const char *name) {
    if (!st || !st->decl) return -1;
    for (int i = 0; i < st->decl->fields.len; i++)
        if (VEC_AT(&st->decl->fields, StructField, i)->name == name) return i;
    return -1;
}
int enum_variant_index(Type *et, const char *name) {
    if (!et || !et->decl) return -1;
    for (int i = 0; i < et->decl->variants.len; i++)
        if (VEC_AT(&et->decl->variants, EnumVariant, i)->name == name) return i;
    return -1;
}
int enum_payload_count(Type *et, int v) {
    if (!et || v < 0 || v >= et->variants.len) return 0;
    Vec *pl = (Vec *)et->variants.data[v];
    return pl->len;
}
Type *enum_payload_type(Type *et, int v, int i) {
    if (!et || v < 0 || v >= et->variants.len) return ty_int;
    Vec *pl = (Vec *)et->variants.data[v];
    if (i < 0 || i >= pl->len) return ty_int;
    return VEC_AT(pl, Type, i);
}

/* ------------------------------------------------------------------ */
/* AST cloning (for monomorphisation)                                   */
/* ------------------------------------------------------------------ */

static Expr *clone_expr(Expr *e);
static Stmt *clone_stmt(Stmt *s);

static Vec clone_vec_expr(Vec *v) {
    Vec r; memset(&r, 0, sizeof r);
    for (int i = 0; i < v->len; i++) vec_push(&r, clone_expr(VEC_AT(v, Expr, i)));
    return r;
}

static Pattern *clone_pat(Pattern *p) {
    if (!p) return NULL;
    Pattern *q = NEW(Pattern);
    *q = *p;
    memset(&q->subs, 0, sizeof(Vec));
    for (int i = 0; i < p->subs.len; i++) vec_push(&q->subs, clone_pat(VEC_AT(&p->subs, Pattern, i)));
    q->fields = p->fields;   /* names are interned, share */
    Vec f; memset(&f, 0, sizeof f);
    for (int i = 0; i < p->fields.len; i++) vec_push(&f, p->fields.data[i]);
    q->fields = f;
    q->sym = NULL;
    return q;
}

static Expr *clone_expr(Expr *e) {
    if (!e) return NULL;
    Expr *c = NEW(Expr);
    *c = *e;
    c->sym = NULL; c->target = NULL; c->extra = NULL; c->type = NULL;
    c->a = clone_expr(e->a);
    c->b = clone_expr(e->b);
    c->c = clone_expr(e->c);
    memset(&c->list, 0, sizeof(Vec));
    if (e->kind == E_STRUCT) {
        for (int i = 0; i < e->list.len; i++) {
            FieldInit *fi = VEC_AT(&e->list, FieldInit, i);
            FieldInit *n = NEW(FieldInit);
            *n = *fi;
            n->value = clone_expr(fi->value);
            vec_push(&c->list, n);
        }
    } else if (e->kind == E_MATCH) {
        for (int i = 0; i < e->list.len; i++) {
            MatchArm *a = VEC_AT(&e->list, MatchArm, i);
            MatchArm *n = NEW(MatchArm);
            *n = *a;
            n->pat = clone_pat(a->pat);
            n->guard = clone_expr(a->guard);
            n->body = clone_expr(a->body);
            n->block = clone_stmt(a->block);
            vec_push(&c->list, n);
        }
    } else {
        c->list = clone_vec_expr(&e->list);
    }
    memset(&c->params, 0, sizeof(Vec));
    for (int i = 0; i < e->params.len; i++) {
        Param *p = VEC_AT(&e->params, Param, i);
        Param *n = NEW(Param); *n = *p;
        vec_push(&c->params, n);
    }
    c->body = clone_stmt(e->body);
    c->stmt = clone_stmt(e->stmt);
    return c;
}

static Stmt *clone_stmt(Stmt *s) {
    if (!s) return NULL;
    Stmt *c = NEW(Stmt);
    *c = *s;
    c->sym = NULL; c->sym2 = NULL; c->type = NULL;
    c->a = clone_expr(s->a);
    c->b = clone_expr(s->b);
    c->then_s = clone_stmt(s->then_s);
    c->else_s = clone_stmt(s->else_s);
    memset(&c->list, 0, sizeof(Vec));
    for (int i = 0; i < s->list.len; i++) vec_push(&c->list, clone_stmt(VEC_AT(&s->list, Stmt, i)));
    return c;
}

/* ------------------------------------------------------------------ */
/* function instances                                                   */
/* ------------------------------------------------------------------ */

static const char *mangle(Decl *d, Vec *targs) {
    Buf b; memset(&b, 0, sizeof b);
    const char *mp = d->mod ? d->mod->modpath : "?";
    for (const char *p = mp; *p; p++) buf_u8(&b, (*p == '/' || *p == '.' || *p == '-') ? '_' : *p);
    buf_u8(&b, '.');
    if (d->recv) { buf_str(&b, d->recv); buf_u8(&b, '.'); }
    buf_str(&b, d->name);
    if (targs && targs->len) {
        buf_u8(&b, '[');
        for (int i = 0; i < targs->len; i++) {
            if (i) buf_u8(&b, ',');
            buf_str(&b, ty_str_of(VEC_AT(targs, Type, i)));
        }
        buf_u8(&b, ']');
    }
    buf_u8(&b, 0);
    const char *s = intern((char *)b.data);
    buf_free(&b);
    return s;
}

static void check_fn_body(FnInst *fi);

static FnInst *instantiate(Decl *d, Vec *targs, Span sp) {
    const char *nm = mangle(d, targs);
    for (int i = 0; i < d->insts.len; i++) {
        FnInst *f = VEC_AT(&d->insts, FnInst, i);
        if (f->name == nm) return f;
    }
    FnInst *fi = NEW(FnInst);
    fi->name = nm;
    fi->mod = d->mod;
    fi->span = d->span;
    fi->doc = d->doc;
    if (targs) for (int i = 0; i < targs->len; i++) vec_push(&fi->targs, targs->data[i]);
    /* clone the AST for generic instances so annotations don't collide */
    if (targs && targs->len) {
        Decl *cd = NEW(Decl);
        *cd = *d;
        memset(&cd->insts, 0, sizeof(Vec));
        cd->body = clone_stmt(d->body);
        fi->decl = cd;
    } else {
        fi->decl = d;
    }
    fi->index = g_unit.fns.len;
    vec_push(&g_unit.fns, fi);
    vec_push(&d->insts, fi);
    vec_push(&work_queue, fi);
    return fi;
}

/* ------------------------------------------------------------------ */
/* expression checking                                                  */
/* ------------------------------------------------------------------ */

static Type *check_expr(Expr *e, Type *want);
static void check_stmt(Stmt *s);
static void check_block(Stmt *s, Scope *sc);
static void check_stmts(Stmt *s, Scope *sc);
static void warn_unused(Scope *sc);

static int assignable(Type *from, Type *to) {
    if (!from || !to) return 1;
    if (ty_eq(from, to)) return 1;
    if (to->kind == TY_OPT) {
        if (from->kind == TY_VOID) return 1;       /* nil */
        if (ty_eq(from, to->elem)) return 1;
        if (from->kind == TY_OPT && from->elem == NULL) return 1;
        if (from->kind == TY_OPT && to->elem && ty_eq(from->elem, to->elem)) return 1;
    }
    if (to->kind == TY_RES) {
        if (ty_eq(from, to->elem)) return 1;
        if (from->kind == TY_RES && from->elem == NULL) return 1;
        if (from->kind == TY_RES && to->elem && ty_eq(from->elem, to->elem)) return 1;
    }
    if (from->kind == TY_OPT && from->elem == NULL && to->kind == TY_OPT) return 1;
    if (from->kind == TY_ANY || to->kind == TY_ANY) return 1;
    /* empty list / map literal adapts */
    if (from->kind == TY_LIST && to->kind == TY_LIST && from->elem->kind == TY_ANY) return 1;
    if (from->kind == TY_MAP && to->kind == TY_MAP &&
        from->elem->kind == TY_ANY && from->val->kind == TY_ANY) return 1;
    return 0;
}

static void want_err(Span sp, Type *want, Type *got, const char *ctx) {
    Diag *d = serr(sp, "type mismatch%s%s", ctx ? " " : "", ctx ? ctx : "");
    diag_note(d, NOSPAN, "expected `%s`", ty_str_of(want));
    diag_note(d, NOSPAN, "found    `%s`", ty_str_of(got));
    if (want && got) {
        if (want->kind == TY_FLOAT && got->kind == TY_INT)
            diag_note(d, NOSPAN, "help: convert with `float(x)`, or write the literal as `1.0`");
        else if (want->kind == TY_INT && got->kind == TY_FLOAT)
            diag_note(d, NOSPAN, "help: convert with `int(x)` (truncates toward zero)");
        else if (want->kind == TY_STR && got->kind != TY_STR)
            diag_note(d, NOSPAN, "help: convert with `str(x)`");
        else if (got->kind == TY_OPT && want->kind != TY_OPT)
            diag_note(d, NOSPAN, "help: unwrap with `x ?? default` or `if let v = x { ... }`");
        else if (got->kind == TY_RES && want->kind != TY_RES)
            diag_note(d, NOSPAN, "help: unwrap with `x?` (propagates the error) or `x ?? default`");
    }
}

static Type *coerce(Expr *e, Type *got, Type *want, const char *ctx) {
    if (!want || !got) return got;
    if (assignable(got, want)) return want->kind == TY_ANY ? got : want;
    want_err(e->span, want, got, ctx);
    return want;
}

/* Unify a syntactic type against a concrete type, filling in bindings. */
static int unify(TypeExpr *te, Type *t, Subst *s, Module *m) {
    if (!te || !t) return 1;
    switch (te->kind) {
        case TE_NAME: {
            /* is this a generic parameter we are solving for? */
            for (int i = 0; i < s->names.len; i++) {
                if ((const char *)s->names.data[i] == te->name) {
                    Type *ex = VEC_AT(&s->types, Type, i);
                    if (!ex) { s->types.data[i] = t; return 1; }
                    return ty_eq(ex, t);
                }
            }
            if (te->name == intern("List") && te->args.len == 1 && t->kind == TY_LIST)
                return unify(VEC_AT(&te->args, TypeExpr, 0), t->elem, s, m);
            if (te->name == intern("Map") && te->args.len == 2 && t->kind == TY_MAP)
                return unify(VEC_AT(&te->args, TypeExpr, 0), t->elem, s, m) &&
                       unify(VEC_AT(&te->args, TypeExpr, 1), t->val, s, m);
            if ((t->kind == TY_STRUCT || t->kind == TY_ENUM) && t->decl &&
                t->decl->name == te->name) {
                for (int i = 0; i < te->args.len && i < t->targs.len; i++)
                    if (!unify(VEC_AT(&te->args, TypeExpr, i), VEC_AT(&t->targs, Type, i), s, m))
                        return 0;
                return 1;
            }
            return 1;
        }
        case TE_LIST: return t->kind == TY_LIST ? unify(te->sub, t->elem, s, m) : 1;
        case TE_OPT:  return t->kind == TY_OPT ? unify(te->sub, t->elem, s, m) : unify(te->sub, t, s, m);
        case TE_RES:  return t->kind == TY_RES ? unify(te->sub, t->elem, s, m) : 1;
        case TE_MAP:  if (t->kind != TY_MAP) return 1;
                      return unify(te->sub, t->elem, s, m) && unify(te->sub2, t->val, s, m);
        case TE_FN: {
            if (t->kind != TY_FN) return 1;
            for (int i = 0; i < te->args.len && i < t->params.len; i++)
                unify(VEC_AT(&te->args, TypeExpr, i), VEC_AT(&t->params, Type, i), s, m);
            if (te->sub) unify(te->sub, t->ret, s, m);
            return 1;
        }
    }
    return 1;
}

/* Look up a symbol, walking out through enclosing closures and adding
   captures as needed. */
static Sym *lookup_capture(FnCtx *fc, const char *name) {
    if (!fc) return NULL;
    Sym *s = scope_get(fc->scope, name);
    if (s) return s;
    if (!fc->parent) return NULL;
    Sym *outer = lookup_capture(fc->parent, name);
    if (!outer) return NULL;
    if (outer->kind == SYM_FN || outer->kind == SYM_CONST ||
        outer->kind == SYM_TYPE || outer->kind == SYM_MOD) return outer;
    outer->boxed = 1;
    Sym *cap = NEW(Sym);
    cap->kind = SYM_CAPTURE;
    cap->name = name;
    cap->type = outer->type;
    cap->is_mut = outer->is_mut;
    cap->slot = fc->inst->captures.len;
    cap->decl = outer->decl;
    cap->span = outer->span;
    vec_push(&fc->inst->captures, outer);
    scope_put(fc->scope, cap);
    return cap;
}

static int new_slot(void) {
    FnCtx *fc = cur_fn;
    return fc->nslots++;
}

static Sym *declare_local(const char *name, Type *t, int is_mut, Span sp) {
    Sym *s = NEW(Sym);
    s->kind = SYM_LOCAL;
    s->name = name;
    s->type = t;
    s->is_mut = is_mut;
    s->span = sp;
    s->slot = new_slot();
    scope_put(cur_fn->scope, s);
    return s;
}

/* ---- builtin dispatch ---- */

static struct { const char *n; int id; } builtins[] = {
    {"str", BI_STR}, {"int", BI_INT}, {"float", BI_FLOAT}, {"byte", BI_BYTE},
    {"bool", BI_BOOL}, {"len", BI_LEN}, {"print", BI_PRINT}, {"println", BI_PRINTLN},
    {"panic", BI_PANIC}, {"assert", BI_ASSERT}, {"assert_eq", BI_ASSERT_EQ},
    {"assert_ne", BI_ASSERT_NE}, {"ok", BI_OK}, {"err", BI_ERR},
    {"err_code", BI_ERR_CODE}, {NULL, 0}
};

static int builtin_id(const char *n) {
    for (int i = 0; builtins[i].n; i++)
        if (intern(builtins[i].n) == n) return builtins[i].id;
    return BI_NONE;
}

static FnInst *runtime_fn(const char *modpath, const char *name, Span sp) {
    Module *m = load_module(modpath, NULL, sp);
    if (!m) { serr(sp, "internal: runtime module `%s` not found", modpath); return NULL; }
    for (int i = 0; i < m->decls.len; i++) {
        Decl *d = VEC_AT(&m->decls, Decl, i);
        if (d->kind == D_FN && d->name == intern(name) && !d->recv)
            return instantiate(d, NULL, sp);
    }
    serr(sp, "internal: runtime function `%s.%s` not found", modpath, name);
    return NULL;
}

FnInst *find_to_str(Type *t) {
    const char *k = typekey(t);
    if (!k) return NULL;
    Decl *d = mtab_get(k, intern("to_str"));
    if (!d) return NULL;
    Vec targs; memset(&targs, 0, sizeof targs);
    if (d->generics.len) {
        if (t->targs.len != d->generics.len) {
            if (t->kind == TY_LIST) vec_push(&targs, t->elem);
            else if (t->kind == TY_MAP) { vec_push(&targs, t->elem); vec_push(&targs, t->val); }
            else if (t->kind == TY_OPT || t->kind == TY_RES) vec_push(&targs, t->elem);
            else return NULL;
        } else {
            for (int i = 0; i < t->targs.len; i++) vec_push(&targs, t->targs.data[i]);
        }
    }
    return instantiate(d, &targs, NOSPAN);
}

/* Resolve a method: returns the Decl and fills targs. */
static Decl *find_method(Type *recv, const char *name, Vec *targs_out, Span sp) {
    const char *k = typekey(recv);
    if (!k) return NULL;
    Decl *d = mtab_get(k, name);
    if (!d) return NULL;
    memset(targs_out, 0, sizeof(Vec));
    if (d->generics.len) {
        /* bind receiver generics positionally */
        if (recv->kind == TY_LIST) vec_push(targs_out, recv->elem);
        else if (recv->kind == TY_MAP) { vec_push(targs_out, recv->elem); vec_push(targs_out, recv->val); }
        else if (recv->kind == TY_OPT || recv->kind == TY_RES) vec_push(targs_out, recv->elem);
        else for (int i = 0; i < recv->targs.len; i++) vec_push(targs_out, recv->targs.data[i]);
        while (targs_out->len < d->generics.len) vec_push(targs_out, ty_any);
        targs_out->len = d->generics.len;
    }
    return d;
}

static Type *inst_ret_type(FnInst *fi) { return fi ? fi->ret : ty_void; }

/* Check a call to a known FnInst-producing Decl. */
static Type *check_static_call(Expr *e, Decl *d, Vec *explicit_targs, Expr *recv,
                               Type *recv_type, Vec *args, Span sp) {
    Subst sub; memset(&sub, 0, sizeof sub);
    for (int i = 0; i < d->generics.len; i++)
        subst_put(&sub, (const char *)d->generics.data[i], NULL);

    if (explicit_targs && explicit_targs->len) {
        if (explicit_targs->len != d->generics.len) {
            serr(sp, "`%s` takes %d type argument%s, found %d",
                 d->name, d->generics.len, d->generics.len == 1 ? "" : "s", explicit_targs->len);
        }
        for (int i = 0; i < d->generics.len && i < explicit_targs->len; i++)
            subst_put(&sub, (const char *)d->generics.data[i], VEC_AT(explicit_targs, Type, i));
    }

    /* bind from receiver */
    if (recv_type && d->recv) {
        if (recv_type->kind == TY_LIST && d->generics.len >= 1)
            subst_put(&sub, (const char *)d->generics.data[0], recv_type->elem);
        else if (recv_type->kind == TY_MAP && d->generics.len >= 2) {
            subst_put(&sub, (const char *)d->generics.data[0], recv_type->elem);
            subst_put(&sub, (const char *)d->generics.data[1], recv_type->val);
        } else if ((recv_type->kind == TY_OPT || recv_type->kind == TY_RES) && d->generics.len >= 1)
            subst_put(&sub, (const char *)d->generics.data[0], recv_type->elem);
        else for (int i = 0; i < recv_type->targs.len && i < d->generics.len; i++)
            subst_put(&sub, (const char *)d->generics.data[i], VEC_AT(&recv_type->targs, Type, i));
    }

    int nparams = d->params.len;
    if (args->len != nparams) {
        Diag *dg = serr(sp, "`%s` expects %d argument%s, found %d",
                        d->name, nparams, nparams == 1 ? "" : "s", args->len);
        diag_note(dg, d->span, "`%s` is declared here", d->name);
        /* still check what we can */
    }

    /* first pass: infer generics from argument types */
    if (d->generics.len) {
        for (int i = 0; i < args->len && i < nparams; i++) {
            Param *p = VEC_AT(&d->params, Param, i);
            Expr *a = VEC_AT(args, Expr, i);
            if (a->kind == E_LAMBDA) {
                /* Check the lambda against the partially-solved parameter type;
                   its inferred result type usually pins the remaining
                   parameters (this is what makes `xs.map(|x| ...)` work). */
                Type *pt = resolve_type(p->type, d->mod, &sub);
                if (pt && pt->kind == TY_FN) {
                    int solved = 1;
                    for (int q = 0; q < pt->params.len; q++)
                        if (VEC_AT(&pt->params, Type, q)->kind == TY_ANY) solved = 0;
                    if (solved) {
                        Type *at = check_expr(a, pt);
                        unify(p->type, at, &sub, d->mod);
                    }
                }
                continue;
            }
            Type *at = check_expr(a, NULL);
            unify(p->type, at, &sub, d->mod);
        }
        for (int i = 0; i < d->generics.len; i++) {
            if (!VEC_AT(&sub.types, Type, i)) {
                Diag *dg = serr(sp, "cannot infer type parameter `%s` of `%s`",
                                (const char *)d->generics.data[i], d->name);
                diag_note(dg, NOSPAN, "help: specify it explicitly, e.g. `%s[Int](...)`", d->name);
                sub.types.data[i] = ty_int;
            }
        }
    }

    Vec targs; memset(&targs, 0, sizeof targs);
    for (int i = 0; i < d->generics.len; i++) vec_push(&targs, sub.types.data[i]);

    /* now check arguments against substituted parameter types */
    for (int i = 0; i < args->len && i < nparams; i++) {
        Param *p = VEC_AT(&d->params, Param, i);
        Type *pt = resolve_type(p->type, d->mod, &sub);
        Expr *a = VEC_AT(args, Expr, i);
        Type *at = check_expr(a, pt);
        if (!assignable(at, pt)) {
            char ctx[256];
            snprintf(ctx, sizeof ctx, "in argument %d of `%s`", i + 1, d->name);
            want_err(a->span, pt, at, ctx);
        }
        a->type = pt;
    }
    for (int i = nparams; i < args->len; i++) check_expr(VEC_AT(args, Expr, i), NULL);

    if (recv_type && d->has_self && recv) recv->type = recv_type;

    int depth = inst_stack.len;
    if (depth > 60) {
        serr(sp, "generic instantiation is too deep (recursive generic function?)");
        return ty_int;
    }
    FnInst *fi = instantiate(d, &targs, sp);
    e->target = fi;
    e->is_static = 1;
    Type *ret = resolve_type(d->ret, d->mod, &sub);
    if (fi && !fi->ret) fi->ret = ret;
    return ret;
}

/* ---- pattern checking ---- */

static void check_pattern(Pattern *p, Type *subject) {
    if (!p) return;
    p->type = subject;
    switch (p->kind) {
        case P_WILD: break;
        case P_BIND: {
            Sym *s = declare_local(p->name, subject, 0, p->span);
            p->sym = s;
            p->slot = s->slot;
            break;
        }
        case P_INT:
            if (subject && subject->kind != TY_INT && subject->kind != TY_BYTE)
                serr(p->span, "integer pattern cannot match `%s`", ty_str_of(subject));
            break;
        case P_FLOAT:
            if (subject && subject->kind != TY_FLOAT)
                serr(p->span, "float pattern cannot match `%s`", ty_str_of(subject));
            break;
        case P_CHAR:
            if (subject && subject->kind != TY_BYTE && subject->kind != TY_INT)
                serr(p->span, "character pattern cannot match `%s`", ty_str_of(subject));
            break;
        case P_STR:
            if (subject && subject->kind != TY_STR)
                serr(p->span, "string pattern cannot match `%s`", ty_str_of(subject));
            break;
        case P_BOOL:
            if (subject && subject->kind != TY_BOOL)
                serr(p->span, "boolean pattern cannot match `%s`", ty_str_of(subject));
            break;
        case P_NIL:
            if (subject && subject->kind != TY_OPT)
                serr(p->span, "`nil` pattern cannot match `%s`", ty_str_of(subject));
            break;
        case P_SOME:
            if (!subject || subject->kind != TY_OPT) {
                serr(p->span, "`some(...)` pattern requires an optional, found `%s`",
                     subject ? ty_str_of(subject) : "?");
                break;
            }
            if (p->subs.len == 1) check_pattern(VEC_AT(&p->subs, Pattern, 0), subject->elem);
            break;
        case P_OK: case P_ERR:
            if (!subject || subject->kind != TY_RES) {
                serr(p->span, "`%s(...)` pattern requires a `!T` value, found `%s`",
                     p->kind == P_OK ? "ok" : "err", subject ? ty_str_of(subject) : "?");
                break;
            }
            if (p->subs.len == 1)
                check_pattern(VEC_AT(&p->subs, Pattern, 0),
                              p->kind == P_OK ? subject->elem : ty_error);
            break;
        case P_ENUM: {
            if (!subject || subject->kind != TY_ENUM) {
                serr(p->span, "enum pattern `%s.%s` cannot match `%s`",
                     p->tyname, p->name, subject ? ty_str_of(subject) : "?");
                break;
            }
            if (subject->decl && subject->decl->name != p->tyname) {
                serr(p->span, "pattern is for enum `%s` but the value has type `%s`",
                     p->tyname, ty_str_of(subject));
                break;
            }
            if (p->modname) {
                Module *om = find_used_module(cur_mod, p->modname);
                if (!om || (subject->decl && subject->decl->mod != om))
                    serr(p->span, "`%s.%s` is not the type of this value", p->modname, p->tyname);
            }
            int v = enum_variant_index(subject, p->name);
            if (v < 0) {
                Diag *d = serr(p->span, "`%s` has no variant `%s`", ty_str_of(subject), p->name);
                if (subject->decl) {
                    for (int i = 0; i < subject->decl->variants.len; i++) {
                        EnumVariant *ev = VEC_AT(&subject->decl->variants, EnumVariant, i);
                        if (edit_dist(p->name, ev->name) <= 2) {
                            diag_note(d, NOSPAN, "did you mean `%s.%s`?", subject->decl->name, ev->name);
                            break;
                        }
                    }
                }
                break;
            }
            p->tag = v;
            int n = enum_payload_count(subject, v);
            if (p->subs.len && p->subs.len != n) {
                serr(p->span, "variant `%s.%s` has %d field%s, pattern binds %d",
                     ty_str_of(subject), p->name, n, n == 1 ? "" : "s", p->subs.len);
            }
            for (int i = 0; i < p->subs.len && i < n; i++)
                check_pattern(VEC_AT(&p->subs, Pattern, i), enum_payload_type(subject, v, i));
            break;
        }
        case P_STRUCT: {
            if (!subject || subject->kind != TY_STRUCT) {
                serr(p->span, "struct pattern cannot match `%s`", subject ? ty_str_of(subject) : "?");
                break;
            }
            for (int i = 0; i < p->subs.len; i++) {
                const char *fn = (const char *)p->fields.data[i];
                int fi = struct_field_index(subject, fn);
                if (fi < 0) { serr(p->span, "`%s` has no field `%s`", ty_str_of(subject), fn); continue; }
                Pattern *sub = VEC_AT(&p->subs, Pattern, i);
                sub->tag = fi;
                check_pattern(sub, struct_field_type(subject, fi));
            }
            break;
        }
        case P_OR:
            for (int i = 0; i < p->subs.len; i++) {
                Pattern *s = VEC_AT(&p->subs, Pattern, i);
                if (s->kind == P_BIND) {
                    serr(s->span, "alternation patterns cannot bind variables");
                    continue;
                }
                check_pattern(s, subject);
            }
            break;
    }
}

/* Approximate exhaustiveness: enums, bools, optionals, results. */
static int patterns_exhaustive(Vec *arms, Type *subject) {
    int has_wild = 0;
    for (int i = 0; i < arms->len; i++) {
        MatchArm *a = VEC_AT(arms, MatchArm, i);
        if (a->guard) continue;
        if (a->pat->kind == P_WILD || a->pat->kind == P_BIND) has_wild = 1;
    }
    if (has_wild) return 1;
    if (!subject) return 0;
    if (subject->kind == TY_ENUM && subject->decl) {
        int n = subject->decl->variants.len;
        char seen[256];
        memset(seen, 0, sizeof seen);
        if (n > 256) return 0;
        for (int i = 0; i < arms->len; i++) {
            MatchArm *a = VEC_AT(arms, MatchArm, i);
            if (a->guard) continue;
            if (a->pat->kind == P_ENUM && a->pat->tag < n) seen[a->pat->tag] = 1;
            if (a->pat->kind == P_OR)
                for (int j = 0; j < a->pat->subs.len; j++) {
                    Pattern *s = VEC_AT(&a->pat->subs, Pattern, j);
                    if (s->kind == P_ENUM && s->tag < n) seen[s->tag] = 1;
                }
        }
        for (int i = 0; i < n; i++) if (!seen[i]) return 0;
        return 1;
    }
    if (subject->kind == TY_BOOL) {
        int t = 0, f = 0;
        for (int i = 0; i < arms->len; i++) {
            MatchArm *a = VEC_AT(arms, MatchArm, i);
            if (a->guard) continue;
            if (a->pat->kind == P_BOOL) { if (a->pat->ival) t = 1; else f = 1; }
        }
        return t && f;
    }
    if (subject->kind == TY_OPT) {
        int nil = 0, some = 0;
        for (int i = 0; i < arms->len; i++) {
            MatchArm *a = VEC_AT(arms, MatchArm, i);
            if (a->guard) continue;
            if (a->pat->kind == P_NIL) nil = 1;
            if (a->pat->kind == P_SOME) some = 1;
        }
        return nil && some;
    }
    if (subject->kind == TY_RES) {
        int o = 0, r = 0;
        for (int i = 0; i < arms->len; i++) {
            MatchArm *a = VEC_AT(arms, MatchArm, i);
            if (a->guard) continue;
            if (a->pat->kind == P_OK) o = 1;
            if (a->pat->kind == P_ERR) r = 1;
        }
        return o && r;
    }
    return 0;
}

/* ---- the expression checker ---- */

static Type *check_binary(Expr *e) {
    Type *a = check_expr(e->a, NULL);
    Type *want_b = NULL;
    if (e->op == T_SHL || e->op == T_SHR) want_b = ty_int;
    Type *b = check_expr(e->b, want_b ? want_b : a);
    int op = e->op;

    if (op == T_AND || op == T_OR) {
        if (a->kind != TY_BOOL || b->kind != TY_BOOL) {
            Diag *d = serr(e->span, "`%s` requires `Bool` operands",
                           op == T_AND ? "and" : "or");
            diag_note(d, NOSPAN, "found `%s` %s `%s`", ty_str_of(a),
                      op == T_AND ? "and" : "or", ty_str_of(b));
        }
        e->idx = OPC_BOOL;
        return ty_bool;
    }

    if (op == T_EQEQ || op == T_BANGEQ) {
        if (!assignable(a, b) && !assignable(b, a)) {
            Diag *d = serr(e->span, "cannot compare `%s` with `%s`", ty_str_of(a), ty_str_of(b));
            diag_note(d, NOSPAN, "`==` requires both sides to have the same type");
            if ((a->kind == TY_INT && b->kind == TY_FLOAT) ||
                (a->kind == TY_FLOAT && b->kind == TY_INT))
                diag_note(d, NOSPAN, "help: Vela never converts numbers implicitly; use `float(x)` or `int(x)`");
        }
        Type *t = (a->kind == TY_VOID) ? b : a;
        switch (t->kind) {
            case TY_INT: case TY_BOOL: case TY_BYTE: e->idx = OPC_INT; break;
            case TY_FLOAT: e->idx = OPC_FLOAT; break;
            case TY_ENUM: e->idx = t->is_prim ? OPC_INT : OPC_ANY; break;
            default: e->idx = OPC_ANY; break;
        }
        if (e->idx == OPC_ANY) e->extra = (void *)t;
        return ty_bool;
    }

    if (op == T_LT || op == T_LE || op == T_GT || op == T_GE) {
        if (!ty_eq(a, b)) {
            serr(e->span, "cannot compare `%s` with `%s`", ty_str_of(a), ty_str_of(b));
            return ty_bool;
        }
        switch (a->kind) {
            case TY_INT: case TY_BYTE: e->idx = OPC_INT; break;
            case TY_FLOAT: e->idx = OPC_FLOAT; break;
            case TY_STR: e->idx = OPC_STR; break;
            default: {
                Diag *d = serr(e->span, "`%s` is not defined for `%s`",
                               tok_names[op], ty_str_of(a));
                diag_note(d, NOSPAN, "ordering comparisons work on `Int`, `Float`, `Byte` and `Str`");
                break;
            }
        }
        return ty_bool;
    }

    /* arithmetic / bitwise */
    if (!ty_eq(a, b)) {
        Diag *d = serr(e->span, "cannot apply `%s` to `%s` and `%s`",
                       tok_names[op], ty_str_of(a), ty_str_of(b));
        if ((a->kind == TY_INT && b->kind == TY_FLOAT) || (a->kind == TY_FLOAT && b->kind == TY_INT))
            diag_note(d, NOSPAN, "help: Vela has no implicit numeric conversion; use `float(x)` or `int(x)`");
        else if (a->kind == TY_STR || b->kind == TY_STR)
            diag_note(d, NOSPAN, "help: use `str(x)` or interpolation `\"{x}\"` to build strings");
        return a;
    }
    switch (a->kind) {
        case TY_INT:  e->idx = OPC_INT; return ty_int;
        case TY_BYTE: e->idx = OPC_BYTE; return ty_byte;
        case TY_FLOAT:
            if (op == T_PERCENT || op == T_AMP || op == T_PIPE || op == T_CARET ||
                op == T_SHL || op == T_SHR) {
                serr(e->span, "`%s` is not defined for `Float`", tok_names[op]);
                return ty_float;
            }
            e->idx = OPC_FLOAT; return ty_float;
        case TY_STR:
            if (op != T_PLUS) {
                serr(e->span, "`%s` is not defined for `Str` (only `+` concatenates)", tok_names[op]);
                return ty_str;
            }
            e->idx = OPC_STR; return ty_str;
        case TY_LIST:
            if (op != T_PLUS) {
                serr(e->span, "`%s` is not defined for `%s`", tok_names[op], ty_str_of(a));
                return a;
            }
            e->idx = OPC_LIST; return a;
        default: {
            Diag *d = serr(e->span, "`%s` is not defined for `%s`", tok_names[op], ty_str_of(a));
            diag_note(d, NOSPAN, "arithmetic works on `Int`, `Float` and `Byte`; `+` also works on `Str` and lists");
            return a;
        }
    }
}

static Type *check_interp(Expr *e) {
    for (int i = 0; i < e->list.len; i++) {
        Expr *p = VEC_AT(&e->list, Expr, i);
        if (p->kind == E_STR) { p->type = ty_str; continue; }
        Type *t = check_expr(p, NULL);
        p->type = t;
        if (t->kind != TY_STR) {
            FnInst *ts = find_to_str(t);
            if (ts) p->extra = ts;
        }
    }
    return ty_str;
}

static Type *check_lambda(Expr *e, Type *want) {
    Vec ptypes; memset(&ptypes, 0, sizeof ptypes);
    for (int i = 0; i < e->params.len; i++) {
        Param *p = VEC_AT(&e->params, Param, i);
        Type *t = NULL;
        if (p->type) t = resolve_type(p->type, cur_mod, cur_fn->subst);
        else if (want && want->kind == TY_FN && i < want->params.len)
            t = VEC_AT(&want->params, Type, i);
        if (!t) {
            serr(p->span, "cannot infer the type of lambda parameter `%s`", p->name);
            diag_note(diag_add(DIAG_NOTE, NOSPAN, "help: annotate it: `|%s: Int| ...`", p->name), NOSPAN, "");
            t = ty_int;
        }
        vec_push(&ptypes, t);
    }
    Type *ret = NULL;
    if (e->texpr) ret = resolve_type(e->texpr, cur_mod, cur_fn->subst);
    else if (want && want->kind == TY_FN) ret = want->ret;
    if (ret && ret->kind == TY_ANY) ret = NULL;

    /* build a synthetic FnInst for the lambda body */
    static int lambda_counter = 0;
    char nm[128];
    snprintf(nm, sizeof nm, "%s$lambda%d", cur_fn->inst->name, lambda_counter++);
    FnInst *fi = NEW(FnInst);
    fi->name = intern(nm);
    fi->mod = cur_mod;
    fi->span = e->span;
    fi->is_lambda = 1;
    fi->enclosing = cur_fn->inst;
    fi->index = g_unit.fns.len;
    vec_push(&g_unit.fns, fi);

    Decl *d = NEW(Decl);
    d->kind = D_FN;
    d->name = fi->name;
    d->mod = cur_mod;
    d->span = e->span;
    d->body = e->body;
    for (int i = 0; i < e->params.len; i++) vec_push(&d->params, e->params.data[i]);
    fi->decl = d;
    fi->nparams = e->params.len;
    for (int i = 0; i < ptypes.len; i++) vec_push(&fi->param_types, ptypes.data[i]);

    /* check the body in a nested function context */
    FnCtx fc;
    memset(&fc, 0, sizeof fc);
    fc.parent = cur_fn;
    fc.inst = fi;
    fc.scope = scope_new(NULL);
    fc.mod = cur_mod;
    fc.subst = cur_fn->subst;
    fc.ret = ret;
    fc.nslots = 1;    /* slot 0 is the environment pointer */
    FnCtx *save = cur_fn;
    cur_fn = &fc;
    for (int i = 0; i < e->params.len; i++) {
        Param *p = VEC_AT(&e->params, Param, i);
        Sym *s = NEW(Sym);
        s->kind = SYM_PARAM;
        s->name = p->name;
        s->type = VEC_AT(&ptypes, Type, i);
        s->slot = fc.nslots++;
        s->span = p->span;
        s->used = 1;
        scope_put(fc.scope, s);
        vec_push(&fi->param_syms, s);
    }
    check_block(e->body, fc.scope);
    if (!fc.ret) fc.ret = ty_void;
    fi->ret = fc.ret;
    fi->nslots = fc.nslots;
    cur_fn = save;

    e->target = fi;
    Vec ps; memset(&ps, 0, sizeof ps);
    for (int i = 0; i < ptypes.len; i++) vec_push(&ps, ptypes.data[i]);
    return ty_fn(ps, fi->ret);
}

static Type *module_member(Expr *e, Module *m, const char *name, Vec *args,
                           Vec *targs, Span sp, int is_call) {
    for (int i = 0; i < m->decls.len; i++) {
        Decl *d = VEC_AT(&m->decls, Decl, i);
        if (d->name != name) continue;
        if (d->kind == D_FN && !d->recv) {
            if (!d->is_pub && m != cur_mod) {
                Diag *dg = serr(sp, "`%s.%s` is private", m->name, name);
                diag_note(dg, d->span, "add `pub` to export it");
            }
            if (!is_call) {
                /* function used as a value */
                Vec no; memset(&no, 0, sizeof no);
                if (d->generics.len) {
                    serr(sp, "cannot use generic function `%s` as a value without type arguments", name);
                }
                FnInst *fi = instantiate(d, &no, sp);
                e->target = fi;
                e->builtin = BI_NONE;
                Vec ps; memset(&ps, 0, sizeof ps);
                for (int j = 0; j < d->params.len; j++)
                    vec_push(&ps, resolve_type(VEC_AT(&d->params, Param, j)->type, m, NULL));
                Type *r = resolve_type(d->ret, m, NULL);
                if (fi && !fi->ret) fi->ret = r;
                return ty_fn(ps, r);
            }
            return check_static_call(e, d, targs, NULL, NULL, args, sp);
        }
        if (d->kind == D_CONST) {
            if (!d->is_pub && m != cur_mod) serr(sp, "`%s.%s` is private", m->name, name);
            e->sym = d->sym;
            e->builtin = BI_NONE;
            return d->type ? d->type : ty_int;
        }
    }
    Diag *dg = serr(sp, "module `%s` has no member `%s`", m->name, name);
    for (int i = 0; i < m->decls.len; i++) {
        Decl *d = VEC_AT(&m->decls, Decl, i);
        if (d->name && d->is_pub && edit_dist(name, d->name) <= 2) {
            diag_note(dg, NOSPAN, "did you mean `%s.%s`?", m->name, d->name);
            break;
        }
    }
    return ty_int;
}

static Type *check_builtin(Expr *e, int bi, Vec *args, Span sp);

static Type *check_expr(Expr *e, Type *want) {
    if (!e) return ty_void;
    if (e->type) return e->type;
    Type *r = ty_void;
    switch (e->kind) {
        case E_INT:   r = (want && want->kind == TY_BYTE) ? ty_byte : ty_int; break;
        case E_FLOAT: r = ty_float; break;
        case E_BOOL:  r = ty_bool; break;
        case E_CHAR:  r = ty_byte; break;
        case E_STR:   r = ty_str; break;
        case E_INTERP: r = check_interp(e); break;
        case E_NIL:
            if (want && want->kind == TY_OPT) r = want;
            else { r = ty_opt(NULL); }
            break;
        case E_RANGE: {
            Type *a = check_expr(e->a, ty_int);
            if (a->kind != TY_INT) serr(e->a->span, "range bounds must be `Int`, found `%s`", ty_str_of(a));
            if (e->b) {
                Type *b = check_expr(e->b, ty_int);
                if (b->kind != TY_INT) serr(e->b->span, "range bounds must be `Int`, found `%s`", ty_str_of(b));
            } else {
                serr(e->span, "an open-ended range `a..` is only allowed when slicing");
            }
            r = ty_range;
            break;
        }
        case E_IDENT: {
            if (e->name == intern("void") && !lookup_capture(cur_fn, e->name)) {
                e->builtin = BI_VOIDVAL;
                r = ty_void;
                break;
            }
            int bi = builtin_id(e->name);
            Sym *s = lookup_capture(cur_fn, e->name);
            if (!s && bi) { e->builtin = bi; r = ty_void; break; }
            if (!s) {
                Diag *d = serr(e->span, "cannot find `%s` in this scope", e->name);
                const char *g = suggest_in_scope(cur_fn->scope, e->name);
                if (g) diag_note(d, NOSPAN, "did you mean `%s`?", g);
                r = ty_int;
                break;
            }
            s->used = 1;
            e->sym = s;
            if (s->kind == SYM_MOD) {
                serr(e->span, "`%s` is a module; use `%s.name` to access its members", e->name, e->name);
                r = ty_int;
            } else if (s->kind == SYM_TYPE) {
                serr(e->span, "`%s` is a type, not a value", e->name);
                r = ty_int;
            } else if (s->kind == SYM_FN) {
                Decl *d = s->decl;
                if (d->generics.len)
                    serr(e->span, "cannot use generic function `%s` as a value without type arguments", e->name);
                Vec no; memset(&no, 0, sizeof no);
                FnInst *fi = instantiate(d, &no, e->span);
                e->target = fi;
                Vec ps; memset(&ps, 0, sizeof ps);
                for (int j = 0; j < d->params.len; j++)
                    vec_push(&ps, resolve_type(VEC_AT(&d->params, Param, j)->type, d->mod, NULL));
                Type *rt = resolve_type(d->ret, d->mod, NULL);
                if (fi && !fi->ret) fi->ret = rt;
                r = ty_fn(ps, rt);
            } else {
                r = s->type ? s->type : ty_int;
            }
            break;
        }
        case E_UNARY: {
            Type *a = check_expr(e->a, want);
            if (e->op == T_NOT) {
                if (a->kind != TY_BOOL) serr(e->span, "`not` requires `Bool`, found `%s`", ty_str_of(a));
                r = ty_bool;
            } else if (e->op == T_MINUS) {
                if (a->kind == TY_FLOAT) { e->idx = OPC_FLOAT; r = ty_float; }
                else if (a->kind == TY_INT) { e->idx = OPC_INT; r = ty_int; }
                else { serr(e->span, "unary `-` requires `Int` or `Float`, found `%s`", ty_str_of(a)); r = a; }
            } else {
                if (a->kind != TY_INT && a->kind != TY_BYTE)
                    serr(e->span, "`~` requires `Int` or `Byte`, found `%s`", ty_str_of(a));
                r = a;
            }
            break;
        }
        case E_BINARY: r = check_binary(e); break;
        case E_LIST: {
            Type *elem = (want && want->kind == TY_LIST) ? want->elem : NULL;
            for (int i = 0; i < e->list.len; i++) {
                Type *t = check_expr(VEC_AT(&e->list, Expr, i), elem);
                if (!elem) elem = t;
                else if (!assignable(t, elem)) {
                    Diag *d = serr(VEC_AT(&e->list, Expr, i)->span,
                                   "list elements must all have the same type");
                    diag_note(d, NOSPAN, "expected `%s` (from earlier elements), found `%s`",
                              ty_str_of(elem), ty_str_of(t));
                }
            }
            if (!elem) elem = ty_any;
            if (elem->kind == TY_ANY && !(want && want->kind == TY_LIST)) {
                serr(e->span, "cannot infer the element type of this empty list");
                diag_add(DIAG_NOTE, NOSPAN, "help: annotate it, e.g. `let xs: [Int] = []`");
            }
            r = ty_list(elem);
            break;
        }
        case E_MAP: {
            Type *kt = (want && want->kind == TY_MAP) ? want->elem : NULL;
            Type *vt = (want && want->kind == TY_MAP) ? want->val : NULL;
            for (int i = 0; i + 1 < e->list.len; i += 2) {
                Type *k = check_expr(VEC_AT(&e->list, Expr, i), kt);
                Type *v = check_expr(VEC_AT(&e->list, Expr, i + 1), vt);
                if (!kt) kt = k; else if (!assignable(k, kt))
                    serr(VEC_AT(&e->list, Expr, i)->span, "map keys must all have type `%s`, found `%s`",
                         ty_str_of(kt), ty_str_of(k));
                if (!vt) vt = v; else if (!assignable(v, vt))
                    serr(VEC_AT(&e->list, Expr, i + 1)->span, "map values must all have type `%s`, found `%s`",
                         ty_str_of(vt), ty_str_of(v));
            }
            if (!kt) kt = ty_any;
            if (!vt) vt = ty_any;
            if (kt->kind == TY_ANY && !(want && want->kind == TY_MAP)) {
                serr(e->span, "cannot infer the types of this empty map");
                diag_add(DIAG_NOTE, NOSPAN, "help: annotate it, e.g. `let m: {Str: Int} = {:}`");
            }
            r = ty_map(kt, vt);
            break;
        }
        case E_STRUCT: {
            Sym *s = scope_get(cur_mod->scope, e->name);
            Decl *d = s && s->kind == SYM_TYPE ? s->decl : NULL;
            if (!d) {
                Type *sub = subst_get(cur_fn->subst, e->name);
                if (sub && sub->kind == TY_STRUCT) d = sub->decl;
            }
            if (!d || d->kind != D_STRUCT) {
                serr(e->span, "`%s` is not a struct type", e->name);
                r = ty_int;
                break;
            }
            Vec targs; memset(&targs, 0, sizeof targs);
            Subst sub; memset(&sub, 0, sizeof sub);
            for (int i = 0; i < d->generics.len; i++)
                subst_put(&sub, (const char *)d->generics.data[i], NULL);
            for (int i = 0; i < e->targs.len; i++)
                if (i < d->generics.len)
                    subst_put(&sub, (const char *)d->generics.data[i],
                              resolve_type(VEC_AT(&e->targs, TypeExpr, i), cur_mod, cur_fn->subst));
            /* infer from field initialisers, or from `want` */
            if (d->generics.len && !e->targs.len && want &&
                (want->kind == TY_STRUCT) && want->decl == d) {
                for (int i = 0; i < d->generics.len && i < want->targs.len; i++)
                    subst_put(&sub, (const char *)d->generics.data[i], VEC_AT(&want->targs, Type, i));
            }
            if (d->generics.len) {
                for (int i = 0; i < e->list.len; i++) {
                    FieldInit *fi = VEC_AT(&e->list, FieldInit, i);
                    int idx = -1;
                    for (int j = 0; j < d->fields.len; j++)
                        if (VEC_AT(&d->fields, StructField, j)->name == fi->name) { idx = j; break; }
                    if (idx < 0) continue;
                    int solved = 1;
                    for (int j = 0; j < sub.types.len; j++) if (!sub.types.data[j]) solved = 0;
                    if (solved) break;
                    Type *at = check_expr(fi->value, NULL);
                    unify(VEC_AT(&d->fields, StructField, idx)->type, at, &sub, d->mod);
                }
                for (int i = 0; i < d->generics.len; i++)
                    if (!sub.types.data[i]) {
                        serr(e->span, "cannot infer type parameter `%s` of `%s`",
                             (const char *)d->generics.data[i], d->name);
                        sub.types.data[i] = ty_int;
                    }
                for (int i = 0; i < d->generics.len; i++) vec_push(&targs, sub.types.data[i]);
            }
            Type *st = struct_instance(d, &targs, e->span);
            /* check fields */
            int nf = d->fields.len;
            char *given = (char *)arena_alloc(&g_arena, (size_t)(nf > 0 ? nf : 1));
            memset(given, 0, (size_t)(nf > 0 ? nf : 1));
            for (int i = 0; i < e->list.len; i++) {
                FieldInit *fi = VEC_AT(&e->list, FieldInit, i);
                int idx = struct_field_index(st, fi->name);
                if (idx < 0) {
                    Diag *dg = serr(fi->span, "`%s` has no field `%s`", ty_str_of(st), fi->name);
                    for (int j = 0; j < nf; j++) {
                        StructField *f = VEC_AT(&d->fields, StructField, j);
                        if (edit_dist(fi->name, f->name) <= 2) {
                            diag_note(dg, NOSPAN, "did you mean `%s`?", f->name);
                            break;
                        }
                    }
                    check_expr(fi->value, NULL);
                    continue;
                }
                if (given[idx]) serr(fi->span, "field `%s` is initialised twice", fi->name);
                given[idx] = 1;
                Type *ft = struct_field_type(st, idx);
                fi->value->type = NULL;
                Type *at = check_expr(fi->value, ft);
                if (!assignable(at, ft)) {
                    char ctx[256];
                    snprintf(ctx, sizeof ctx, "in field `%s` of `%s`", fi->name, ty_str_of(st));
                    want_err(fi->value->span, ft, at, ctx);
                }
                fi->span.len = fi->span.len;
                e->idx = 0;
            }
            for (int j = 0; j < nf; j++)
                if (!given[j]) {
                    StructField *f = VEC_AT(&d->fields, StructField, j);
                    Diag *dg = serr(e->span, "missing field `%s` in `%s` literal", f->name, d->name);
                    diag_note(dg, f->span, "`%s` is declared here with type `%s`",
                              f->name, ty_str_of(struct_field_type(st, j)));
                }
            r = st;
            break;
        }
        case E_LAMBDA: r = check_lambda(e, want); break;
        case E_FIELD: {
            /* module member? */
            if (e->a->kind == E_IDENT) {
                Sym *s = scope_get(cur_fn->scope, e->a->name);
                if (s && s->kind == SYM_MOD) {
                    e->a->sym = s;
                    Vec no; memset(&no, 0, sizeof no);
                    r = module_member(e, s->mod, e->name, &no, NULL, e->span, 0);
                    break;
                }
                if (s && s->kind == SYM_TYPE && s->decl->kind == D_ENUM) {
                    /* payload-free variant */
                    Vec no; memset(&no, 0, sizeof no);
                    Type *et = struct_instance(s->decl, &no, e->span);
                    if (s->decl->generics.len) {
                        if (want && want->kind == TY_ENUM && want->decl == s->decl) et = want;
                        else { serr(e->span, "cannot infer type arguments for `%s`", s->decl->name); }
                    }
                    int v = enum_variant_index(et, e->name);
                    if (v < 0) {
                        Diag *d = serr(e->span, "`%s` has no variant `%s`", s->decl->name, e->name);
                        for (int i = 0; i < s->decl->variants.len; i++) {
                            EnumVariant *ev = VEC_AT(&s->decl->variants, EnumVariant, i);
                            if (edit_dist(e->name, ev->name) <= 2) {
                                diag_note(d, NOSPAN, "did you mean `%s.%s`?", s->decl->name, ev->name);
                                break;
                            }
                        }
                        r = et; break;
                    }
                    if (enum_payload_count(et, v) != 0) {
                        serr(e->span, "variant `%s.%s` needs %d argument%s",
                             s->decl->name, e->name, enum_payload_count(et, v),
                             enum_payload_count(et, v) == 1 ? "" : "s");
                    }
                    e->kind = E_STRUCT;   /* reuse: enum construction */
                    e->idx = v;
                    e->builtin = -1;      /* marks enum construction */
                    e->type = et;
                    return et;
                }
            }
            Type *a = check_expr(e->a, NULL);
            if (a->kind == TY_STRUCT) {
                int idx = struct_field_index(a, e->name);
                if (idx < 0) {
                    Diag *d = serr(e->span, "`%s` has no field `%s`", ty_str_of(a), e->name);
                    if (a->decl)
                        for (int j = 0; j < a->decl->fields.len; j++) {
                            StructField *f = VEC_AT(&a->decl->fields, StructField, j);
                            if (edit_dist(e->name, f->name) <= 2) {
                                diag_note(d, NOSPAN, "did you mean `.%s`?", f->name);
                                break;
                            }
                        }
                    Vec tno; memset(&tno, 0, sizeof tno);
                    if (find_method(a, e->name, &tno, e->span))
                        diag_note(d, NOSPAN, "`%s` is a method; call it with `.%s()`", e->name, e->name);
                    r = ty_int;
                    break;
                }
                if (a->decl && !a->decl->is_pub && a->decl->mod != cur_mod)
                    serr(e->span, "`%s` is private to module `%s`", a->decl->name, a->decl->mod->name);
                e->idx = idx;
                r = struct_field_type(a, idx);
            } else if (a->kind == TY_ERRTYPE) {
                if (e->name == intern("msg")) { e->idx = 0; r = ty_str; }
                else if (e->name == intern("code")) { e->idx = 1; r = ty_int; }
                else { serr(e->span, "`Error` has fields `msg` and `code`"); r = ty_int; }
            } else {
                Diag *d = serr(e->span, "`%s` has no field `%s`", ty_str_of(a), e->name);
                Vec tno; memset(&tno, 0, sizeof tno);
                if (find_method(a, e->name, &tno, e->span))
                    diag_note(d, NOSPAN, "`%s` is a method; call it with `.%s()`", e->name, e->name);
                r = ty_int;
            }
            break;
        }
        case E_METHOD: {
            /* module function call? */
            if (e->a->kind == E_IDENT) {
                Sym *s = scope_get(cur_fn->scope, e->a->name);
                if (s && s->kind == SYM_MOD) {
                    e->a->sym = s;
                    Vec targs; memset(&targs, 0, sizeof targs);
                    for (int i = 0; i < e->targs.len; i++)
                        vec_push(&targs, resolve_type(VEC_AT(&e->targs, TypeExpr, i), cur_mod, cur_fn->subst));
                    r = module_member(e, s->mod, e->name, &e->list, &targs, e->span, 1);
                    break;
                }
                if (s && s->kind == SYM_TYPE && s->decl->kind == D_ENUM) {
                    Decl *ed = s->decl;
                    Vec targs; memset(&targs, 0, sizeof targs);
                    Subst sub; memset(&sub, 0, sizeof sub);
                    for (int i = 0; i < ed->generics.len; i++)
                        subst_put(&sub, (const char *)ed->generics.data[i], NULL);
                    for (int i = 0; i < e->targs.len && i < ed->generics.len; i++)
                        subst_put(&sub, (const char *)ed->generics.data[i],
                                  resolve_type(VEC_AT(&e->targs, TypeExpr, i), cur_mod, cur_fn->subst));
                    if (ed->generics.len && want && want->kind == TY_ENUM && want->decl == ed)
                        for (int i = 0; i < ed->generics.len && i < want->targs.len; i++)
                            subst_put(&sub, (const char *)ed->generics.data[i], VEC_AT(&want->targs, Type, i));
                    /* find variant */
                    int v = -1;
                    for (int i = 0; i < ed->variants.len; i++)
                        if (VEC_AT(&ed->variants, EnumVariant, i)->name == e->name) { v = i; break; }
                    if (v < 0) {
                        /* maybe a static method on the enum type */
                        Diag *d = serr(e->span, "`%s` has no variant `%s`", ed->name, e->name);
                        for (int i = 0; i < ed->variants.len; i++) {
                            EnumVariant *ev = VEC_AT(&ed->variants, EnumVariant, i);
                            if (edit_dist(e->name, ev->name) <= 2) {
                                diag_note(d, NOSPAN, "did you mean `%s.%s`?", ed->name, ev->name);
                                break;
                            }
                        }
                        r = ty_int; break;
                    }
                    EnumVariant *ev = VEC_AT(&ed->variants, EnumVariant, v);
                    if (ed->generics.len) {
                        for (int i = 0; i < e->list.len && i < ev->types.len; i++) {
                            Type *at = check_expr(VEC_AT(&e->list, Expr, i), NULL);
                            unify(VEC_AT(&ev->types, TypeExpr, i), at, &sub, ed->mod);
                        }
                        for (int i = 0; i < ed->generics.len; i++)
                            if (!sub.types.data[i]) {
                                serr(e->span, "cannot infer type parameter `%s` of `%s`",
                                     (const char *)ed->generics.data[i], ed->name);
                                sub.types.data[i] = ty_int;
                            }
                        for (int i = 0; i < ed->generics.len; i++) vec_push(&targs, sub.types.data[i]);
                    }
                    Type *et = struct_instance(ed, &targs, e->span);
                    int np = enum_payload_count(et, v);
                    if (e->list.len != np)
                        serr(e->span, "variant `%s.%s` takes %d value%s, found %d",
                             ed->name, ev->name, np, np == 1 ? "" : "s", e->list.len);
                    for (int i = 0; i < e->list.len && i < np; i++) {
                        Expr *a = VEC_AT(&e->list, Expr, i);
                        a->type = NULL;
                        Type *pt = enum_payload_type(et, v, i);
                        Type *at = check_expr(a, pt);
                        if (!assignable(at, pt)) {
                            char ctx[256];
                            snprintf(ctx, sizeof ctx, "in `%s.%s`", ed->name, ev->name);
                            want_err(a->span, pt, at, ctx);
                        }
                    }
                    e->idx = v;
                    e->builtin = -1;
                    e->kind = E_STRUCT;      /* lowered as enum construction */
                    e->type = et;
                    return et;
                }
            }
            Type *recv = check_expr(e->a, NULL);
            Vec targs; memset(&targs, 0, sizeof targs);
            Decl *d = find_method(recv, e->name, &targs, e->span);
            if (!d) {
                Diag *dg = serr(e->span, "`%s` has no method `%s`", ty_str_of(recv), e->name);
                const char *k = typekey(recv);
                if (k) {
                    for (int i = 0; i < MTAB_N; i++)
                        for (MEnt *me = mtab[i]; me; me = me->next)
                            if (me->key == k && edit_dist(e->name, me->name) <= 2) {
                                diag_note(dg, NOSPAN, "did you mean `.%s()`?", me->name);
                                i = MTAB_N; break;
                            }
                }
                if (recv->kind == TY_OPT)
                    diag_note(dg, NOSPAN, "help: unwrap the optional first (`x ?? default` or `if let`)");
                if (recv->kind == TY_RES)
                    diag_note(dg, NOSPAN, "help: unwrap the result first (`x?` or `x ?? default`)");
                for (int i = 0; i < e->list.len; i++) check_expr(VEC_AT(&e->list, Expr, i), NULL);
                r = ty_int;
                break;
            }
            if (!d->is_pub && d->mod != cur_mod)
                serr(e->span, "method `%s.%s` is private to module `%s`",
                     ty_str_of(recv), e->name, d->mod->name);
            Vec etargs; memset(&etargs, 0, sizeof etargs);
            for (int i = 0; i < e->targs.len; i++)
                vec_push(&etargs, resolve_type(VEC_AT(&e->targs, TypeExpr, i), cur_mod, cur_fn->subst));
            r = check_static_call(e, d, e->targs.len ? &etargs : NULL, e->a, recv, &e->list, e->span);
            break;
        }
        case E_CALL: {
            Expr *cal = e->a;
            if (cal->kind == E_IDENT) {
                int bi = builtin_id(cal->name);
                Sym *s = lookup_capture(cur_fn, cal->name);
                if (!s && bi) { r = check_builtin(e, bi, &e->list, e->span); break; }
                if (s && s->kind == SYM_FN) {
                    Vec targs; memset(&targs, 0, sizeof targs);
                    for (int i = 0; i < cal->targs.len; i++)
                        vec_push(&targs, resolve_type(VEC_AT(&cal->targs, TypeExpr, i), cur_mod, cur_fn->subst));
                    s->used = 1;
                    r = check_static_call(e, s->decl, cal->targs.len ? &targs : NULL,
                                          NULL, NULL, &e->list, e->span);
                    break;
                }
                if (s && s->kind == SYM_TYPE) {
                    serr(e->span, "`%s` is a type; construct it with `%s{ ... }`", cal->name, cal->name);
                    r = ty_int; break;
                }
                if (!s) {
                    Diag *d = serr(cal->span, "cannot find function `%s` in this scope", cal->name);
                    const char *g = suggest_in_scope(cur_fn->scope, cal->name);
                    if (g) diag_note(d, NOSPAN, "did you mean `%s`?", g);
                    for (int i = 0; i < e->list.len; i++) check_expr(VEC_AT(&e->list, Expr, i), NULL);
                    r = ty_int; break;
                }
            }
            /* indirect call through a function value */
            Type *ft = check_expr(cal, NULL);
            if (ft->kind != TY_FN) {
                Diag *d = serr(e->span, "`%s` is not callable", ty_str_of(ft));
                diag_note(d, NOSPAN, "only functions and closures can be called");
                for (int i = 0; i < e->list.len; i++) check_expr(VEC_AT(&e->list, Expr, i), NULL);
                r = ty_int; break;
            }
            if (e->list.len != ft->params.len)
                serr(e->span, "this closure expects %d argument%s, found %d",
                     ft->params.len, ft->params.len == 1 ? "" : "s", e->list.len);
            for (int i = 0; i < e->list.len; i++) {
                Type *pt = i < ft->params.len ? VEC_AT(&ft->params, Type, i) : NULL;
                Expr *a = VEC_AT(&e->list, Expr, i);
                Type *at = check_expr(a, pt);
                if (pt && !assignable(at, pt)) want_err(a->span, pt, at, "in closure argument");
            }
            e->is_static = 0;
            r = ft->ret;
            break;
        }
        case E_INDEX: {
            Type *a = check_expr(e->a, NULL);
            if (a->kind == TY_LIST) {
                Type *i = check_expr(e->b, ty_int);
                if (i->kind != TY_INT) serr(e->b->span, "list index must be `Int`, found `%s`", ty_str_of(i));
                e->idx = IDX_LIST;
                r = a->elem;
            } else if (a->kind == TY_MAP) {
                Type *k = check_expr(e->b, a->elem);
                if (!assignable(k, a->elem))
                    want_err(e->b->span, a->elem, k, "in map key");
                e->idx = IDX_MAP;
                r = ty_opt(a->val);
            } else if (a->kind == TY_STR) {
                Type *i = check_expr(e->b, ty_int);
                if (i->kind != TY_INT) serr(e->b->span, "string index must be `Int`, found `%s`", ty_str_of(i));
                e->idx = IDX_STR;
                r = ty_byte;
            } else {
                Diag *d = serr(e->span, "cannot index `%s`", ty_str_of(a));
                diag_note(d, NOSPAN, "indexing works on lists, maps and strings");
                check_expr(e->b, NULL);
                r = ty_int;
            }
            break;
        }
        case E_SLICE: {
            Type *a = check_expr(e->a, NULL);
            if (e->b) { Type *t = check_expr(e->b, ty_int);
                        if (t->kind != TY_INT) serr(e->b->span, "slice bounds must be `Int`"); }
            if (e->c) { Type *t = check_expr(e->c, ty_int);
                        if (t->kind != TY_INT) serr(e->c->span, "slice bounds must be `Int`"); }
            if (a->kind == TY_LIST) { e->idx = IDX_LIST; r = a; }
            else if (a->kind == TY_STR) { e->idx = IDX_STR; r = ty_str; }
            else { serr(e->span, "cannot slice `%s`", ty_str_of(a)); r = a; }
            break;
        }
        case E_TRY: {
            Type *a = check_expr(e->a, NULL);
            if (a->kind == TY_RES) {
                if (!cur_fn->ret || cur_fn->ret->kind != TY_RES) {
                    Diag *d = serr(e->span, "`?` can only be used in a function that returns `!T`");
                    diag_note(d, NOSPAN, "this function returns `%s`",
                              cur_fn->ret ? ty_str_of(cur_fn->ret) : "Void");
                    diag_note(d, NOSPAN, "help: change the return type to `!%s`, or use `?? default`",
                              cur_fn->ret ? ty_str_of(cur_fn->ret) : "Void");
                }
                e->idx = 1;
                r = a->elem;
            } else if (a->kind == TY_OPT) {
                if (!cur_fn->ret || (cur_fn->ret->kind != TY_OPT && cur_fn->ret->kind != TY_RES)) {
                    Diag *d = serr(e->span, "`?` on an optional requires a function returning `?T` or `!T`");
                    diag_note(d, NOSPAN, "this function returns `%s`",
                              cur_fn->ret ? ty_str_of(cur_fn->ret) : "Void");
                    diag_note(d, NOSPAN, "help: use `x ?? default` to supply a fallback");
                }
                e->idx = cur_fn->ret && cur_fn->ret->kind == TY_RES ? 2 : 0;
                r = a->elem ? a->elem : ty_int;
            } else {
                Diag *d = serr(e->span, "`?` requires `?T` or `!T`, found `%s`", ty_str_of(a));
                diag_note(d, NOSPAN, "`?` propagates a missing value or an error to the caller");
                r = a;
            }
            break;
        }
        case E_ORELSE: {
            Type *a = check_expr(e->a, NULL);
            if (a->kind == TY_OPT) {
                Type *inner = a->elem ? a->elem : (want ? want : ty_int);
                Type *b = check_expr(e->b, inner);
                if (e->b->kind != E_STMTEXPR && !assignable(b, inner))
                    want_err(e->b->span, inner, b, "on the right of `??`");
                e->idx = 0;
                r = inner;
            } else if (a->kind == TY_RES) {
                Type *inner = a->elem;
                Type *b = check_expr(e->b, inner);
                if (e->b->kind != E_STMTEXPR && !assignable(b, inner))
                    want_err(e->b->span, inner, b, "on the right of `??`");
                e->idx = 1;
                r = inner;
            } else {
                Diag *d = serr(e->span, "`??` requires `?T` or `!T` on the left, found `%s`", ty_str_of(a));
                diag_note(d, NOSPAN, "`??` supplies a fallback for a missing value or an error");
                check_expr(e->b, a);
                r = a;
            }
            break;
        }
        case E_CAST: {
            Type *a = check_expr(e->a, NULL);
            Type *t = resolve_type(e->texpr, cur_mod, cur_fn->subst);
            if ((a->kind == TY_INT && t->kind == TY_BYTE) ||
                (a->kind == TY_BYTE && t->kind == TY_INT)) { e->idx = 0; }
            else if (ty_eq(a, t)) { e->idx = 0; }
            else {
                Diag *d = serr(e->span, "cannot cast `%s` to `%s`", ty_str_of(a), ty_str_of(t));
                diag_note(d, NOSPAN, "help: `as` only converts between `Int` and `Byte`; use `int()`, `float()` or `str()` otherwise");
            }
            r = t;
            break;
        }
        case E_MATCH: {
            Type *subj = check_expr(e->a, NULL);
            Type *res = want;
            int is_expr = 1;
            int stmt_pos = e->is_static;
            /* A block arm yields the value of its final expression, so
               `=> { let t = ...; t * 2 }` works like `=> expr`. */
            if (!stmt_pos) {
                for (int i = 0; i < e->list.len; i++) {
                    MatchArm *a = VEC_AT(&e->list, MatchArm, i);
                    if (a->body || !a->block || !a->block->list.len) continue;
                    Stmt *last = VEC_AT(&a->block->list, Stmt, a->block->list.len - 1);
                    if (last->kind == S_EXPR) {
                        a->body = last->a;
                        a->block->list.len--;
                    }
                }
            }
            for (int i = 0; i < e->list.len; i++) {
                MatchArm *a = VEC_AT(&e->list, MatchArm, i);
                Scope *sc = scope_new(cur_fn->scope);
                Scope *save = cur_fn->scope;
                cur_fn->scope = sc;
                check_pattern(a->pat, subj);
                if (a->guard) {
                    Type *g = check_expr(a->guard, ty_bool);
                    if (g->kind != TY_BOOL) serr(a->guard->span, "match guard must be `Bool`, found `%s`", ty_str_of(g));
                }
                if (a->body) {
                    if (a->block) check_stmts(a->block, sc);
                    Type *bt = check_expr(a->body, res);
                    if (!res) res = bt;
                    else if (!assignable(bt, res) && bt->kind != TY_VOID) {
                        Diag *d = serr(a->body->span, "match arms have incompatible types");
                        diag_note(d, NOSPAN, "expected `%s`, found `%s`", ty_str_of(res), ty_str_of(bt));
                    }
                    warn_unused(sc);
                } else {
                    check_stmts(a->block, sc);
                    is_expr = 0;
                }
                warn_unused(sc);
                cur_fn->scope = save;
            }
            if (e->list.len == 0) {
                serr(e->span, "`match` must have at least one arm");
                r = ty_void; break;
            }
            e->idx = is_expr;
            if (is_expr && want && want->kind != TY_VOID) {
                if (!patterns_exhaustive(&e->list, subj)) {
                    Diag *d = serr(e->span, "`match` used as an expression must cover every case");
                    if (subj && subj->kind == TY_ENUM && subj->decl) {
                        for (int i = 0; i < subj->decl->variants.len; i++) {
                            int found = 0;
                            for (int j = 0; j < e->list.len; j++) {
                                MatchArm *a = VEC_AT(&e->list, MatchArm, j);
                                if (a->guard) continue;
                                if (a->pat->kind == P_ENUM && a->pat->tag == i) found = 1;
                                if (a->pat->kind == P_OR)
                                    for (int q = 0; q < a->pat->subs.len; q++)
                                        if (VEC_AT(&a->pat->subs, Pattern, q)->tag == i) found = 1;
                            }
                            if (!found)
                                diag_note(d, NOSPAN, "missing: `%s.%s`", subj->decl->name,
                                          VEC_AT(&subj->decl->variants, EnumVariant, i)->name);
                        }
                    }
                    diag_note(d, NOSPAN, "help: add a `_ => ...` arm");
                }
            }
            r = res ? res : ty_void;
            break;
        }
        case E_INTRINSIC: {
            const char *n = e->name;
            for (int i = 0; i < e->list.len; i++) check_expr(VEC_AT(&e->list, Expr, i), ty_int);
            if (n == intern("syscall")) r = ty_int;
            else if (n == intern("load8") || n == intern("load16") ||
                     n == intern("load32") || n == intern("load64")) r = ty_int;
            else if (n == intern("store8") || n == intern("store16") ||
                     n == intern("store32") || n == intern("store64")) r = ty_void;
            else if (n == intern("addr")) {
                if (e->list.len == 1) { VEC_AT(&e->list, Expr, 0)->type = NULL;
                                        check_expr(VEC_AT(&e->list, Expr, 0), NULL); }
                r = ty_int;
            }
            else if (n == intern("ref")) {
                if (e->targs.len != 1) { serr(e->span, "`@ref` needs one type argument"); r = ty_int; }
                else r = resolve_type(VEC_AT(&e->targs, TypeExpr, 0), cur_mod, cur_fn->subst);
            }
            else if (n == intern("sizeof")) r = ty_int;
            else if (n == intern("stack_top")) r = ty_int;
            else if (n == intern("rt_base")) r = ty_int;
            else if (n == intern("argc") || n == intern("argv") || n == intern("envp")) r = ty_int;
            else if (n == intern("save_regs")) r = ty_int;
            else if (n == intern("restore_regs")) r = ty_void;
            else if (n == intern("f2bits")) {
                if (e->list.len == 1) { VEC_AT(&e->list, Expr, 0)->type = NULL;
                                        check_expr(VEC_AT(&e->list, Expr, 0), ty_float); }
                r = ty_int;
            }
            else if (n == intern("bits2f")) r = ty_float;
            else if (n == intern("fsqrt")) {
                if (e->list.len == 1) { VEC_AT(&e->list, Expr, 0)->type = NULL;
                                        check_expr(VEC_AT(&e->list, Expr, 0), ty_float); }
                r = ty_float;
            }
            else if (n == intern("trap")) r = ty_void;
            else { serr(e->span, "unknown intrinsic `@%s`", n); r = ty_int; }
            break;
        }
        case E_STMTEXPR:
            check_stmt(e->stmt);
            r = want ? want : ty_void;
            break;
        default:
            r = ty_int;
            break;
    }
    e->type = r;
    return r;
}

/* ---- builtins ---- */

static Type *check_builtin(Expr *e, int bi, Vec *args, Span sp) {
    e->builtin = bi;
    int want_args = 1;
    switch (bi) {
        case BI_PRINT: case BI_PRINTLN: want_args = -1; break;
        case BI_ASSERT_EQ: case BI_ASSERT_NE: want_args = 2; break;
        case BI_ERR_CODE: want_args = 2; break;
        case BI_OK: want_args = -2; break;
        case BI_ASSERT: want_args = -2; break;
        default: want_args = 1;
    }
    if (want_args >= 0 && args->len != want_args) {
        serr(sp, "`%s` takes %d argument%s, found %d",
             builtins[bi - 1].n, want_args, want_args == 1 ? "" : "s", args->len);
    }
    if (want_args == -2 && args->len > 2)
        serr(sp, "`%s` takes at most 2 arguments, found %d", builtins[bi - 1].n, args->len);

    switch (bi) {
        case BI_STR: {
            if (!args->len) return ty_str;
            Expr *a = VEC_AT(args, Expr, 0);
            Type *t = check_expr(a, NULL);
            if (t->kind != TY_STR) {
                FnInst *ts = find_to_str(t);
                if (ts) e->extra = ts;
            }
            return ty_str;
        }
        case BI_INT: {
            if (!args->len) return ty_int;
            Type *t = check_expr(VEC_AT(args, Expr, 0), NULL);
            if (t->kind == TY_STR) return ty_opt(ty_int);
            if (t->kind == TY_FLOAT || t->kind == TY_BYTE || t->kind == TY_BOOL || t->kind == TY_INT)
                return ty_int;
            serr(sp, "cannot convert `%s` to `Int`", ty_str_of(t));
            return ty_int;
        }
        case BI_FLOAT: {
            if (!args->len) return ty_float;
            Type *t = check_expr(VEC_AT(args, Expr, 0), NULL);
            if (t->kind == TY_STR) return ty_opt(ty_float);
            if (t->kind == TY_INT || t->kind == TY_FLOAT || t->kind == TY_BYTE) return ty_float;
            serr(sp, "cannot convert `%s` to `Float`", ty_str_of(t));
            return ty_float;
        }
        case BI_BYTE: {
            if (!args->len) return ty_byte;
            Type *t = check_expr(VEC_AT(args, Expr, 0), NULL);
            if (t->kind != TY_INT && t->kind != TY_BYTE)
                serr(sp, "cannot convert `%s` to `Byte`", ty_str_of(t));
            return ty_byte;
        }
        case BI_BOOL: {
            if (!args->len) return ty_bool;
            Type *t = check_expr(VEC_AT(args, Expr, 0), NULL);
            if (t->kind != TY_INT && t->kind != TY_BYTE && t->kind != TY_BOOL)
                serr(sp, "cannot convert `%s` to `Bool`", ty_str_of(t));
            return ty_bool;
        }
        case BI_LEN: {
            if (!args->len) return ty_int;
            Type *t = check_expr(VEC_AT(args, Expr, 0), NULL);
            if (t->kind != TY_STR && t->kind != TY_LIST && t->kind != TY_MAP) {
                Diag *d = serr(sp, "`len` requires `Str`, a list or a map, found `%s`", ty_str_of(t));
                diag_note(d, NOSPAN, "help: `len(x)` counts bytes in a `Str` and elements in a list or map");
            }
            e->idx = t->kind == TY_STR ? IDX_STR : (t->kind == TY_MAP ? IDX_MAP : IDX_LIST);
            return ty_int;
        }
        case BI_PRINT: case BI_PRINTLN: {
            for (int i = 0; i < args->len; i++) {
                Expr *a = VEC_AT(args, Expr, i);
                Type *t = check_expr(a, NULL);
                if (t->kind != TY_STR) {
                    FnInst *ts = find_to_str(t);
                    if (ts) a->extra = ts;
                }
            }
            return ty_void;
        }
        case BI_PANIC: {
            if (args->len) {
                Type *t = check_expr(VEC_AT(args, Expr, 0), ty_str);
                if (t->kind != TY_STR) want_err(VEC_AT(args, Expr, 0)->span, ty_str, t, "in `panic`");
            }
            return ty_void;
        }
        case BI_ASSERT: {
            if (args->len) {
                Type *t = check_expr(VEC_AT(args, Expr, 0), ty_bool);
                if (t->kind != TY_BOOL) want_err(VEC_AT(args, Expr, 0)->span, ty_bool, t, "in `assert`");
            }
            if (args->len > 1) {
                Type *t = check_expr(VEC_AT(args, Expr, 1), ty_str);
                if (t->kind != TY_STR) want_err(VEC_AT(args, Expr, 1)->span, ty_str, t, "in `assert` message");
            }
            return ty_void;
        }
        case BI_ASSERT_EQ: case BI_ASSERT_NE: {
            if (args->len >= 2) {
                Type *a = check_expr(VEC_AT(args, Expr, 0), NULL);
                Type *b = check_expr(VEC_AT(args, Expr, 1), a);
                if (!assignable(b, a))
                    serr(sp, "`%s` needs two values of the same type, found `%s` and `%s`",
                         bi == BI_ASSERT_EQ ? "assert_eq" : "assert_ne", ty_str_of(a), ty_str_of(b));
                e->extra = (void *)a;
                for (int i = 0; i < 2; i++) {
                    Expr *x = VEC_AT(args, Expr, i);
                    FnInst *ts = find_to_str(x->type);
                    if (ts && !x->extra) x->extra = ts;
                }
            }
            return ty_void;
        }
        case BI_OK: {
            Type *inner = NULL;
            if (args->len) inner = check_expr(VEC_AT(args, Expr, 0), NULL);
            else inner = ty_void;
            return ty_res(inner);
        }
        case BI_ERR: {
            if (args->len) {
                Type *t = check_expr(VEC_AT(args, Expr, 0), ty_str);
                if (t->kind != TY_STR) want_err(VEC_AT(args, Expr, 0)->span, ty_str, t, "in `err`");
            }
            return ty_res(NULL);
        }
        case BI_ERR_CODE: {
            if (args->len >= 2) {
                Type *t = check_expr(VEC_AT(args, Expr, 0), ty_str);
                if (t->kind != TY_STR) want_err(VEC_AT(args, Expr, 0)->span, ty_str, t, "in `err_code`");
                Type *c = check_expr(VEC_AT(args, Expr, 1), ty_int);
                if (c->kind != TY_INT) want_err(VEC_AT(args, Expr, 1)->span, ty_int, c, "in `err_code`");
            }
            return ty_res(NULL);
        }
    }
    return ty_void;
}

/* ------------------------------------------------------------------ */
/* statements                                                           */
/* ------------------------------------------------------------------ */

static void warn_unused(Scope *cs) {
    for (int i = 0; i < cs->nbuckets; i++)
        for (Sym *sym = cs->buckets[i]; sym; sym = sym->next)
            if (sym->kind == SYM_LOCAL && !sym->used && sym->name[0] != '_')
                swarn(sym->span, "unused variable `%s` (prefix with `_` to silence)", sym->name);
}

static void check_stmts(Stmt *s, Scope *sc) {
    if (!s) return;
    Scope *save = cur_fn->scope;
    cur_fn->scope = sc ? sc : scope_new(save);
    for (int i = 0; i < s->list.len; i++) check_stmt(VEC_AT(&s->list, Stmt, i));
    cur_fn->scope = save;
}

static void check_block(Stmt *s, Scope *sc) {
    if (!s) return;
    Scope *save = cur_fn->scope;
    cur_fn->scope = sc ? sc : scope_new(save);
    for (int i = 0; i < s->list.len; i++) check_stmt(VEC_AT(&s->list, Stmt, i));
    warn_unused(cur_fn->scope);
    cur_fn->scope = save;
}

static void check_stmt(Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case S_LET: {
            Type *want = s->texpr ? resolve_type(s->texpr, cur_mod, cur_fn->subst) : NULL;
            Type *t = check_expr(s->a, want);
            if (want) {
                if (!assignable(t, want)) want_err(s->a->span, want, t, "in `let`");
                t = want;
            }
            if (t->kind == TY_VOID) {
                Diag *d = serr(s->span, "cannot bind a `Void` value to `%s`", s->name);
                diag_note(d, NOSPAN, "this expression does not produce a value");
            }
            if (t->kind == TY_OPT && !t->elem) {
                Diag *d = serr(s->span, "cannot infer the type of `%s` from `nil`", s->name);
                diag_note(d, NOSPAN, "help: annotate it, e.g. `let %s: ?Int = nil`", s->name);
                t = ty_opt(ty_int);
            }
            Sym *prev = scope_get_local(cur_fn->scope, s->name);
            if (prev && prev->kind == SYM_LOCAL)
                swarn(s->span, "`%s` shadows an earlier binding in the same block", s->name);
            Sym *sym = declare_local(s->name, t, s->is_mut, s->span);
            s->sym = sym;
            s->type = t;
            break;
        }
        case S_ASSIGN: {
            Type *lt = check_expr(s->a, NULL);
            if (s->a->kind == E_IDENT) {
                Sym *sym = (Sym *)s->a->sym;
                if (sym && !sym->is_mut && sym->kind != SYM_CAPTURE) {
                    Diag *d = serr(s->span, "cannot assign to immutable `%s`", s->a->name);
                    diag_note(d, sym->span, "declare it as `let mut %s = ...`", s->a->name);
                } else if (sym && sym->kind == SYM_CAPTURE && !sym->is_mut) {
                    serr(s->span, "cannot assign to captured immutable `%s`", s->a->name);
                }
            }
            if (s->a->kind == E_INDEX && s->a->idx == IDX_STR)
                serr(s->span, "strings are immutable; build a new one instead");
            if (s->op != T_EQ) {
                /* compound assignment: type must support the operator */
                Expr fake;
                memset(&fake, 0, sizeof fake);
                fake.kind = E_BINARY;
                fake.span = s->span;
                fake.op = (s->op == T_PLUSEQ) ? T_PLUS : (s->op == T_MINUSEQ) ? T_MINUS :
                          (s->op == T_STAREQ) ? T_STAR : (s->op == T_SLASHEQ) ? T_SLASH : T_PERCENT;
                fake.a = s->a;
                fake.b = s->b;
                Type *save_t = s->a->type;
                Type *rt = check_binary(&fake);
                s->a->type = save_t;
                s->op = fake.op;
                s->type = rt;
                s->b->type = s->b->type;
                s->name2 = NULL;
                s->sym2 = (void *)(intptr_t)fake.idx;
                if (!assignable(rt, lt)) want_err(s->span, lt, rt, "in compound assignment");
                break;
            }
            Type *rt = check_expr(s->b, lt);
            if (!assignable(rt, lt)) want_err(s->b->span, lt, rt, "in assignment");
            s->type = lt;
            break;
        }
        case S_EXPR: {
            if (s->a && s->a->kind == E_MATCH) s->a->is_static = 1;
            Type *t = check_expr(s->a, NULL);
            if (s->a->kind == E_BINARY || s->a->kind == E_IDENT || s->a->kind == E_FIELD ||
                s->a->kind == E_INT || s->a->kind == E_STR)
                swarn(s->span, "this expression has no effect");
            (void)t;
            break;
        }
        case S_RETURN: {
            Type *want = cur_fn->ret;
            if (s->a) {
                Type *t = check_expr(s->a, want);
                if (!want) { cur_fn->ret = t; }
                else if (!assignable(t, want)) {
                    if (want->kind == TY_VOID)
                        serr(s->span, "this function returns nothing, but `return` has a value");
                    else want_err(s->a->span, want, t, "in `return`");
                }
            } else {
                if (want && want->kind != TY_VOID) {
                    Diag *d = serr(s->span, "`return` needs a value of type `%s`", ty_str_of(want));
                    diag_note(d, NOSPAN, "this function is declared to return `%s`", ty_str_of(want));
                }
            }
            break;
        }
        case S_IF: {
            Type *c = check_expr(s->a, ty_bool);
            if (c->kind != TY_BOOL) {
                Diag *d = serr(s->a->span, "`if` condition must be `Bool`, found `%s`", ty_str_of(c));
                if (c->kind == TY_OPT)
                    diag_note(d, NOSPAN, "help: use `if let v = x { ... }` to test an optional");
                else if (c->kind == TY_INT)
                    diag_note(d, NOSPAN, "help: compare it, e.g. `x != 0`");
            }
            check_block(s->then_s, NULL);
            if (s->else_s) {
                if (s->else_s->kind == S_BLOCK) check_block(s->else_s, NULL);
                else check_stmt(s->else_s);
            }
            break;
        }
        case S_IFLET: {
            Type *c = check_expr(s->a, NULL);
            Type *inner;
            if (c->kind == TY_OPT) { inner = c->elem ? c->elem : ty_int; s->op = 0; }
            else if (c->kind == TY_RES) { inner = c->elem; s->op = 1; }
            else {
                Diag *d = serr(s->a->span, "`if let` requires `?T` or `!T`, found `%s`", ty_str_of(c));
                diag_note(d, NOSPAN, "help: plain values are always present; use `if cond { ... }`");
                inner = c;
            }
            Scope *sc = scope_new(cur_fn->scope);
            Scope *save = cur_fn->scope;
            cur_fn->scope = sc;
            Sym *sym = declare_local(s->name, inner, 0, s->span);
            sym->used = 1;
            s->sym = sym;
            check_block(s->then_s, sc);
            cur_fn->scope = save;
            if (s->else_s) {
                if (s->else_s->kind == S_BLOCK) check_block(s->else_s, NULL);
                else check_stmt(s->else_s);
            }
            break;
        }
        case S_WHILE: {
            Type *c = check_expr(s->a, ty_bool);
            if (c->kind != TY_BOOL)
                serr(s->a->span, "`while` condition must be `Bool`, found `%s`", ty_str_of(c));
            cur_fn->loop_depth++;
            check_block(s->then_s, NULL);
            cur_fn->loop_depth--;
            break;
        }
        case S_FOR: {
            Type *it = check_expr(s->a, NULL);
            Type *vt = NULL, *kt = NULL;
            if (it->kind == TY_RANGE) { kt = ty_int; s->op = 0; }
            else if (it->kind == TY_LIST) {
                if (s->name2) { kt = ty_int; vt = it->elem; s->op = 4; }
                else { kt = it->elem; s->op = 1; }
            }
            else if (it->kind == TY_MAP) { kt = it->elem; vt = it->val; s->op = 2; }
            else if (it->kind == TY_STR) {
                if (s->name2) { kt = ty_int; vt = ty_byte; s->op = 5; }
                else { kt = ty_byte; s->op = 3; }
            }
            else {
                Diag *d = serr(s->a->span, "cannot iterate over `%s`", ty_str_of(it));
                diag_note(d, NOSPAN, "`for` works on ranges (`0..n`), lists, maps and strings");
                kt = ty_int;
            }
            if (s->name2 && !vt && it->kind == TY_RANGE)
                serr(s->span, "`for i, x` needs a list, map or string; a range yields one value");
            Scope *sc = scope_new(cur_fn->scope);
            Scope *save = cur_fn->scope;
            cur_fn->scope = sc;
            Sym *a = declare_local(s->name, kt, 0, s->span);
            a->used = 1;
            s->sym = a;
            if (s->name2) {
                Sym *b = declare_local(s->name2, vt ? vt : ty_int, 0, s->span);
                b->used = 1;
                s->sym2 = b;
            }
            cur_fn->loop_depth++;
            check_block(s->then_s, sc);
            cur_fn->loop_depth--;
            cur_fn->scope = save;
            break;
        }
        case S_BLOCK: check_block(s, NULL); break;
        case S_BREAK: case S_CONTINUE:
            if (cur_fn->loop_depth == 0)
                serr(s->span, "`%s` outside of a loop", s->kind == S_BREAK ? "break" : "continue");
            break;
        case S_MATCH: break;
    }
}

/* ------------------------------------------------------------------ */
/* function bodies                                                      */
/* ------------------------------------------------------------------ */

static int always_returns(Stmt *s);
static int patterns_exhaustive(Vec *arms, Type *subject);
static int block_returns(Stmt *b);

static int block_returns(Stmt *b) {
    if (!b) return 0;
    for (int i = 0; i < b->list.len; i++)
        if (always_returns(VEC_AT(&b->list, Stmt, i))) return 1;
    return 0;
}

static int always_returns(Stmt *s) {
    if (!s) return 0;
    switch (s->kind) {
        case S_RETURN: return 1;
        case S_BLOCK: return block_returns(s);
        case S_IF: case S_IFLET:
            return s->else_s && block_returns(s->then_s) &&
                   (s->else_s->kind == S_BLOCK ? block_returns(s->else_s) : always_returns(s->else_s));
        case S_EXPR:
            /* `panic(...)` diverges */
            if (s->a && s->a->kind == E_CALL && s->a->builtin == BI_PANIC) return 1;
            /* an exhaustive `match` whose every arm diverges also diverges */
            if (s->a && s->a->kind == E_MATCH && s->a->list.len) {
                if (!patterns_exhaustive(&s->a->list, s->a->a ? s->a->a->type : NULL)) return 0;
                for (int i = 0; i < s->a->list.len; i++) {
                    MatchArm *a = VEC_AT(&s->a->list, MatchArm, i);
                    int div = 0;
                    if (a->body && a->body->kind == E_STMTEXPR && a->body->stmt &&
                        a->body->stmt->kind == S_RETURN) div = 1;
                    if (a->body && a->body->kind == E_CALL && a->body->builtin == BI_PANIC) div = 1;
                    if (a->block && block_returns(a->block)) div = 1;
                    if (a->block && a->body && a->body->kind == E_STMTEXPR &&
                        a->body->stmt && a->body->stmt->kind == S_RETURN) div = 1;
                    if (!div) return 0;
                }
                return 1;
            }
            return 0;
        case S_WHILE:
            return s->a && s->a->kind == E_BOOL && s->a->ival == 1 && !block_returns(s->then_s);
        default: return 0;
    }
}

static void check_fn_body(FnInst *fi) {
    Decl *d = fi->decl;
    if (!d || !d->body) return;
    Module *save_mod = cur_mod;
    cur_mod = d->mod;

    Subst *sub = NEW(Subst);
    memset(sub, 0, sizeof(Subst));
    for (int i = 0; i < d->generics.len && i < fi->targs.len; i++)
        subst_put(sub, (const char *)d->generics.data[i], VEC_AT(&fi->targs, Type, i));

    FnCtx fc;
    memset(&fc, 0, sizeof fc);
    fc.inst = fi;
    fc.mod = d->mod;
    fc.subst = sub;
    fc.scope = scope_new(d->mod->scope);
    fc.ret = resolve_type(d->ret, d->mod, sub);
    fc.nslots = 1;    /* slot 0 is the environment pointer */
    fi->ret = fc.ret;

    FnCtx *savefn = cur_fn;
    cur_fn = &fc;

    if (d->has_self) {
        Type *recv = NULL;
        /* rebuild the receiver type from the declaration + type args */
        const char *rn = d->recv;
        if (rn == intern("List")) recv = ty_list(fi->targs.len ? VEC_AT(&fi->targs, Type, 0) : ty_int);
        else if (rn == intern("Map")) recv = ty_map(fi->targs.len ? VEC_AT(&fi->targs, Type, 0) : ty_int,
                                                    fi->targs.len > 1 ? VEC_AT(&fi->targs, Type, 1) : ty_int);
        else if (rn == intern("Option")) recv = ty_opt(fi->targs.len ? VEC_AT(&fi->targs, Type, 0) : ty_int);
        else if (rn == intern("Result")) recv = ty_res(fi->targs.len ? VEC_AT(&fi->targs, Type, 0) : ty_int);
        else {
            Type *b = builtin_named(rn);
            if (b) recv = b;
            else {
                Decl *td = find_type_decl(d->mod, rn);
                if (td) {
                    Vec ta; memset(&ta, 0, sizeof ta);
                    for (int i = 0; i < fi->targs.len; i++) vec_push(&ta, fi->targs.data[i]);
                    recv = struct_instance(td, &ta, d->span);
                } else recv = ty_int;
            }
        }
        Sym *s = NEW(Sym);
        s->kind = SYM_PARAM;
        s->name = intern("self");
        s->type = recv;
        s->is_mut = 1;
        s->slot = fc.nslots++;
        s->span = d->span;
        s->used = 1;
        scope_put(fc.scope, s);
        vec_push(&fi->param_types, recv);
        vec_push(&fi->param_syms, s);
    }
    for (int i = 0; i < d->params.len; i++) {
        Param *p = VEC_AT(&d->params, Param, i);
        Type *t = resolve_type(p->type, d->mod, sub);
        Sym *s = NEW(Sym);
        s->kind = SYM_PARAM;
        s->name = p->name;
        s->type = t;
        s->slot = fc.nslots++;
        s->span = p->span;
        if (p->name[0] == '_') s->used = 1;
        scope_put(fc.scope, s);
        vec_push(&fi->param_types, t);
        vec_push(&fi->param_syms, s);
    }
    fi->nparams = fi->param_types.len;

    check_block(d->body, fc.scope);

    if (fc.ret && fc.ret->kind != TY_VOID && !block_returns(d->body)) {
        Diag *dg = serr(d->span, "`%s` must return a value of type `%s` on every path",
                        d->name, ty_str_of(fc.ret));
        diag_note(dg, NOSPAN, "help: add a `return` at the end of the function");
    }
    /* unused parameters */
    for (int i = 0; i < fc.scope->nbuckets; i++)
        for (Sym *sym = fc.scope->buckets[i]; sym; sym = sym->next)
            if (sym->kind == SYM_PARAM && !sym->used && sym->name[0] != '_' && sym->name != intern("self"))
                swarn(sym->span, "unused parameter `%s` (prefix with `_` to silence)", sym->name);

    fi->nslots = fc.nslots;
    cur_fn = savefn;
    cur_mod = save_mod;
}

/* ------------------------------------------------------------------ */
/* collection + driver                                                  */
/* ------------------------------------------------------------------ */

static void collect_module(Module *m) {
    if (m->scope) return;
    m->scope = scope_new(NULL);
    Scope *sc = (Scope *)m->scope;

    for (int i = 0; i < m->decls.len; i++) {
        Decl *d = VEC_AT(&m->decls, Decl, i);
        d->mod = m;
        switch (d->kind) {
            case D_USE: break;
            case D_STRUCT: case D_ENUM: case D_ALIAS: {
                Sym *prev = scope_get_local(sc, d->name);
                if (prev) {
                    Diag *dg = serr(d->span, "`%s` is declared more than once in this module", d->name);
                    diag_note(dg, prev->span, "the first declaration is here");
                }
                Sym *s = NEW(Sym);
                s->kind = SYM_TYPE; s->name = d->name; s->decl = d; s->span = d->span; s->mod = m;
                scope_put(sc, s);
                d->sym = s;
                break;
            }
            case D_CONST: {
                Sym *s = NEW(Sym);
                s->kind = SYM_CONST; s->name = d->name; s->decl = d; s->span = d->span; s->mod = m;
                scope_put(sc, s);
                d->sym = s;
                vec_push(&g_unit.globals, s);
                break;
            }
            case D_FN: {
                if (d->recv) break;   /* methods live in the method table */
                Sym *prev = scope_get_local(sc, d->name);
                if (prev && prev->kind == SYM_FN) {
                    Diag *dg = serr(d->span, "function `%s` is declared more than once", d->name);
                    diag_note(dg, prev->span, "the first declaration is here");
                }
                Sym *s = NEW(Sym);
                s->kind = SYM_FN; s->name = d->name; s->decl = d; s->span = d->span; s->mod = m;
                scope_put(sc, s);
                d->sym = s;
                break;
            }
            case D_TEST: break;
        }
    }
}

static void wire_uses(Module *m) {
    Scope *sc = (Scope *)m->scope;
    for (int i = 0; i < m->decls.len; i++) {
        Decl *d = VEC_AT(&m->decls, Decl, i);
        if (d->kind != D_USE) continue;
        Module *t = load_module(d->path, m->file, d->span);
        if (!t) continue;
        d->target_mod = t;
        Sym *prev = scope_get_local(sc, d->alias);
        if (prev) {
            Diag *dg = serr(d->span, "`%s` is already in scope", d->alias);
            diag_note(dg, prev->span, "previously declared here");
            diag_note(dg, NOSPAN, "help: rename the import with `use %s as other`", d->path);
        }
        Sym *s = NEW(Sym);
        s->kind = SYM_MOD; s->name = d->alias; s->mod = t; s->span = d->span;
        scope_put(sc, s);
    }
}

/* Register methods after all modules are collected. */
static void register_methods(Module *m) {
    for (int i = 0; i < m->decls.len; i++) {
        Decl *d = VEC_AT(&m->decls, Decl, i);
        if (d->kind != D_FN || !d->recv) continue;
        const char *key = NULL;
        Type *b = builtin_named(d->recv);
        if (b) key = typekey(b);
        else if (d->recv == intern("List")) key = intern("List");
        else if (d->recv == intern("Map")) key = intern("Map");
        else if (d->recv == intern("Option")) key = intern("Option");
        else if (d->recv == intern("Result")) key = intern("Result");
        else if (d->recv == intern("Error")) key = intern("Error");
        else {
            Decl *td = find_type_decl(m, d->recv);
            if (!td) {
                Diag *dg = serr(d->span, "cannot define a method on unknown type `%s`", d->recv);
                diag_note(dg, NOSPAN, "methods may only be declared on a type in this module or on a built-in type");
                continue;
            }
            char buf[512];
            snprintf(buf, sizeof buf, "%s.%s", m->modpath, td->name);
            key = intern(buf);
        }
        Decl *prev = mtab_get(key, d->name);
        if (prev) {
            Diag *dg = serr(d->span, "method `%s.%s` is defined twice", d->recv, d->name);
            diag_note(dg, prev->span, "the first definition is here");
            continue;
        }
        if (!d->has_self) {
            Diag *dg = serr(d->span, "method `%s.%s` must take `self` as its first parameter",
                            d->recv, d->name);
            diag_note(dg, NOSPAN, "help: write `fn %s.%s(self, ...)`", d->recv, d->name);
        }
        mtab_put(key, d->name, d);
    }
}

/* Fold a `const` initialiser to a literal where possible. Constants that fold
   are inlined at every use, which is both faster and means they are available
   before the runtime has started (the syscall numbers in `core` depend on it). */
typedef struct { int k; int64_t i; double f; const char *s; int slen; } CV;

static int const_eval(Expr *e, CV *out, int depth) {
    if (!e || depth > 32) return 0;
    switch (e->kind) {
        case E_INT: case E_BOOL: case E_CHAR:
            out->k = 1; out->i = e->ival; return 1;
        case E_FLOAT: out->k = 2; out->f = e->fval; return 1;
        case E_STR: out->k = 3; out->s = e->sval; out->slen = (int)e->ival; return 1;
        case E_IDENT: {
            Sym *s = (Sym *)e->sym;
            if (!s || s->kind != SYM_CONST || !s->decl) return 0;
            Decl *d = s->decl;
            if (!d->cfold) return 0;
            out->k = d->cfold; out->i = d->cfold_i; out->f = d->cfold_f;
            out->s = d->cfold_s; out->slen = d->cfold_len;
            return 1;
        }
        case E_UNARY: {
            CV a;
            if (!const_eval(e->a, &a, depth + 1)) return 0;
            if (e->op == T_MINUS) {
                if (a.k == 1) { out->k = 1; out->i = -a.i; return 1; }
                if (a.k == 2) { out->k = 2; out->f = -a.f; return 1; }
                return 0;
            }
            if (e->op == T_NOT && a.k == 1) { out->k = 1; out->i = !a.i; return 1; }
            if (e->op == T_TILDE && a.k == 1) { out->k = 1; out->i = ~a.i; return 1; }
            return 0;
        }
        case E_BINARY: {
            CV a, b;
            if (!const_eval(e->a, &a, depth + 1)) return 0;
            if (!const_eval(e->b, &b, depth + 1)) return 0;
            if (a.k == 1 && b.k == 1) {
                int64_t x = a.i, y = b.i, r = 0;
                switch (e->op) {
                    case T_PLUS: r = (int64_t)((uint64_t)x + (uint64_t)y); break;
                    case T_MINUS: r = (int64_t)((uint64_t)x - (uint64_t)y); break;
                    case T_STAR: r = (int64_t)((uint64_t)x * (uint64_t)y); break;
                    case T_SLASH: if (!y) return 0; r = x / y; break;
                    case T_PERCENT: if (!y) return 0; r = x % y; break;
                    case T_AMP: r = x & y; break;
                    case T_PIPE: r = x | y; break;
                    case T_CARET: r = x ^ y; break;
                    case T_SHL: if (y < 0 || y > 63) return 0; r = (int64_t)((uint64_t)x << y); break;
                    case T_SHR: if (y < 0 || y > 63) return 0; r = x >> y; break;
                    case T_EQEQ: r = x == y; break;
                    case T_BANGEQ: r = x != y; break;
                    case T_LT: r = x < y; break;
                    case T_LE: r = x <= y; break;
                    case T_GT: r = x > y; break;
                    case T_GE: r = x >= y; break;
                    case T_AND: r = x && y; break;
                    case T_OR: r = x || y; break;
                    default: return 0;
                }
                out->k = 1; out->i = r; return 1;
            }
            if (a.k == 2 && b.k == 2) {
                double r;
                switch (e->op) {
                    case T_PLUS: r = a.f + b.f; break;
                    case T_MINUS: r = a.f - b.f; break;
                    case T_STAR: r = a.f * b.f; break;
                    case T_SLASH: if (b.f == 0) return 0; r = a.f / b.f; break;
                    default: return 0;
                }
                out->k = 2; out->f = r; return 1;
            }
            if (a.k == 3 && b.k == 3 && e->op == T_PLUS) {
                char *buf = (char *)arena_alloc(&g_arena, (size_t)(a.slen + b.slen + 1));
                memcpy(buf, a.s, (size_t)a.slen);
                memcpy(buf + a.slen, b.s, (size_t)b.slen);
                buf[a.slen + b.slen] = 0;
                out->k = 3; out->s = intern_n(buf, (size_t)(a.slen + b.slen));
                out->slen = a.slen + b.slen;
                return 1;
            }
            return 0;
        }
        default: return 0;
    }
}

static void check_consts(Module *m) {
    Module *save = cur_mod;
    cur_mod = m;
    FnCtx fc;
    memset(&fc, 0, sizeof fc);
    fc.mod = m;
    fc.scope = (Scope *)m->scope;
    FnInst dummy;
    memset(&dummy, 0, sizeof dummy);
    dummy.name = intern("<const>");
    fc.inst = &dummy;
    FnCtx *savefn = cur_fn;
    cur_fn = &fc;
    for (int i = 0; i < m->decls.len; i++) {
        Decl *d = VEC_AT(&m->decls, Decl, i);
        if (d->kind != D_CONST) continue;
        Type *want = d->texpr ? resolve_type(d->texpr, m, NULL) : NULL;
        Type *t = check_expr(d->value, want);
        if (want && !assignable(t, want)) want_err(d->value->span, want, t, "in `const`");
        d->type = want ? want : t;
        if (d->sym) ((Sym *)d->sym)->type = d->type;
        if (d->value->kind != E_INT && d->value->kind != E_FLOAT &&
            d->value->kind != E_STR && d->value->kind != E_BOOL &&
            d->value->kind != E_CHAR && d->value->kind != E_BINARY &&
            d->value->kind != E_UNARY && d->value->kind != E_IDENT &&
            d->value->kind != E_LIST && d->value->kind != E_MAP &&
            d->value->kind != E_STRUCT && d->value->kind != E_NIL &&
            d->value->kind != E_INTERP && d->value->kind != E_FIELD) {
            serr(d->value->span, "`const` initialisers must be constant expressions");
        }
    }
    cur_fn = savefn;
    cur_mod = save;
}

static void mark_expr_unused(void) { }

int sema_run(Unit *u) {
    sema_errors = 0;
    for (int i = 0; i < u->modules.len; i++) collect_module(VEC_AT(&u->modules, Module, i));
    for (int i = 0; i < u->modules.len; i++) wire_uses(VEC_AT(&u->modules, Module, i));
    /* loading modules may append more */
    for (int i = 0; i < u->modules.len; i++) {
        Module *m = VEC_AT(&u->modules, Module, i);
        if (!m->scope) { collect_module(m); wire_uses(m); }
    }
    for (int i = 0; i < u->modules.len; i++) register_methods(VEC_AT(&u->modules, Module, i));
    for (int i = 0; i < u->modules.len; i++) check_consts(VEC_AT(&u->modules, Module, i));
    /* fold constants to literals (fixpoint, so consts may refer to consts) */
    for (int round = 0; round < 8; round++) {
        int changed = 0;
        for (int i = 0; i < u->modules.len; i++) {
            Module *m = VEC_AT(&u->modules, Module, i);
            for (int j = 0; j < m->decls.len; j++) {
                Decl *d = VEC_AT(&m->decls, Decl, j);
                if (d->kind != D_CONST || d->cfold) continue;
                CV cv;
                memset(&cv, 0, sizeof cv);
                if (const_eval(d->value, &cv, 0)) {
                    d->cfold = cv.k;
                    d->cfold_i = cv.i;
                    d->cfold_f = cv.f;
                    d->cfold_s = cv.s;
                    d->cfold_len = cv.slen;
                    changed = 1;
                }
            }
        }
        if (!changed) break;
    }

    /* seed: main (or all tests) plus every non-generic function so that
       `vela check` reports errors in code that is not yet called. */
    for (int i = 0; i < u->modules.len; i++) {
        Module *m = VEC_AT(&u->modules, Module, i);
        for (int j = 0; j < m->decls.len; j++) {
            Decl *d = VEC_AT(&m->decls, Decl, j);
            if (d->kind == D_FN && d->generics.len == 0)
                instantiate(d, NULL, d->span);
            else if (d->kind == D_TEST && u->build_tests) {
                Decl *fd = NEW(Decl);
                *fd = *d;
                fd->kind = D_FN;
                fd->name = intern("$test");
                FnInst *fi = instantiate(fd, NULL, d->span);
                if (fi) { fi->is_test = 1; fi->test_name = d->name; vec_push(&u->tests, fi); }
            }
        }
    }

    /* drain the work queue (monomorphisation adds to it) */
    int guard = 0;
    while (work_queue.len) {
        if (++guard > 200000) { serr(NOSPAN, "compiler: instantiation limit exceeded"); break; }
        FnInst *fi = VEC_AT(&work_queue, FnInst, work_queue.len - 1);
        work_queue.len--;
        check_fn_body(fi);
    }
    return sema_errors == 0 && diag_error_count() == 0;
}
