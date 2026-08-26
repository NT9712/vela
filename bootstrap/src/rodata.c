/* rodata.c — read-only data segment construction.
 *
 * Holds string literal objects, static closures, and runtime type descriptors.
 * Descriptors are what let a single `any_to_str` / `any_eq` / `any_hash`
 * implementation in the standard library work for every type: the compiler
 * emits a compact description of each type's shape, and the runtime walks it.
 */
#include "vela.h"

RoData g_rodata;

typedef struct { int at; int kind; int target; } Fixup;
/* kind 0: patch with rodata_base + target
   kind 1: patch with the entry address of function index `target` */

static void ro_align(int n) {
    while (g_rodata.data.len % (size_t)n) buf_u8(&g_rodata.data, 0);
}

static void ro_u64(uint64_t v) { buf_u64(&g_rodata.data, v); }

static void ro_fix(int at, int kind, int target) {
    Fixup *f = NEW(Fixup);
    f->at = at; f->kind = kind; f->target = target;
    vec_push(&g_rodata.fixups, f);
}

int rodata_here(void) { return (int)g_rodata.data.len; }

/* ---- object header helper ---- */
/* +0 u32 size | +4 u8 kind | +5 u8 mark | +6 u16 tag | +8 u64 aux */
enum { OKIND_ATOMIC = 0, OKIND_SCAN = 1, OKIND_STATIC = 2 };

static void ro_header(uint32_t size, uint8_t kind, uint16_t tag, uint64_t aux) {
    buf_u32(&g_rodata.data, size);
    buf_u8(&g_rodata.data, kind);
    buf_u8(&g_rodata.data, 1);     /* permanently marked: never swept */
    buf_u16(&g_rodata.data, tag);
    buf_u64(&g_rodata.data, aux);
}

/* ---- interned static strings ---- */

typedef struct SEnt { const char *s; int len; int off; struct SEnt *next; } SEnt;
#define SN 2048
static SEnt *stab[SN];

static uint32_t shash(const char *s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

int rodata_str(const char *s, int len) {
    uint32_t h = shash(s, len) % SN;
    for (SEnt *e = stab[h]; e; e = e->next)
        if (e->len == len && memcmp(e->s, s, (size_t)len) == 0) return e->off;
    ro_align(16);
    int off = (int)g_rodata.data.len;
    ro_header((uint32_t)(16 + len + 1), OKIND_ATOMIC | OKIND_STATIC, 0, (uint64_t)len);
    buf_put(&g_rodata.data, s, (size_t)len);
    buf_u8(&g_rodata.data, 0);
    SEnt *e = NEW(SEnt);
    e->s = s; e->len = len; e->off = off; e->next = stab[h];
    stab[h] = e;
    return off;
}

/* ---- static closures for plain function references ---- */

typedef struct CEnt { FnInst *f; int off; struct CEnt *next; } CEnt;
static CEnt *ctab;

int rodata_closure(FnInst *f) {
    for (CEnt *e = ctab; e; e = e->next) if (e->f == f) return e->off;
    ro_align(16);
    int off = (int)g_rodata.data.len;
    ro_header(32, OKIND_SCAN | OKIND_STATIC, 0, 0);
    int at = (int)g_rodata.data.len;
    ro_u64(0);                 /* code address, patched */
    ro_fix(at, 1, f->index);
    ro_u64(0);                 /* ncaps */
    CEnt *e = NEW(CEnt);
    e->f = f; e->off = off; e->next = ctab;
    ctab = e;
    return off;
}

/* ---- type descriptors ---- */

enum {
    TD_VOID = 0, TD_INT, TD_FLOAT, TD_BOOL, TD_BYTE, TD_STR, TD_LIST, TD_MAP,
    TD_OPT, TD_RES, TD_FN, TD_RANGE, TD_STRUCT, TD_ENUM, TD_ERROR
};

typedef struct TDEnt { Type *t; int off; struct TDEnt *next; } TDEnt;
static TDEnt *tdtab;

#define TD_SIZE 56

static int td_kind(Type *t) {
    switch (t->kind) {
        case TY_VOID: return TD_VOID;
        case TY_INT: return TD_INT;
        case TY_FLOAT: return TD_FLOAT;
        case TY_BOOL: return TD_BOOL;
        case TY_BYTE: return TD_BYTE;
        case TY_STR: return TD_STR;
        case TY_LIST: return TD_LIST;
        case TY_MAP: return TD_MAP;
        case TY_OPT: return TD_OPT;
        case TY_RES: return TD_RES;
        case TY_FN: return TD_FN;
        case TY_RANGE: return TD_RANGE;
        case TY_STRUCT: return TD_STRUCT;
        case TY_ENUM: return TD_ENUM;
        case TY_ERRTYPE: return TD_ERROR;
        default: return TD_INT;
    }
}

static void patch64(int at, uint64_t v) {
    memcpy(g_rodata.data.data + at, &v, 8);
}

int typedesc_for(Type *t) {
    if (!t) t = ty_int;
    for (TDEnt *e = tdtab; e; e = e->next) if (ty_eq(e->t, t)) return e->off;

    ro_align(16);
    int off = (int)g_rodata.data.len;
    /* reserve first so recursive types terminate */
    for (int i = 0; i < TD_SIZE; i++) buf_u8(&g_rodata.data, 0);
    TDEnt *e = NEW(TDEnt);
    e->t = t; e->off = off; e->next = tdtab;
    tdtab = e;

    const char *nm = ty_str_of(t);
    int name_off = rodata_str(nm, (int)strlen(nm));

    /* gather sub-types and names */
    Vec subs; memset(&subs, 0, sizeof subs);
    Vec names; memset(&names, 0, sizeof names);
    uint64_t aux = 0;

    switch (t->kind) {
        case TY_LIST: vec_push(&subs, t->elem); break;
        case TY_MAP:  vec_push(&subs, t->elem); vec_push(&subs, t->val); break;
        case TY_OPT:  vec_push(&subs, t->elem ? t->elem : ty_int);
                      aux = ty_is_ref(t->elem ? t->elem : ty_int) ? 0 : 1; break;
        case TY_RES:  vec_push(&subs, t->elem ? t->elem : ty_void); break;
        case TY_STRUCT:
            for (int i = 0; i < t->fields.len; i++) {
                vec_push(&subs, t->fields.data[i]);
                StructField *f = VEC_AT(&t->decl->fields, StructField, i);
                vec_push(&names, (void *)f->name);
            }
            break;
        case TY_ENUM:
            aux = t->is_prim ? 1 : 0;
            for (int i = 0; i < t->variants.len; i++) {
                EnumVariant *v = VEC_AT(&t->decl->variants, EnumVariant, i);
                vec_push(&names, (void *)v->name);
            }
            break;
        default: break;
    }

    int subs_off = 0, names_off = 0, vsubs_off = 0;

    if (t->kind == TY_ENUM) {
        /* For enums, `subs` points at an array of nsub pairs:
              [ptr to payload-descriptor array, payload count]
           so the runtime can walk any variant's payload. */
        int nv = t->variants.len;
        Vec arrs; memset(&arrs, 0, sizeof arrs);
        Vec cnts; memset(&cnts, 0, sizeof cnts);
        for (int i = 0; i < nv; i++) {
            Vec *pl = (Vec *)t->variants.data[i];
            Vec offs; memset(&offs, 0, sizeof offs);
            for (int j = 0; j < pl->len; j++)
                vec_push(&offs, (void *)(intptr_t)typedesc_for(VEC_AT(pl, Type, j)));
            ro_align(8);
            int a = (int)g_rodata.data.len;
            for (int j = 0; j < pl->len; j++) {
                ro_fix((int)g_rodata.data.len, 0, (int)(intptr_t)offs.data[j]);
                ro_u64(0);
            }
            vec_push(&arrs, (void *)(intptr_t)a);
            vec_push(&cnts, (void *)(intptr_t)pl->len);
        }
        ro_align(8);
        vsubs_off = (int)g_rodata.data.len;
        for (int i = 0; i < nv; i++) {
            int cnt = (int)(intptr_t)cnts.data[i];
            if (cnt) ro_fix((int)g_rodata.data.len, 0, (int)(intptr_t)arrs.data[i]);
            ro_u64(0);
            ro_u64((uint64_t)cnt);
        }
        subs_off = vsubs_off;
    } else if (subs.len) {
        Vec offs; memset(&offs, 0, sizeof offs);
        for (int i = 0; i < subs.len; i++)
            vec_push(&offs, (void *)(intptr_t)typedesc_for(VEC_AT(&subs, Type, i)));
        ro_align(8);
        subs_off = (int)g_rodata.data.len;
        for (int i = 0; i < subs.len; i++) {
            ro_fix((int)g_rodata.data.len, 0, (int)(intptr_t)offs.data[i]);
            ro_u64(0);
        }
    }

    if (names.len) {
        Vec offs; memset(&offs, 0, sizeof offs);
        for (int i = 0; i < names.len; i++) {
            const char *n = (const char *)names.data[i];
            vec_push(&offs, (void *)(intptr_t)rodata_str(n, (int)strlen(n)));
        }
        ro_align(8);
        names_off = (int)g_rodata.data.len;
        for (int i = 0; i < names.len; i++) {
            ro_fix((int)g_rodata.data.len, 0, (int)(intptr_t)offs.data[i]);
            ro_u64(0);
        }
    }

    FnInst *ts = find_to_str(t);
    int ts_off = 0;
    if (ts) ts_off = rodata_closure(ts);

    /* now fill the reserved descriptor */
    patch64(off + 0, (uint64_t)td_kind(t));
    ro_fix(off + 8, 0, name_off);   patch64(off + 8, 0);
    int nsub = (t->kind == TY_ENUM) ? t->variants.len :
               (t->kind == TY_STRUCT) ? t->fields.len : subs.len;
    patch64(off + 16, (uint64_t)nsub);
    if (subs_off) { ro_fix(off + 24, 0, subs_off); }
    patch64(off + 24, 0);
    if (names_off) { ro_fix(off + 32, 0, names_off); }
    patch64(off + 32, 0);
    patch64(off + 40, aux);
    if (ts_off) { ro_fix(off + 48, 0, ts_off); }
    patch64(off + 48, 0);
    return off;
}

void typedesc_reset(void) {
    memset(stab, 0, sizeof stab);
    ctab = NULL;
    tdtab = NULL;
    g_rodata.data.len = 0;
    g_rodata.fixups.len = 0;
}

/* Apply fixups once the final layout is known. */
int rodata_fixup_fn_targets(int **out) {
    static Vec tmp;
    tmp.len = 0;
    for (int i = 0; i < g_rodata.fixups.len; i++) {
        Fixup *f = VEC_AT(&g_rodata.fixups, Fixup, i);
        if (f->kind == 1) vec_push(&tmp, (void *)(intptr_t)f->target);
    }
    static int *arr; static int cap;
    if (tmp.len > cap) { cap = tmp.len + 16; arr = (int *)realloc(arr, sizeof(int) * (size_t)cap); }
    for (int i = 0; i < tmp.len; i++) arr[i] = (int)(intptr_t)tmp.data[i];
    *out = arr;
    return tmp.len;
}

void rodata_relocate(uint64_t base, uint64_t *fn_addrs, int nfns) {
    for (int i = 0; i < g_rodata.fixups.len; i++) {
        Fixup *f = VEC_AT(&g_rodata.fixups, Fixup, i);
        uint64_t v = 0;
        if (f->kind == 0) v = base + (uint64_t)f->target;
        else if (f->kind == 1 && f->target >= 0 && f->target < nfns) v = fn_addrs[f->target];
        memcpy(g_rodata.data.data + f->at, &v, 8);
    }
}
