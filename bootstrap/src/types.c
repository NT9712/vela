/* types.c — the Vela type universe.
 *
 * Every Vela value occupies exactly one 64-bit machine word. Primitives (Int,
 * Float, Bool, Byte, payload-free enums) are that word; everything else is a
 * pointer to a garbage-collected heap object. This uniformity is what keeps the
 * backend small and the calling convention trivial.
 */
#include "vela.h"

Type *ty_void, *ty_int, *ty_float, *ty_bool, *ty_byte, *ty_str,
     *ty_range, *ty_any, *ty_error;

static Type *mk(TypeKind k) {
    Type *t = NEW(Type);
    t->kind = k;
    t->size = 8;
    return t;
}

__attribute__((constructor))
static void ty_init(void) {
    ty_void  = mk(TY_VOID);
    ty_int   = mk(TY_INT);
    ty_float = mk(TY_FLOAT);
    ty_bool  = mk(TY_BOOL);
    ty_byte  = mk(TY_BYTE);
    ty_str   = mk(TY_STR);
    ty_range = mk(TY_RANGE);
    ty_any   = mk(TY_ANY);
    ty_error = mk(TY_ERRTYPE);
}

/* Interning caches so that ty_eq can usually be a pointer compare. */
static Vec cache_list, cache_map, cache_opt, cache_res, cache_fn;

Type *ty_list(Type *e) {
    for (int i = 0; i < cache_list.len; i++) {
        Type *t = VEC_AT(&cache_list, Type, i);
        if (t->elem == e) return t;
    }
    Type *t = mk(TY_LIST); t->elem = e;
    vec_push(&cache_list, t);
    return t;
}

Type *ty_map(Type *k, Type *v) {
    for (int i = 0; i < cache_map.len; i++) {
        Type *t = VEC_AT(&cache_map, Type, i);
        if (t->elem == k && t->val == v) return t;
    }
    Type *t = mk(TY_MAP); t->elem = k; t->val = v;
    vec_push(&cache_map, t);
    return t;
}

Type *ty_opt(Type *e) {
    if (e && e->kind == TY_OPT) return e;   /* ??T collapses to ?T */
    for (int i = 0; i < cache_opt.len; i++) {
        Type *t = VEC_AT(&cache_opt, Type, i);
        if (t->elem == e) return t;
    }
    Type *t = mk(TY_OPT); t->elem = e;
    vec_push(&cache_opt, t);
    return t;
}

Type *ty_res(Type *e) {
    for (int i = 0; i < cache_res.len; i++) {
        Type *t = VEC_AT(&cache_res, Type, i);
        if (t->elem == e) return t;
    }
    Type *t = mk(TY_RES); t->elem = e;
    vec_push(&cache_res, t);
    return t;
}

Type *ty_fn(Vec params, Type *ret) {
    for (int i = 0; i < cache_fn.len; i++) {
        Type *t = VEC_AT(&cache_fn, Type, i);
        if (t->ret != ret || t->params.len != params.len) continue;
        int same = 1;
        for (int j = 0; j < params.len; j++)
            if (t->params.data[j] != params.data[j]) { same = 0; break; }
        if (same) return t;
    }
    Type *t = mk(TY_FN);
    t->ret = ret;
    for (int j = 0; j < params.len; j++) vec_push(&t->params, params.data[j]);
    vec_push(&cache_fn, t);
    return t;
}

int ty_eq(Type *a, Type *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
        case TY_LIST: case TY_OPT: case TY_RES: return ty_eq(a->elem, b->elem);
        case TY_MAP:  return ty_eq(a->elem, b->elem) && ty_eq(a->val, b->val);
        case TY_FN: {
            if (a->params.len != b->params.len) return 0;
            for (int i = 0; i < a->params.len; i++)
                if (!ty_eq(VEC_AT(&a->params, Type, i), VEC_AT(&b->params, Type, i))) return 0;
            return ty_eq(a->ret, b->ret);
        }
        case TY_STRUCT: case TY_ENUM: {
            if (a->decl != b->decl) return 0;
            if (a->targs.len != b->targs.len) return 0;
            for (int i = 0; i < a->targs.len; i++)
                if (!ty_eq(VEC_AT(&a->targs, Type, i), VEC_AT(&b->targs, Type, i))) return 0;
            return 1;
        }
        case TY_GENERIC: return a->name == b->name;
        default: return 1;
    }
}

/* Is this type represented as a pointer to a heap object? */
int ty_is_ref(Type *t) {
    if (!t) return 0;
    switch (t->kind) {
        case TY_INT: case TY_FLOAT: case TY_BOOL: case TY_BYTE: case TY_VOID:
            return 0;
        case TY_ENUM:
            return !t->is_prim;
        case TY_OPT:
            /* ?T over a primitive is a boxed cell; over a reference it is a
               nullable pointer. Either way the word is a pointer or nil. */
            return 1;
        default:
            return 1;
    }
}

static void ty_write(Buf *b, Type *t) {
    if (!t) { buf_str(b, "<unknown>"); return; }
    switch (t->kind) {
        case TY_VOID:  buf_str(b, "Void"); break;
        case TY_INT:   buf_str(b, "Int"); break;
        case TY_FLOAT: buf_str(b, "Float"); break;
        case TY_BOOL:  buf_str(b, "Bool"); break;
        case TY_BYTE:  buf_str(b, "Byte"); break;
        case TY_STR:   buf_str(b, "Str"); break;
        case TY_RANGE: buf_str(b, "Range"); break;
        case TY_ANY:   buf_str(b, "_"); break;
        case TY_ERRTYPE: buf_str(b, "Error"); break;
        case TY_LIST:  buf_u8(b, '['); ty_write(b, t->elem); buf_u8(b, ']'); break;
        case TY_MAP:   buf_u8(b, '{'); ty_write(b, t->elem); buf_str(b, ": ");
                       ty_write(b, t->val); buf_u8(b, '}'); break;
        case TY_OPT:   buf_u8(b, '?'); ty_write(b, t->elem); break;
        case TY_RES:   buf_u8(b, '!'); ty_write(b, t->elem); break;
        case TY_GENERIC: buf_str(b, t->name ? t->name : "T"); break;
        case TY_FN: {
            buf_str(b, "fn(");
            for (int i = 0; i < t->params.len; i++) {
                if (i) buf_str(b, ", ");
                ty_write(b, VEC_AT(&t->params, Type, i));
            }
            buf_u8(b, ')');
            if (t->ret && t->ret->kind != TY_VOID) {
                buf_str(b, " -> ");
                ty_write(b, t->ret);
            }
            break;
        }
        case TY_STRUCT: case TY_ENUM: {
            buf_str(b, t->decl ? t->decl->name : (t->name ? t->name : "?"));
            if (t->targs.len) {
                buf_u8(b, '[');
                for (int i = 0; i < t->targs.len; i++) {
                    if (i) buf_str(b, ", ");
                    ty_write(b, VEC_AT(&t->targs, Type, i));
                }
                buf_u8(b, ']');
            }
            break;
        }
    }
}

const char *ty_str_of(Type *t) {
    Buf b; memset(&b, 0, sizeof b);
    ty_write(&b, t);
    buf_u8(&b, 0);
    const char *s = intern((char *)b.data);
    buf_free(&b);
    return s;
}

/* ---------------- scopes ---------------- */

Scope *scope_new(Scope *parent) {
    Scope *s = NEW(Scope);
    s->parent = parent;
    s->nbuckets = 32;
    s->buckets = NEWN(Sym *, s->nbuckets);
    memset(s->buckets, 0, sizeof(Sym *) * (size_t)s->nbuckets);
    return s;
}

static uint32_t ptrhash(const void *p) {
    uintptr_t v = (uintptr_t)p;
    v ^= v >> 33; v *= 0xff51afd7ed558ccdULL; v ^= v >> 29;
    return (uint32_t)v;
}

static void scope_grow(Scope *s) {
    int nb = s->nbuckets * 2;
    Sym **nbk = NEWN(Sym *, nb);
    memset(nbk, 0, sizeof(Sym *) * (size_t)nb);
    for (int i = 0; i < s->nbuckets; i++) {
        Sym *sym = s->buckets[i];
        while (sym) {
            Sym *next = sym->next;
            uint32_t h = ptrhash(sym->name) % (uint32_t)nb;
            sym->next = nbk[h];
            nbk[h] = sym;
            sym = next;
        }
    }
    s->buckets = nbk;
    s->nbuckets = nb;
}

Sym *scope_put(Scope *s, Sym *sym) {
    if (s->count > s->nbuckets * 2) scope_grow(s);
    uint32_t h = ptrhash(sym->name) % (uint32_t)s->nbuckets;
    sym->next = s->buckets[h];
    s->buckets[h] = sym;
    s->count++;
    return sym;
}

Sym *scope_get_local(Scope *s, const char *name) {
    if (!s) return NULL;
    uint32_t h = ptrhash(name) % (uint32_t)s->nbuckets;
    for (Sym *sym = s->buckets[h]; sym; sym = sym->next)
        if (sym->name == name) return sym;
    return NULL;
}

Sym *scope_get(Scope *s, const char *name) {
    for (; s; s = s->parent) {
        Sym *sym = scope_get_local(s, name);
        if (sym) return sym;
    }
    return NULL;
}
