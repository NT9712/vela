/* irgen.c — lower the type-annotated AST into Vela IR.
 *
 * The IR is a list of basic blocks holding three-address instructions over
 * virtual registers. Two invariants make the backend simple and are checked by
 * `ir_verify`:
 *
 *   1. a virtual register is defined exactly once, and
 *   2. it is used only in the block that defines it.
 *
 * Anything that must cross a block boundary travels through a frame slot. That
 * removes any need for phi nodes and makes register allocation a per-block
 * linear scan.
 */
#include "vela.h"

/* object layout constants, shared with the runtime (lib/core/core.vela) */
#define HDR         16
#define OFF_AUX     8
#define OKIND_ATOMIC 0
#define OKIND_SCAN   1

#define LIST_CAP   16
#define LIST_DATA  24
#define LIST_SIZE  32
#define RES_TAG    16
#define RES_VAL    24
#define RES_SIZE   32
#define BOX_VAL    16
#define BOX_SIZE   24
#define CLOS_CODE  16
#define CLOS_NCAP  24
#define CLOS_HDR   32

int rodata_str(const char *s, int len);
int rodata_closure(FnInst *f);

typedef struct {
    FnInst *f;
    int     blk;
    Vec     break_stack;
    Vec     cont_stack;
} Gen;

/* ------------------------------------------------------------------ */
/* IR building primitives                                              */
/* ------------------------------------------------------------------ */

int ir_new_block(FnInst *f, const char *label) {
    IrBlock *b = NEW(IrBlock);
    b->id = f->blocks.len;
    b->label = label;
    vec_push(&f->blocks, b);
    return b->id;
}

static int blk_terminated(FnInst *f, int blk) {
    IrBlock *b = VEC_AT(&f->blocks, IrBlock, blk);
    if (!b->ins.len) return 0;
    IrIns *last = VEC_AT(&b->ins, IrIns, b->ins.len - 1);
    return last->op == IR_JMP || last->op == IR_BR ||
           last->op == IR_RET || last->op == IR_RETV;
}

IrIns *ir_emit(FnInst *f, int blk, IrOp op) {
    IrBlock *b = VEC_AT(&f->blocks, IrBlock, blk);
    IrIns *i = NEW(IrIns);
    i->op = op;
    i->dst = -1; i->a = -1; i->b = -1;
    i->size = 8;
    i->span = NOSPAN;
    vec_push(&b->ins, i);
    return i;
}

static IrIns *g_ins(Gen *g, IrOp op) {
    if (blk_terminated(g->f, g->blk)) g->blk = ir_new_block(g->f, "unreachable");
    return ir_emit(g->f, g->blk, op);
}

static int new_vreg(Gen *g, int is_float) {
    FnInst *f = g->f;
    int v = f->nvregs++;
    if (f->nvregs > f->vreg_cap) {
        int nc = f->vreg_cap ? f->vreg_cap * 2 : 64;
        while (nc < f->nvregs) nc *= 2;
        int *nf = NEWN(int, nc);
        memset(nf, 0, sizeof(int) * (size_t)nc);
        if (f->vreg_float) memcpy(nf, f->vreg_float, sizeof(int) * (size_t)f->vreg_cap);
        f->vreg_float = nf;
        f->vreg_cap = nc;
    }
    f->vreg_float[v] = is_float;
    return v;
}

static int new_slot(Gen *g) { return g->f->nslots++; }

static int emit_const(Gen *g, int64_t v) {
    IrIns *i = g_ins(g, IR_CONST);
    i->dst = new_vreg(g, 0);
    i->imm = v;
    return i->dst;
}

static int emit_constf(Gen *g, double v) {
    IrIns *i = g_ins(g, IR_CONSTF);
    i->dst = new_vreg(g, 1);
    i->fimm = v;
    return i->dst;
}

static void emit_store_local(Gen *g, int slot, int v) {
    IrIns *i = g_ins(g, IR_STORE_LOCAL);
    i->target = slot;
    i->a = v;
}

static int emit_load_local(Gen *g, int slot, int is_float) {
    IrIns *i = g_ins(g, IR_LOAD_LOCAL);
    i->dst = new_vreg(g, is_float);
    i->target = slot;
    return i->dst;
}

static int emit_load_mem(Gen *g, int base, int off, int is_float, int size) {
    IrIns *i = g_ins(g, IR_LOAD_MEM);
    i->dst = new_vreg(g, is_float);
    i->a = base;
    i->imm = off;
    i->size = size;
    return i->dst;
}

static void emit_store_mem(Gen *g, int base, int off, int val, int size) {
    IrIns *i = g_ins(g, IR_STORE_MEM);
    i->a = base;
    i->b = val;
    i->imm = off;
    i->size = size;
}

static void emit_jmp(Gen *g, int target) {
    if (blk_terminated(g->f, g->blk)) return;
    IrIns *i = ir_emit(g->f, g->blk, IR_JMP);
    i->target = target;
}

static void emit_br(Gen *g, int cond, int t, int f) {
    if (blk_terminated(g->f, g->blk)) return;
    IrIns *i = ir_emit(g->f, g->blk, IR_BR);
    i->a = cond;
    i->target = t;
    i->target2 = f;
}

/* ---- runtime function lookup ---- */

static FnInst *rt_cache[64];
static const char *rt_names[64];
static int rt_n;

static FnInst *rt(const char *name) {
    const char *n = intern(name);
    for (int i = 0; i < rt_n; i++) if (rt_names[i] == n) return rt_cache[i];
    char mangled[256];
    snprintf(mangled, sizeof mangled, "core.%s", name);
    const char *mn = intern(mangled);
    for (int i = 0; i < g_unit.fns.len; i++) {
        FnInst *f = VEC_AT(&g_unit.fns, FnInst, i);
        if (f->name == mn) {
            if (rt_n < 64) { rt_names[rt_n] = n; rt_cache[rt_n] = f; rt_n++; }
            return f;
        }
    }
    fatal("internal: runtime function `core.%s` is missing from lib/core", name);
    return NULL;
}

static int emit_call(Gen *g, FnInst *target, int *args, int nargs, int ret_float) {
    IrIns *i = g_ins(g, IR_CALL);
    i->target = target ? target->index : -1;
    i->dbg = target ? target->name : "?";
    for (int k = 0; k < nargs; k++) vec_push(&i->args, (void *)(intptr_t)args[k]);
    i->dst = new_vreg(g, ret_float);
    return i->dst;
}

static int emit_call_n(Gen *g, const char *rtname, int *args, int nargs) {
    return emit_call(g, rt(rtname), args, nargs, 0);
}

/* allocate a heap object of `size` bytes with GC kind `kind` */
static int emit_alloc(Gen *g, int size, int kind, int tag, int64_t aux) {
    int a[2];
    a[0] = emit_const(g, size);
    a[1] = emit_const(g, kind);
    int p = emit_call_n(g, "alloc", a, 2);
    if (tag) {
        int tv = emit_const(g, tag);
        IrIns *i = g_ins(g, IR_STORE_MEM);
        i->a = p; i->b = tv; i->imm = 6; i->size = 2;
    }
    if (aux) {
        int v = emit_const(g, aux);
        emit_store_mem(g, p, OFF_AUX, v, 8);
    }
    return p;
}

/* ------------------------------------------------------------------ */
/* expression lowering                                                  */
/* ------------------------------------------------------------------ */

static int gen_expr(Gen *g, Expr *e);
static void gen_stmt(Gen *g, Stmt *s);
static void gen_block(Gen *g, Stmt *s);

static int is_float_ty(Type *t) { return t && t->kind == TY_FLOAT; }

static int bitcast_f2i(Gen *g, int v) {
    IrIns *i = g_ins(g, IR_F2I);
    i->dst = new_vreg(g, 0);
    i->a = v;
    i->imm = 1;
    return i->dst;
}
static int bitcast_i2f(Gen *g, int v) {
    IrIns *i = g_ins(g, IR_I2F);
    i->dst = new_vreg(g, 1);
    i->a = v;
    i->imm = 1;
    return i->dst;
}

/* Widen a value of type `from` for storage in a slot of type `to`. Only
   optional boxing and result wrapping need code. */
static int gen_coerce(Gen *g, int v, Type *from, Type *to) {
    if (!to || !from) return v;
    if (to->kind == TY_OPT && from->kind != TY_OPT) {
        if (from->kind == TY_VOID) return emit_const(g, 0);
        Type *inner = to->elem ? to->elem : from;
        if (!ty_is_ref(inner)) {
            int raw = is_float_ty(inner) ? bitcast_f2i(g, v) : v;
            int p = emit_alloc(g, BOX_SIZE, OKIND_ATOMIC, 0, 0);
            emit_store_mem(g, p, BOX_VAL, raw, 8);
            return p;
        }
        return v;
    }
    if (to->kind == TY_RES && from->kind != TY_RES) {
        int raw = (from->kind == TY_VOID) ? emit_const(g, 0)
                : is_float_ty(from) ? bitcast_f2i(g, v) : v;
        int p = emit_alloc(g, RES_SIZE, OKIND_SCAN, 0, 0);
        int z = emit_const(g, 0);
        emit_store_mem(g, p, RES_TAG, z, 8);
        emit_store_mem(g, p, RES_VAL, raw, 8);
        return p;
    }
    return v;
}

static int unbox_opt(Gen *g, int v, Type *opt) {
    Type *inner = opt && opt->elem ? opt->elem : ty_int;
    if (!ty_is_ref(inner)) {
        int raw = emit_load_mem(g, v, BOX_VAL, 0, 8);
        return is_float_ty(inner) ? bitcast_i2f(g, raw) : raw;
    }
    return v;
}

/* raw 64-bit word of a value (floats become their bit pattern) */
static int gen_word(Gen *g, Expr *e) {
    int v = gen_expr(g, e);
    if (is_float_ty(e->type)) return bitcast_f2i(g, v);
    return v;
}

static int gen_str_lit(Gen *g, const char *s, int len) {
    IrIns *i = g_ins(g, IR_RODATA_ADDR);
    i->dst = new_vreg(g, 0);
    i->target = rodata_str(s, len);
    return i->dst;
}

static int gen_desc(Gen *g, Type *t) {
    IrIns *i = g_ins(g, IR_RODATA_ADDR);
    i->dst = new_vreg(g, 0);
    i->target = typedesc_for(t);
    return i->dst;
}

static int gen_to_str(Gen *g, Expr *e) {
    Type *t = e->type;
    if (t && t->kind == TY_STR) return gen_expr(g, e);
    if (e->extra) {                     /* user-defined `to_str` */
        FnInst *fi = (FnInst *)e->extra;
        int a[1];
        a[0] = gen_expr(g, e);
        return emit_call(g, fi, a, 1, 0);
    }
    int a[2];
    a[0] = gen_word(g, e);
    a[1] = gen_desc(g, t);
    return emit_call_n(g, "any_to_str", a, 2);
}

static int sym_slot(Sym *s) { return s->slot; }

static int gen_load_sym(Gen *g, Sym *s) {
    int is_f = is_float_ty(s->type);
    if (s->kind == SYM_CAPTURE) {
        int env = emit_load_local(g, 0, 0);
        int cell = emit_load_mem(g, env, CLOS_HDR + s->slot * 8, 0, 8);
        int raw = emit_load_mem(g, cell, BOX_VAL, 0, 8);
        return is_f ? bitcast_i2f(g, raw) : raw;
    }
    if (s->boxed) {
        int cell = emit_load_local(g, sym_slot(s), 0);
        int raw = emit_load_mem(g, cell, BOX_VAL, 0, 8);
        return is_f ? bitcast_i2f(g, raw) : raw;
    }
    return emit_load_local(g, sym_slot(s), is_f);
}

static void gen_store_sym(Gen *g, Sym *s, int v) {
    int raw = is_float_ty(s->type) ? bitcast_f2i(g, v) : v;
    if (s->kind == SYM_CAPTURE) {
        int env = emit_load_local(g, 0, 0);
        int cell = emit_load_mem(g, env, CLOS_HDR + s->slot * 8, 0, 8);
        emit_store_mem(g, cell, BOX_VAL, raw, 8);
        return;
    }
    if (s->boxed) {
        int cell = emit_load_local(g, sym_slot(s), 0);
        emit_store_mem(g, cell, BOX_VAL, raw, 8);
        return;
    }
    emit_store_local(g, sym_slot(s), v);
}

/* create the heap cell for a captured local */
static void gen_make_box(Gen *g, Sym *s, int v) {
    int raw = is_float_ty(s->type) ? bitcast_f2i(g, v) : v;
    int p = emit_alloc(g, BOX_SIZE, ty_is_ref(s->type) ? OKIND_SCAN : OKIND_ATOMIC, 0, 0);
    emit_store_mem(g, p, BOX_VAL, raw, 8);
    emit_store_local(g, sym_slot(s), p);
}

static void gen_bind(Gen *g, Sym *s, int v) {
    if (s->boxed) gen_make_box(g, s, v);
    else emit_store_local(g, sym_slot(s), v);
}

static int gen_binop_ir(Gen *g, int op, int cls, int a, int b, Span sp) {
    IrOp o = IR_ADD;
    int isf = (cls == OPC_FLOAT);
    switch (op) {
        case T_PLUS:    o = isf ? IR_FADD : IR_ADD; break;
        case T_MINUS:   o = isf ? IR_FSUB : IR_SUB; break;
        case T_STAR:    o = isf ? IR_FMUL : IR_MUL; break;
        case T_SLASH:   o = isf ? IR_FDIV : IR_DIV; break;
        case T_PERCENT: o = IR_MOD; break;
        case T_AMP:     o = IR_AND; break;
        case T_PIPE:    o = IR_OR; break;
        case T_CARET:   o = IR_XOR; break;
        case T_SHL:     o = IR_SHL; break;
        case T_SHR:     o = IR_SHR; break;
        case T_EQEQ:    o = isf ? IR_FEQ : IR_EQ; break;
        case T_BANGEQ:  o = isf ? IR_FNE : IR_NE; break;
        case T_LT:      o = isf ? IR_FLT : IR_LT; break;
        case T_LE:      o = isf ? IR_FLE : IR_LE; break;
        case T_GT:      o = isf ? IR_FGT : IR_GT; break;
        case T_GE:      o = isf ? IR_FGE : IR_GE; break;
        default: o = IR_ADD; break;
    }
    int cmp = (o >= IR_EQ && o <= IR_FGE);
    IrIns *i = g_ins(g, o);
    i->dst = new_vreg(g, isf && !cmp);
    i->a = a; i->b = b;
    i->span = sp;
    return i->dst;
}

static int mask_byte(Gen *g, int v) {
    int m = emit_const(g, 255);
    IrIns *i = g_ins(g, IR_AND);
    i->dst = new_vreg(g, 0); i->a = v; i->b = m;
    return i->dst;
}

static int logical_not(Gen *g, int v) {
    int one = emit_const(g, 1);
    IrIns *i = g_ins(g, IR_XOR);
    i->dst = new_vreg(g, 0); i->a = v; i->b = one;
    return i->dst;
}

static int gen_binary(Gen *g, Expr *e) {
    int op = e->op, cls = e->idx;
    if (op == T_AND || op == T_OR) {
        int slot = new_slot(g);
        int a = gen_expr(g, e->a);
        emit_store_local(g, slot, a);
        int rhs = ir_new_block(g->f, "sc.rhs");
        int end = ir_new_block(g->f, "sc.end");
        if (op == T_AND) emit_br(g, a, rhs, end);
        else emit_br(g, a, end, rhs);
        g->blk = rhs;
        int b = gen_expr(g, e->b);
        emit_store_local(g, slot, b);
        emit_jmp(g, end);
        g->blk = end;
        return emit_load_local(g, slot, 0);
    }
    if (cls == OPC_STR) {
        int a = gen_expr(g, e->a);
        int b = gen_expr(g, e->b);
        int args[2] = { a, b };
        if (op == T_PLUS) return emit_call_n(g, "str_concat", args, 2);
        if (op == T_EQEQ) return emit_call_n(g, "str_eq", args, 2);
        if (op == T_BANGEQ) return logical_not(g, emit_call_n(g, "str_eq", args, 2));
        int c = emit_call_n(g, "str_cmp", args, 2);
        int z = emit_const(g, 0);
        return gen_binop_ir(g, op, OPC_INT, c, z, e->span);
    }
    if (cls == OPC_LIST) {
        int a = gen_expr(g, e->a);
        int b = gen_expr(g, e->b);
        int d = gen_desc(g, e->type);
        int args[3] = { a, b, d };
        return emit_call_n(g, "list_concat", args, 3);
    }
    if (cls == OPC_ANY) {
        Type *t = (Type *)e->extra;
        int a = gen_word(g, e->a);
        int b = gen_word(g, e->b);
        int d = gen_desc(g, t ? t : e->a->type);
        int args[3] = { a, b, d };
        int r = emit_call_n(g, "any_eq", args, 3);
        if (op == T_BANGEQ) return logical_not(g, r);
        return r;
    }
    int a = gen_expr(g, e->a);
    int b = gen_expr(g, e->b);
    int r = gen_binop_ir(g, op, cls, a, b, e->span);
    if (cls == OPC_BYTE && (op == T_PLUS || op == T_MINUS || op == T_STAR ||
                            op == T_SHL || op == T_SHR))
        return mask_byte(g, r);
    return r;
}

static int gen_lambda(Gen *g, Expr *e) {
    FnInst *fi = (FnInst *)e->target;
    int ncap = fi ? fi->captures.len : 0;
    int size = CLOS_HDR + ncap * 8;
    int p = emit_alloc(g, size, OKIND_SCAN, 0, 0);
    IrIns *fa = g_ins(g, IR_FN_ADDR);
    fa->dst = new_vreg(g, 0);
    fa->target = fi ? fi->index : -1;
    emit_store_mem(g, p, CLOS_CODE, fa->dst, 8);
    int nc = emit_const(g, ncap);
    emit_store_mem(g, p, CLOS_NCAP, nc, 8);
    for (int i = 0; i < ncap; i++) {
        Sym *outer = VEC_AT(&fi->captures, Sym, i);
        int cell;
        if (outer->kind == SYM_CAPTURE) {
            int env = emit_load_local(g, 0, 0);
            cell = emit_load_mem(g, env, CLOS_HDR + outer->slot * 8, 0, 8);
        } else {
            cell = emit_load_local(g, sym_slot(outer), 0);
        }
        emit_store_mem(g, p, CLOS_HDR + i * 8, cell, 8);
    }
    return p;
}

static int gen_struct_lit(Gen *g, Expr *e) {
    Type *t = e->type;
    if (e->builtin == -1) {
        if (t->is_prim) return emit_const(g, e->idx);
        int np = enum_payload_count(t, e->idx);
        int *vals = NEWN(int, np > 0 ? np : 1);
        for (int i = 0; i < np && i < e->list.len; i++) {
            Expr *a = VEC_AT(&e->list, Expr, i);
            Type *ft = enum_payload_type(t, e->idx, i);
            int v = gen_expr(g, a);
            v = gen_coerce(g, v, a->type, ft);
            vals[i] = is_float_ty(ft) ? bitcast_f2i(g, v) : v;
        }
        int p = emit_alloc(g, HDR + np * 8, OKIND_SCAN, e->idx, 0);
        for (int i = 0; i < np && i < e->list.len; i++)
            emit_store_mem(g, p, HDR + i * 8, vals[i], 8);
        return p;
    }
    int nf = t->fields.len;
    int *vals = NEWN(int, nf > 0 ? nf : 1);
    int *has = NEWN(int, nf > 0 ? nf : 1);
    memset(has, 0, sizeof(int) * (size_t)(nf > 0 ? nf : 1));
    for (int i = 0; i < e->list.len; i++) {
        FieldInit *fi = VEC_AT(&e->list, FieldInit, i);
        int idx = struct_field_index(t, fi->name);
        if (idx < 0 || idx >= nf) continue;
        Type *ft = struct_field_type(t, idx);
        int v = gen_expr(g, fi->value);
        v = gen_coerce(g, v, fi->value->type, ft);
        vals[idx] = is_float_ty(ft) ? bitcast_f2i(g, v) : v;
        has[idx] = 1;
    }
    int p = emit_alloc(g, HDR + nf * 8, OKIND_SCAN, 0, 0);
    for (int i = 0; i < nf; i++)
        if (has[i]) emit_store_mem(g, p, HDR + i * 8, vals[i], 8);
    return p;
}

static int gen_list_lit(Gen *g, Expr *e) {
    Type *t = e->type;
    int n = e->list.len;
    int *vals = NEWN(int, n > 0 ? n : 1);
    for (int i = 0; i < n; i++) {
        Expr *a = VEC_AT(&e->list, Expr, i);
        int v = gen_expr(g, a);
        v = gen_coerce(g, v, a->type, t->elem);
        vals[i] = is_float_ty(t->elem) ? bitcast_f2i(g, v) : v;
    }
    int args[2];
    args[0] = emit_const(g, n);
    args[1] = emit_const(g, ty_is_ref(t->elem) ? 1 : 0);
    int p = emit_call_n(g, "list_new", args, 2);
    int pslot = new_slot(g);
    emit_store_local(g, pslot, p);
    for (int i = 0; i < n; i++) {
        int a2[2] = { emit_load_local(g, pslot, 0), vals[i] };
        emit_call_n(g, "list_push", a2, 2);
    }
    return emit_load_local(g, pslot, 0);
}

static int gen_map_lit(Gen *g, Expr *e) {
    Type *t = e->type;
    int args[2];
    args[0] = gen_desc(g, t->elem);
    args[1] = emit_const(g, ty_is_ref(t->val) ? 1 : 0);
    int m = emit_call_n(g, "map_new", args, 2);
    int mslot = new_slot(g);
    emit_store_local(g, mslot, m);
    for (int i = 0; i + 1 < e->list.len; i += 2) {
        Expr *ke = VEC_AT(&e->list, Expr, i);
        Expr *ve = VEC_AT(&e->list, Expr, i + 1);
        int k = gen_word(g, ke);
        int v = gen_expr(g, ve);
        v = gen_coerce(g, v, ve->type, t->val);
        if (is_float_ty(t->val)) v = bitcast_f2i(g, v);
        int a3[3] = { emit_load_local(g, mslot, 0), k, v };
        emit_call_n(g, "map_set", a3, 3);
    }
    return emit_load_local(g, mslot, 0);
}

static int gen_index(Gen *g, Expr *e) {
    int a = gen_expr(g, e->a);
    if (e->idx == IDX_LIST) {
        int b = gen_expr(g, e->b);
        int args[2] = { a, b };
        int r = emit_call_n(g, "list_get", args, 2);
        if (is_float_ty(e->type)) return bitcast_i2f(g, r);
        return r;
    }
    if (e->idx == IDX_STR) {
        int b = gen_expr(g, e->b);
        int args[2] = { a, b };
        return emit_call_n(g, "str_index", args, 2);
    }
    /* map lookup yields ?V */
    int k = gen_word(g, e->b);
    int args[2] = { a, k };
    int found = emit_call_n(g, "map_find", args, 2);
    int fslot = new_slot(g);
    emit_store_local(g, fslot, found);
    int out = new_slot(g);
    int bfound = ir_new_block(g->f, "map.found");
    int bnil = ir_new_block(g->f, "map.nil");
    int bend = ir_new_block(g->f, "map.end");
    emit_br(g, found, bfound, bnil);

    g->blk = bfound;
    Type *vt = e->a->type->val;
    int addr = emit_load_local(g, fslot, 0);
    int v = emit_load_mem(g, addr, 0, 0, 8);
    if (!ty_is_ref(vt)) {
        int p = emit_alloc(g, BOX_SIZE, OKIND_ATOMIC, 0, 0);
        emit_store_mem(g, p, BOX_VAL, v, 8);
        v = p;
    }
    emit_store_local(g, out, v);
    emit_jmp(g, bend);

    g->blk = bnil;
    emit_store_local(g, out, emit_const(g, 0));
    emit_jmp(g, bend);

    g->blk = bend;
    return emit_load_local(g, out, 0);
}

static int gen_call(Gen *g, Expr *e);
static int gen_builtin(Gen *g, Expr *e);

static int gen_try(Gen *g, Expr *e) {
    int v0 = gen_expr(g, e->a);
    Type *at = e->a->type;
    int vs = new_slot(g);
    emit_store_local(g, vs, v0);
    int out = new_slot(g);
    int bok = ir_new_block(g->f, "try.ok");
    int bbad = ir_new_block(g->f, "try.bad");

    if (at->kind == TY_RES) {
        int tag = emit_load_mem(g, v0, RES_TAG, 0, 8);
        emit_br(g, tag, bbad, bok);
        g->blk = bbad;
        int rv = emit_load_local(g, vs, 0);
        IrIns *r = g_ins(g, IR_RETV);
        r->a = rv;
        g->blk = bok;
        int base = emit_load_local(g, vs, 0);
        int val = emit_load_mem(g, base, RES_VAL, 0, 8);
        if (is_float_ty(e->type)) val = bitcast_i2f(g, val);
        emit_store_local(g, out, val);
    } else {
        emit_br(g, v0, bok, bbad);
        g->blk = bbad;
        if (e->idx == 2) {
            int m = gen_str_lit(g, "unexpected nil", 14);
            int zero = emit_const(g, 0);
            int a2[2] = { m, zero };
            int er = emit_call_n(g, "error_new", a2, 2);
            int p = emit_alloc(g, RES_SIZE, OKIND_SCAN, 0, 0);
            int one = emit_const(g, 1);
            emit_store_mem(g, p, RES_TAG, one, 8);
            emit_store_mem(g, p, RES_VAL, er, 8);
            IrIns *r = g_ins(g, IR_RETV);
            r->a = p;
        } else {
            int z = emit_const(g, 0);
            IrIns *r = g_ins(g, IR_RETV);
            r->a = z;
        }
        g->blk = bok;
        emit_store_local(g, out, unbox_opt(g, emit_load_local(g, vs, 0), at));
    }
    return emit_load_local(g, out, is_float_ty(e->type));
}

static int gen_orelse(Gen *g, Expr *e) {
    int v = gen_expr(g, e->a);
    Type *at = e->a->type;
    int vs = new_slot(g);
    emit_store_local(g, vs, v);
    int out = new_slot(g);
    int bok = ir_new_block(g->f, "or.ok");
    int bbad = ir_new_block(g->f, "or.else");
    int bend = ir_new_block(g->f, "or.end");

    if (at->kind == TY_RES) {
        int tag = emit_load_mem(g, v, RES_TAG, 0, 8);
        emit_br(g, tag, bbad, bok);
        g->blk = bok;
        int base = emit_load_local(g, vs, 0);
        int val = emit_load_mem(g, base, RES_VAL, 0, 8);
        if (is_float_ty(e->type)) val = bitcast_i2f(g, val);
        emit_store_local(g, out, val);
    } else {
        emit_br(g, v, bok, bbad);
        g->blk = bok;
        emit_store_local(g, out, unbox_opt(g, emit_load_local(g, vs, 0), at));
    }
    emit_jmp(g, bend);

    g->blk = bbad;
    if (e->b->kind == E_STMTEXPR) {
        gen_stmt(g, e->b->stmt);
    } else {
        int d = gen_expr(g, e->b);
        d = gen_coerce(g, d, e->b->type, e->type);
        emit_store_local(g, out, d);
        emit_jmp(g, bend);
    }
    g->blk = bend;
    return emit_load_local(g, out, is_float_ty(e->type));
}

/* ---- pattern matching ---- */

/* Test the value held in frame slot `vs` (of type `t`) against `pat`; jump to
   `fail` when it does not match, otherwise fall through with the bindings
   established. The subject travels in a frame slot because a virtual register
   may not cross a block boundary. */
static void gen_pattern(Gen *g, Pattern *p, int vs, Type *t, int fail) {
    switch (p->kind) {
        case P_WILD: return;
        case P_BIND:
            gen_bind(g, (Sym *)p->sym, emit_load_local(g, vs, is_float_ty(t)));
            return;
        case P_INT: case P_CHAR: case P_BOOL: {
            int v = emit_load_local(g, vs, 0);
            int c = emit_const(g, p->ival);
            IrIns *i = g_ins(g, IR_EQ);
            i->dst = new_vreg(g, 0); i->a = v; i->b = c;
            int nxt = ir_new_block(g->f, "pat.ok");
            emit_br(g, i->dst, nxt, fail);
            g->blk = nxt;
            return;
        }
        case P_FLOAT: {
            int v = emit_load_local(g, vs, 1);
            int c = emit_constf(g, p->fval);
            IrIns *i = g_ins(g, IR_FEQ);
            i->dst = new_vreg(g, 0); i->a = v; i->b = c;
            int nxt = ir_new_block(g->f, "pat.ok");
            emit_br(g, i->dst, nxt, fail);
            g->blk = nxt;
            return;
        }
        case P_STR: {
            int v = emit_load_local(g, vs, 0);
            int lit = gen_str_lit(g, p->sval, (int)p->ival);
            int args[2] = { v, lit };
            int r = emit_call_n(g, "str_eq", args, 2);
            int nxt = ir_new_block(g->f, "pat.ok");
            emit_br(g, r, nxt, fail);
            g->blk = nxt;
            return;
        }
        case P_NIL: {
            int v = emit_load_local(g, vs, 0);
            int nxt = ir_new_block(g->f, "pat.ok");
            emit_br(g, v, fail, nxt);
            g->blk = nxt;
            return;
        }
        case P_SOME: {
            int v = emit_load_local(g, vs, 0);
            int nxt = ir_new_block(g->f, "pat.ok");
            emit_br(g, v, nxt, fail);
            g->blk = nxt;
            if (p->subs.len == 1) {
                int inner = unbox_opt(g, emit_load_local(g, vs, 0), t);
                int s2 = new_slot(g);
                emit_store_local(g, s2, inner);
                gen_pattern(g, VEC_AT(&p->subs, Pattern, 0), s2, t->elem, fail);
            }
            return;
        }
        case P_OK: case P_ERR: {
            int v = emit_load_local(g, vs, 0);
            int tag = emit_load_mem(g, v, RES_TAG, 0, 8);
            int nxt = ir_new_block(g->f, "pat.ok");
            if (p->kind == P_OK) emit_br(g, tag, fail, nxt);
            else emit_br(g, tag, nxt, fail);
            g->blk = nxt;
            if (p->subs.len == 1) {
                Type *it = p->kind == P_OK ? t->elem : ty_error;
                int base = emit_load_local(g, vs, 0);
                int val = emit_load_mem(g, base, RES_VAL, 0, 8);
                if (is_float_ty(it)) val = bitcast_i2f(g, val);
                int s2 = new_slot(g);
                emit_store_local(g, s2, val);
                gen_pattern(g, VEC_AT(&p->subs, Pattern, 0), s2, it, fail);
            }
            return;
        }
        case P_ENUM: {
            int v = emit_load_local(g, vs, 0);
            int tag = t->is_prim ? v : emit_load_mem(g, v, 6, 0, 2);
            int c = emit_const(g, p->tag);
            IrIns *i = g_ins(g, IR_EQ);
            i->dst = new_vreg(g, 0); i->a = tag; i->b = c;
            int nxt = ir_new_block(g->f, "pat.ok");
            emit_br(g, i->dst, nxt, fail);
            g->blk = nxt;
            for (int k = 0; k < p->subs.len; k++) {
                Type *pt = enum_payload_type(t, p->tag, k);
                int base = emit_load_local(g, vs, 0);
                int fv = emit_load_mem(g, base, HDR + k * 8, 0, 8);
                if (is_float_ty(pt)) fv = bitcast_i2f(g, fv);
                int s2 = new_slot(g);
                emit_store_local(g, s2, fv);
                gen_pattern(g, VEC_AT(&p->subs, Pattern, k), s2, pt, fail);
            }
            return;
        }
        case P_STRUCT: {
            for (int k = 0; k < p->subs.len; k++) {
                Pattern *sub = VEC_AT(&p->subs, Pattern, k);
                Type *ft = struct_field_type(t, sub->tag);
                int base = emit_load_local(g, vs, 0);
                int fv = emit_load_mem(g, base, HDR + sub->tag * 8, 0, 8);
                if (is_float_ty(ft)) fv = bitcast_i2f(g, fv);
                int s2 = new_slot(g);
                emit_store_local(g, s2, fv);
                gen_pattern(g, sub, s2, ft, fail);
            }
            return;
        }
        case P_OR: {
            int succ = ir_new_block(g->f, "or.succ");
            for (int k = 0; k < p->subs.len; k++) {
                int nextalt = (k + 1 < p->subs.len) ? ir_new_block(g->f, "or.alt") : fail;
                gen_pattern(g, VEC_AT(&p->subs, Pattern, k), vs, t, nextalt);
                emit_jmp(g, succ);
                if (k + 1 < p->subs.len) g->blk = nextalt;
            }
            g->blk = succ;
            return;
        }
    }
}

static int gen_match(Gen *g, Expr *e, int as_expr) {
    int subj = gen_expr(g, e->a);
    Type *st = e->a->type;
    int subj_slot = new_slot(g);
    emit_store_local(g, subj_slot, subj);
    int out = as_expr ? new_slot(g) : -1;
    int bend = ir_new_block(g->f, "match.end");

    for (int i = 0; i < e->list.len; i++) {
        MatchArm *a = VEC_AT(&e->list, MatchArm, i);
        int bfail = ir_new_block(g->f, "arm.fail");
        gen_pattern(g, a->pat, subj_slot, st, bfail);
        if (a->guard) {
            int gv = gen_expr(g, a->guard);
            int bok = ir_new_block(g->f, "guard.ok");
            emit_br(g, gv, bok, bfail);
            g->blk = bok;
        }
        if (a->body) {
            int r = gen_expr(g, a->body);
            if (as_expr) {
                r = gen_coerce(g, r, a->body->type, e->type);
                emit_store_local(g, out, r);
            }
        } else {
            gen_block(g, a->block);
        }
        emit_jmp(g, bend);
        g->blk = bfail;
    }
    if (as_expr) {
        int m = gen_str_lit(g, "no match arm applied", 20);
        int a1[1] = { m };
        emit_call_n(g, "panic_str", a1, 1);
        g_ins(g, IR_TRAP);
    }
    emit_jmp(g, bend);
    g->blk = bend;
    return as_expr ? emit_load_local(g, out, is_float_ty(e->type)) : -1;
}

/* ---- calls ---- */

static int gen_call(Gen *g, Expr *e) {
    if (e->builtin > 0) return gen_builtin(g, e);
    FnInst *target = (FnInst *)e->target;
    Expr *callee = (e->kind == E_CALL) ? e->a : NULL;
    Expr *recv = (e->kind == E_METHOD && target && target->decl && target->decl->has_self)
                 ? e->a : NULL;
    if (target) {
        int n = e->list.len + (recv ? 1 : 0);
        int *vals = NEWN(int, n > 0 ? n : 1);
        int k = 0;
        if (recv) vals[k++] = gen_expr(g, recv);
        for (int i = 0; i < e->list.len; i++) {
            Expr *a = VEC_AT(&e->list, Expr, i);
            Type *pt = (k < target->param_types.len)
                       ? VEC_AT(&target->param_types, Type, k) : a->type;
            int v = gen_expr(g, a);
            vals[k++] = gen_coerce(g, v, a->type, pt);
        }
        return emit_call(g, target, vals, n, is_float_ty(e->type));
    }
    /* indirect call through a closure value */
    Expr *ce = callee ? callee : e->a;
    int cl = gen_expr(g, ce);
    int cslot = new_slot(g);
    emit_store_local(g, cslot, cl);
    Type *ft = ce->type;
    int nargs = e->list.len;
    int *vals = NEWN(int, nargs > 0 ? nargs : 1);
    for (int i = 0; i < nargs; i++) {
        Expr *a = VEC_AT(&e->list, Expr, i);
        Type *pt = (ft && ft->kind == TY_FN && i < ft->params.len)
                   ? VEC_AT(&ft->params, Type, i) : a->type;
        int v = gen_expr(g, a);
        vals[i] = gen_coerce(g, v, a->type, pt);
    }
    int env = emit_load_local(g, cslot, 0);
    int code = emit_load_mem(g, env, CLOS_CODE, 0, 8);
    IrIns *i = g_ins(g, IR_CALL_IND);
    i->a = code;
    i->b = env;
    for (int k = 0; k < nargs; k++) vec_push(&i->args, (void *)(intptr_t)vals[k]);
    i->dst = new_vreg(g, is_float_ty(e->type));
    return i->dst;
}

static int gen_builtin(Gen *g, Expr *e) {
    Vec *args = &e->list;
    switch (e->builtin) {
        case BI_STR: return gen_to_str(g, VEC_AT(args, Expr, 0));
        case BI_LEN: {
            Expr *a = VEC_AT(args, Expr, 0);
            int v = gen_expr(g, a);
            if (e->idx == IDX_MAP) { int x[1] = { v }; return emit_call_n(g, "map_len", x, 1); }
            return emit_load_mem(g, v, OFF_AUX, 0, 8);
        }
        case BI_INT: {
            Expr *a = VEC_AT(args, Expr, 0);
            int v = gen_expr(g, a);
            if (a->type->kind == TY_FLOAT) {
                IrIns *i = g_ins(g, IR_F2I);
                i->dst = new_vreg(g, 0); i->a = v; i->imm = 0;
                return i->dst;
            }
            if (a->type->kind == TY_STR) { int x[1] = { v }; return emit_call_n(g, "str_to_int", x, 1); }
            return v;
        }
        case BI_FLOAT: {
            Expr *a = VEC_AT(args, Expr, 0);
            int v = gen_expr(g, a);
            if (a->type->kind == TY_FLOAT) return v;
            if (a->type->kind == TY_STR) { int x[1] = { v }; return emit_call_n(g, "str_to_float", x, 1); }
            IrIns *i = g_ins(g, IR_I2F);
            i->dst = new_vreg(g, 1); i->a = v; i->imm = 0;
            return i->dst;
        }
        case BI_BYTE: return mask_byte(g, gen_expr(g, VEC_AT(args, Expr, 0)));
        case BI_BOOL: {
            Expr *a = VEC_AT(args, Expr, 0);
            int v = gen_expr(g, a);
            if (a->type->kind == TY_BOOL) return v;
            int z = emit_const(g, 0);
            IrIns *i = g_ins(g, IR_NE);
            i->dst = new_vreg(g, 0); i->a = v; i->b = z;
            return i->dst;
        }
        case BI_PRINT: case BI_PRINTLN: {
            for (int i = 0; i < args->len; i++) {
                int s = gen_to_str(g, VEC_AT(args, Expr, i));
                int x[1] = { s };
                emit_call_n(g, "print_str", x, 1);
            }
            if (e->builtin == BI_PRINTLN) {
                int nl = gen_str_lit(g, "\n", 1);
                int x[1] = { nl };
                emit_call_n(g, "print_str", x, 1);
            }
            return emit_const(g, 0);
        }
        case BI_PANIC: {
            int s = gen_expr(g, VEC_AT(args, Expr, 0));
            int x[1] = { s };
            emit_call_n(g, "panic_str", x, 1);
            g_ins(g, IR_TRAP);
            return emit_const(g, 0);
        }
        case BI_ASSERT: {
            Expr *c = VEC_AT(args, Expr, 0);
            int v = gen_expr(g, c);
            int bok = ir_new_block(g->f, "assert.ok");
            int bbad = ir_new_block(g->f, "assert.bad");
            emit_br(g, v, bok, bbad);
            g->blk = bbad;
            int msg;
            if (args->len > 1) msg = gen_expr(g, VEC_AT(args, Expr, 1));
            else msg = gen_str_lit(g, "assertion failed", 16);
            SrcFile *sf = src_get(e->span.file);
            const char *disp = sf ? sf->display : "?";
            int file = gen_str_lit(g, disp, (int)strlen(disp));
            int line = emit_const(g, e->span.line);
            int x[3] = { msg, file, line };
            emit_call_n(g, "assert_fail", x, 3);
            g_ins(g, IR_TRAP);
            g->blk = bok;
            return emit_const(g, 0);
        }
        case BI_ASSERT_EQ: case BI_ASSERT_NE: {
            Expr *ae = VEC_AT(args, Expr, 0);
            Expr *be = VEC_AT(args, Expr, 1);
            Type *t = ae->type;
            int cmp;
            if (t->kind == TY_FLOAT) {
                int a = gen_expr(g, ae), b = gen_expr(g, be);
                IrIns *i = g_ins(g, IR_FEQ);
                i->dst = new_vreg(g, 0); i->a = a; i->b = b;
                cmp = i->dst;
            } else if (t->kind == TY_INT || t->kind == TY_BOOL || t->kind == TY_BYTE ||
                       (t->kind == TY_ENUM && t->is_prim)) {
                int a = gen_expr(g, ae), b = gen_expr(g, be);
                IrIns *i = g_ins(g, IR_EQ);
                i->dst = new_vreg(g, 0); i->a = a; i->b = b;
                cmp = i->dst;
            } else if (t->kind == TY_STR) {
                int a = gen_expr(g, ae), b = gen_expr(g, be);
                int x[2] = { a, b };
                cmp = emit_call_n(g, "str_eq", x, 2);
            } else {
                int a = gen_word(g, ae), b = gen_word(g, be);
                int d = gen_desc(g, t);
                int x[3] = { a, b, d };
                cmp = emit_call_n(g, "any_eq", x, 3);
            }
            int bok = ir_new_block(g->f, "aeq.ok");
            int bbad = ir_new_block(g->f, "aeq.bad");
            if (e->builtin == BI_ASSERT_EQ) emit_br(g, cmp, bok, bbad);
            else emit_br(g, cmp, bbad, bok);
            g->blk = bbad;
            ae->type = NULL; be->type = NULL;
            ae->type = t;
            int sa = gen_to_str(g, ae);
            int sb = gen_to_str(g, be);
            SrcFile *sf = src_get(e->span.file);
            const char *disp = sf ? sf->display : "?";
            int file = gen_str_lit(g, disp, (int)strlen(disp));
            int line = emit_const(g, e->span.line);
            int kind = emit_const(g, e->builtin == BI_ASSERT_EQ ? 0 : 1);
            int x[5] = { sa, sb, file, line, kind };
            emit_call_n(g, "assert_cmp_fail", x, 5);
            g_ins(g, IR_TRAP);
            g->blk = bok;
            return emit_const(g, 0);
        }
        case BI_OK: {
            int v = args->len ? gen_word(g, VEC_AT(args, Expr, 0)) : emit_const(g, 0);
            int p = emit_alloc(g, RES_SIZE, OKIND_SCAN, 0, 0);
            int z = emit_const(g, 0);
            emit_store_mem(g, p, RES_TAG, z, 8);
            emit_store_mem(g, p, RES_VAL, v, 8);
            return p;
        }
        case BI_ERR: case BI_ERR_CODE: {
            int m = gen_expr(g, VEC_AT(args, Expr, 0));
            int c = (e->builtin == BI_ERR_CODE) ? gen_expr(g, VEC_AT(args, Expr, 1))
                                                : emit_const(g, 0);
            int a2[2] = { m, c };
            int er = emit_call_n(g, "error_new", a2, 2);
            int p = emit_alloc(g, RES_SIZE, OKIND_SCAN, 0, 0);
            int one = emit_const(g, 1);
            emit_store_mem(g, p, RES_TAG, one, 8);
            emit_store_mem(g, p, RES_VAL, er, 8);
            return p;
        }
    }
    return emit_const(g, 0);
}

static int gen_intrinsic(Gen *g, Expr *e) {
    const char *n = e->name;
    int nargs = e->list.len;
    int vals[8];
    int is_f2bits = (n == intern("f2bits"));
    for (int i = 0; i < nargs && i < 8; i++) {
        Expr *a = VEC_AT(&e->list, Expr, i);
        vals[i] = is_f2bits ? gen_expr(g, a) : gen_word(g, a);
    }

    if (n == intern("syscall")) {
        IrIns *i = g_ins(g, IR_SYSCALL);
        for (int k = 0; k < nargs && k < 7; k++) vec_push(&i->args, (void *)(intptr_t)vals[k]);
        i->dst = new_vreg(g, 0);
        return i->dst;
    }
    if (n == intern("load8") || n == intern("load16") ||
        n == intern("load32") || n == intern("load64")) {
        int sz = n == intern("load8") ? 1 : n == intern("load16") ? 2 :
                 n == intern("load32") ? 4 : 8;
        return emit_load_mem(g, vals[0], 0, 0, sz);
    }
    if (n == intern("store8") || n == intern("store16") ||
        n == intern("store32") || n == intern("store64")) {
        int sz = n == intern("store8") ? 1 : n == intern("store16") ? 2 :
                 n == intern("store32") ? 4 : 8;
        emit_store_mem(g, vals[0], 0, vals[1], sz);
        return emit_const(g, 0);
    }
    if (n == intern("addr")) return vals[0];
    if (n == intern("ref")) return vals[0];
    if (n == intern("sizeof")) return emit_const(g, 8);
    if (n == intern("rt_base")) {
        IrIns *i = g_ins(g, IR_STACK_TOP);
        i->dst = new_vreg(g, 0);
        i->imm = -1;
        return i->dst;
    }
    if (n == intern("stack_top") || n == intern("argc") ||
        n == intern("argv") || n == intern("envp")) {
        IrIns *i = g_ins(g, IR_STACK_TOP);
        i->dst = new_vreg(g, 0);
        i->imm = n == intern("stack_top") ? 0 : n == intern("argc") ? 1 :
                 n == intern("argv") ? 2 : 3;
        return i->dst;
    }
    if (n == intern("save_regs")) {
        IrIns *i = g_ins(g, IR_SAVE_REGS);
        i->dst = new_vreg(g, 0);
        return i->dst;
    }
    if (n == intern("restore_regs")) { g_ins(g, IR_RESTORE_REGS); return emit_const(g, 0); }
    if (n == intern("f2bits")) return bitcast_f2i(g, vals[0]);
    if (n == intern("bits2f")) return bitcast_i2f(g, vals[0]);
    if (n == intern("trap")) { g_ins(g, IR_TRAP); return emit_const(g, 0); }
    return emit_const(g, 0);
}

static int gen_const_sym(Gen *g, Sym *s, Type *t) {
    Decl *d = s->decl;
    if (d && d->cfold == 1) return emit_const(g, d->cfold_i);
    if (d && d->cfold == 2) return emit_constf(g, d->cfold_f);
    if (d && d->cfold == 3) return gen_str_lit(g, d->cfold_s, d->cfold_len);
    IrIns *i = g_ins(g, IR_GLOBAL_ADDR);
    i->dst = new_vreg(g, 0);
    i->target = s->slot;
    return emit_load_mem(g, i->dst, 0, is_float_ty(t), 8);
}

static int gen_expr(Gen *g, Expr *e) {
    if (!e) return emit_const(g, 0);
    switch (e->kind) {
        case E_INT: case E_CHAR: case E_BOOL: return emit_const(g, e->ival);
        case E_FLOAT: return emit_constf(g, e->fval);
        case E_NIL: return emit_const(g, 0);
        case E_STR: return gen_str_lit(g, e->sval, (int)e->ival);
        case E_INTERP: {
            int acc = -1;
            for (int i = 0; i < e->list.len; i++) {
                Expr *p = VEC_AT(&e->list, Expr, i);
                int s = gen_to_str(g, p);
                if (acc < 0) acc = s;
                else { int x[2] = { acc, s }; acc = emit_call_n(g, "str_concat", x, 2); }
            }
            if (acc < 0) acc = gen_str_lit(g, "", 0);
            return acc;
        }
        case E_IDENT: {
            Sym *s = (Sym *)e->sym;
            if (s && s->kind == SYM_CONST) return gen_const_sym(g, s, e->type);
            if (s && s->kind == SYM_FN) {
                IrIns *i = g_ins(g, IR_RODATA_ADDR);
                i->dst = new_vreg(g, 0);
                i->target = rodata_closure((FnInst *)e->target);
                return i->dst;
            }
            if (!s) return emit_const(g, 0);
            return gen_load_sym(g, s);
        }
        case E_UNARY: {
            int v = gen_expr(g, e->a);
            if (e->op == T_NOT) return logical_not(g, v);
            if (e->op == T_MINUS) {
                IrIns *i = g_ins(g, e->idx == OPC_FLOAT ? IR_FNEG : IR_NEG);
                i->dst = new_vreg(g, e->idx == OPC_FLOAT); i->a = v;
                return i->dst;
            }
            IrIns *i = g_ins(g, IR_NOT);
            i->dst = new_vreg(g, 0); i->a = v;
            if (e->type && e->type->kind == TY_BYTE) return mask_byte(g, i->dst);
            return i->dst;
        }
        case E_BINARY: return gen_binary(g, e);
        case E_LIST: return gen_list_lit(g, e);
        case E_MAP: return gen_map_lit(g, e);
        case E_STRUCT: return gen_struct_lit(g, e);
        case E_LAMBDA: return gen_lambda(g, e);
        case E_FIELD: {
            if (e->a->kind == E_IDENT && e->a->sym &&
                ((Sym *)e->a->sym)->kind == SYM_MOD) {
                if (e->target) {
                    IrIns *i = g_ins(g, IR_RODATA_ADDR);
                    i->dst = new_vreg(g, 0);
                    i->target = rodata_closure((FnInst *)e->target);
                    return i->dst;
                }
                Sym *cs = (Sym *)e->sym;
                if (cs) return gen_const_sym(g, cs, e->type);
                return emit_const(g, 0);
            }
            int base = gen_expr(g, e->a);
            int v = emit_load_mem(g, base, HDR + e->idx * 8, 0, 8);
            if (is_float_ty(e->type)) return bitcast_i2f(g, v);
            return v;
        }
        case E_METHOD: case E_CALL: return gen_call(g, e);
        case E_INDEX: return gen_index(g, e);
        case E_SLICE: {
            int a = gen_expr(g, e->a);
            int lo = e->b ? gen_expr(g, e->b) : emit_const(g, 0);
            int hi;
            if (e->c) {
                hi = gen_expr(g, e->c);
                if (e->inclusive) {
                    int one = emit_const(g, 1);
                    IrIns *i = g_ins(g, IR_ADD);
                    i->dst = new_vreg(g, 0); i->a = hi; i->b = one;
                    hi = i->dst;
                }
            } else hi = emit_const(g, -1);
            int x[3] = { a, lo, hi };
            return emit_call_n(g, e->idx == IDX_STR ? "str_slice" : "list_slice", x, 3);
        }
        case E_RANGE: {
            int lo = gen_expr(g, e->a);
            int hi = e->b ? gen_expr(g, e->b) : emit_const(g, 0);
            if (e->inclusive) {
                int one = emit_const(g, 1);
                IrIns *i = g_ins(g, IR_ADD);
                i->dst = new_vreg(g, 0); i->a = hi; i->b = one;
                hi = i->dst;
            }
            int los = new_slot(g), his = new_slot(g);
            emit_store_local(g, los, lo);
            emit_store_local(g, his, hi);
            int p = emit_alloc(g, HDR + 16, OKIND_ATOMIC, 0, 0);
            emit_store_mem(g, p, HDR, emit_load_local(g, los, 0), 8);
            emit_store_mem(g, p, HDR + 8, emit_load_local(g, his, 0), 8);
            return p;
        }
        case E_TRY: return gen_try(g, e);
        case E_ORELSE: return gen_orelse(g, e);
        case E_CAST: return gen_expr(g, e->a);
        case E_MATCH: return gen_match(g, e, 1);
        case E_INTRINSIC: return gen_intrinsic(g, e);
        case E_STMTEXPR: gen_stmt(g, e->stmt); return emit_const(g, 0);
        default: return emit_const(g, 0);
    }
}

/* ------------------------------------------------------------------ */
/* statement lowering                                                   */
/* ------------------------------------------------------------------ */

static int compound_combine(Gen *g, int op, int cls, int cur, int rhs, Type *t, Span sp) {
    if (cls == OPC_STR) { int x[2] = { cur, rhs }; return emit_call_n(g, "str_concat", x, 2); }
    if (cls == OPC_LIST) { int d = gen_desc(g, t); int x[3] = { cur, rhs, d };
                           return emit_call_n(g, "list_concat", x, 3); }
    int r = gen_binop_ir(g, op, cls, cur, rhs, sp);
    if (cls == OPC_BYTE) return mask_byte(g, r);
    return r;
}

static void gen_assign(Gen *g, Stmt *s) {
    Expr *lhs = s->a;
    int op = s->op;
    int cls = (int)(intptr_t)s->sym2;

    if (op != T_EQ) {
        if (lhs->kind == E_IDENT) {
            Sym *sy = (Sym *)lhs->sym;
            int cur = gen_load_sym(g, sy);
            int r = gen_expr(g, s->b);
            gen_store_sym(g, sy, compound_combine(g, op, cls, cur, r, sy->type, s->span));
            return;
        }
        if (lhs->kind == E_FIELD) {
            int base = gen_expr(g, lhs->a);
            int bslot = new_slot(g);
            emit_store_local(g, bslot, base);
            int cur = emit_load_mem(g, base, HDR + lhs->idx * 8, 0, 8);
            if (is_float_ty(lhs->type)) cur = bitcast_i2f(g, cur);
            int r = gen_expr(g, s->b);
            int nv = compound_combine(g, op, cls, cur, r, lhs->type, s->span);
            if (is_float_ty(lhs->type)) nv = bitcast_f2i(g, nv);
            emit_store_mem(g, emit_load_local(g, bslot, 0), HDR + lhs->idx * 8, nv, 8);
            return;
        }
        if (lhs->kind == E_INDEX && lhs->idx == IDX_LIST) {
            int lst = gen_expr(g, lhs->a);
            int idx = gen_expr(g, lhs->b);
            int ls = new_slot(g), is = new_slot(g);
            emit_store_local(g, ls, lst);
            emit_store_local(g, is, idx);
            int x[2] = { lst, idx };
            int cur = emit_call_n(g, "list_get", x, 2);
            if (is_float_ty(lhs->type)) cur = bitcast_i2f(g, cur);
            int r = gen_expr(g, s->b);
            int nv = compound_combine(g, op, cls, cur, r, lhs->type, s->span);
            if (is_float_ty(lhs->type)) nv = bitcast_f2i(g, nv);
            int y[3] = { emit_load_local(g, ls, 0), emit_load_local(g, is, 0), nv };
            emit_call_n(g, "list_set", y, 3);
            return;
        }
        if (lhs->kind == E_INDEX && lhs->idx == IDX_MAP) {
            Type *vt = lhs->a->type->val;
            int m = gen_expr(g, lhs->a);
            int k = gen_word(g, lhs->b);
            int x[2] = { m, k };
            int addr = emit_call_n(g, "map_find_or_panic", x, 2);
            int as = new_slot(g);
            emit_store_local(g, as, addr);
            int cur = emit_load_mem(g, addr, 0, 0, 8);
            if (is_float_ty(vt)) cur = bitcast_i2f(g, cur);
            int r = gen_expr(g, s->b);
            int nv = compound_combine(g, op, cls, cur, r, vt, s->span);
            if (is_float_ty(vt)) nv = bitcast_f2i(g, nv);
            emit_store_mem(g, emit_load_local(g, as, 0), 0, nv, 8);
            return;
        }
        return;
    }

    if (lhs->kind == E_IDENT) {
        Sym *sy = (Sym *)lhs->sym;
        int v = gen_expr(g, s->b);
        v = gen_coerce(g, v, s->b->type, sy->type);
        gen_store_sym(g, sy, v);
        return;
    }
    if (lhs->kind == E_FIELD) {
        int base = gen_expr(g, lhs->a);
        int bslot = new_slot(g);
        emit_store_local(g, bslot, base);
        int v = gen_expr(g, s->b);
        v = gen_coerce(g, v, s->b->type, lhs->type);
        if (is_float_ty(lhs->type)) v = bitcast_f2i(g, v);
        emit_store_mem(g, emit_load_local(g, bslot, 0), HDR + lhs->idx * 8, v, 8);
        return;
    }
    if (lhs->kind == E_INDEX) {
        if (lhs->idx == IDX_LIST) {
            int lst = gen_expr(g, lhs->a);
            int idx = gen_expr(g, lhs->b);
            int ls = new_slot(g), is = new_slot(g);
            emit_store_local(g, ls, lst);
            emit_store_local(g, is, idx);
            int v = gen_expr(g, s->b);
            v = gen_coerce(g, v, s->b->type, lhs->type);
            if (is_float_ty(lhs->type)) v = bitcast_f2i(g, v);
            int x[3] = { emit_load_local(g, ls, 0), emit_load_local(g, is, 0), v };
            emit_call_n(g, "list_set", x, 3);
            return;
        }
        if (lhs->idx == IDX_MAP) {
            Type *vt = lhs->a->type->val;
            int m = gen_expr(g, lhs->a);
            int ms = new_slot(g);
            emit_store_local(g, ms, m);
            int k = gen_word(g, lhs->b);
            int ks = new_slot(g);
            emit_store_local(g, ks, k);
            int v = gen_expr(g, s->b);
            v = gen_coerce(g, v, s->b->type, vt);
            if (is_float_ty(vt)) v = bitcast_f2i(g, v);
            int x[3] = { emit_load_local(g, ms, 0), emit_load_local(g, ks, 0), v };
            emit_call_n(g, "map_set", x, 3);
            return;
        }
    }
}

static void gen_for(Gen *g, Stmt *s) {
    Sym *var = (Sym *)s->sym;
    Sym *var2 = (Sym *)s->sym2;
    int bcond = ir_new_block(g->f, "for.cond");
    int bbody = ir_new_block(g->f, "for.body");
    int bstep = ir_new_block(g->f, "for.step");
    int bend = ir_new_block(g->f, "for.end");

    if (s->op == 0) {                                  /* range */
        int r = gen_expr(g, s->a);
        int islot = new_slot(g), hslot = new_slot(g);
        emit_store_local(g, islot, emit_load_mem(g, r, HDR, 0, 8));
        emit_store_local(g, hslot, emit_load_mem(g, r, HDR + 8, 0, 8));
        emit_jmp(g, bcond);
        g->blk = bcond;
        int i = emit_load_local(g, islot, 0);
        int h = emit_load_local(g, hslot, 0);
        int c = gen_binop_ir(g, T_LT, OPC_INT, i, h, s->span);
        emit_br(g, c, bbody, bend);
        g->blk = bbody;
        gen_bind(g, var, emit_load_local(g, islot, 0));
        vec_push(&g->break_stack, (void *)(intptr_t)bend);
        vec_push(&g->cont_stack, (void *)(intptr_t)bstep);
        gen_block(g, s->then_s);
        g->break_stack.len--; g->cont_stack.len--;
        emit_jmp(g, bstep);
        g->blk = bstep;
        int one = emit_const(g, 1);
        int ni = gen_binop_ir(g, T_PLUS, OPC_INT, emit_load_local(g, islot, 0), one, s->span);
        emit_store_local(g, islot, ni);
        emit_jmp(g, bcond);
        g->blk = bend;
        return;
    }

    if (s->op == 1 || s->op == 3 || s->op == 4 || s->op == 5) {  /* list / string */
        int with_index = (s->op == 4 || s->op == 5);
        int is_str = (s->op == 3 || s->op == 5);
        int lst = gen_expr(g, s->a);
        int lslot = new_slot(g), islot = new_slot(g);
        emit_store_local(g, lslot, lst);
        emit_store_local(g, islot, emit_const(g, 0));
        emit_jmp(g, bcond);
        g->blk = bcond;
        int l = emit_load_local(g, lslot, 0);
        int n = emit_load_mem(g, l, OFF_AUX, 0, 8);
        int i = emit_load_local(g, islot, 0);
        int c = gen_binop_ir(g, T_LT, OPC_INT, i, n, s->span);
        emit_br(g, c, bbody, bend);
        g->blk = bbody;
        int x[2] = { emit_load_local(g, lslot, 0), emit_load_local(g, islot, 0) };
        int v = emit_call_n(g, is_str ? "str_index" : "list_get", x, 2);
        Sym *valsym = with_index ? var2 : var;
        if (with_index) gen_bind(g, var, emit_load_local(g, islot, 0));
        if (valsym) {
            if (is_float_ty(valsym->type)) v = bitcast_i2f(g, v);
            gen_bind(g, valsym, v);
        }
        vec_push(&g->break_stack, (void *)(intptr_t)bend);
        vec_push(&g->cont_stack, (void *)(intptr_t)bstep);
        gen_block(g, s->then_s);
        g->break_stack.len--; g->cont_stack.len--;
        emit_jmp(g, bstep);
        g->blk = bstep;
        int one = emit_const(g, 1);
        int ni = gen_binop_ir(g, T_PLUS, OPC_INT, emit_load_local(g, islot, 0), one, s->span);
        emit_store_local(g, islot, ni);
        emit_jmp(g, bcond);
        g->blk = bend;
        return;
    }

    /* map */
    int m = gen_expr(g, s->a);
    int mslot = new_slot(g), islot = new_slot(g);
    emit_store_local(g, mslot, m);
    emit_store_local(g, islot, emit_const(g, 0));
    emit_jmp(g, bcond);
    g->blk = bcond;
    int y[2] = { emit_load_local(g, mslot, 0), emit_load_local(g, islot, 0) };
    int nxt = emit_call_n(g, "map_next", y, 2);
    emit_store_local(g, islot, nxt);
    int zero = emit_const(g, 0);
    int c = gen_binop_ir(g, T_GE, OPC_INT, nxt, zero, s->span);
    emit_br(g, c, bbody, bend);
    g->blk = bbody;
    int z[2] = { emit_load_local(g, mslot, 0), emit_load_local(g, islot, 0) };
    int k = emit_call_n(g, "map_key_at", z, 2);
    if (is_float_ty(var->type)) k = bitcast_i2f(g, k);
    gen_bind(g, var, k);
    if (var2) {
        int w[2] = { emit_load_local(g, mslot, 0), emit_load_local(g, islot, 0) };
        int v = emit_call_n(g, "map_val_at", w, 2);
        if (is_float_ty(var2->type)) v = bitcast_i2f(g, v);
        gen_bind(g, var2, v);
    }
    vec_push(&g->break_stack, (void *)(intptr_t)bend);
    vec_push(&g->cont_stack, (void *)(intptr_t)bstep);
    gen_block(g, s->then_s);
    g->break_stack.len--; g->cont_stack.len--;
    emit_jmp(g, bstep);
    g->blk = bstep;
    int one = emit_const(g, 1);
    int ni = gen_binop_ir(g, T_PLUS, OPC_INT, emit_load_local(g, islot, 0), one, s->span);
    emit_store_local(g, islot, ni);
    emit_jmp(g, bcond);
    g->blk = bend;
}

static void gen_stmt(Gen *g, Stmt *s) {
    if (!s) return;
    switch (s->kind) {
        case S_LET: {
            Sym *sy = (Sym *)s->sym;
            int v = gen_expr(g, s->a);
            v = gen_coerce(g, v, s->a->type, sy->type);
            gen_bind(g, sy, v);
            break;
        }
        case S_ASSIGN: gen_assign(g, s); break;
        case S_EXPR: gen_expr(g, s->a); break;
        case S_RETURN: {
            if (s->a) {
                int v = gen_expr(g, s->a);
                v = gen_coerce(g, v, s->a->type, g->f->ret);
                IrIns *i = g_ins(g, IR_RETV);
                i->a = v;
                i->is_float = is_float_ty(g->f->ret);
            } else g_ins(g, IR_RET);
            break;
        }
        case S_IF: {
            int c = gen_expr(g, s->a);
            int bt = ir_new_block(g->f, "if.then");
            int be = s->else_s ? ir_new_block(g->f, "if.else") : 0;
            int bend = ir_new_block(g->f, "if.end");
            emit_br(g, c, bt, s->else_s ? be : bend);
            g->blk = bt;
            gen_block(g, s->then_s);
            emit_jmp(g, bend);
            if (s->else_s) {
                g->blk = be;
                if (s->else_s->kind == S_BLOCK) gen_block(g, s->else_s);
                else gen_stmt(g, s->else_s);
                emit_jmp(g, bend);
            }
            g->blk = bend;
            break;
        }
        case S_IFLET: {
            int v = gen_expr(g, s->a);
            int vs = new_slot(g);
            emit_store_local(g, vs, v);
            Sym *sy = (Sym *)s->sym;
            int bt = ir_new_block(g->f, "iflet.then");
            int be = s->else_s ? ir_new_block(g->f, "iflet.else") : 0;
            int bend = ir_new_block(g->f, "iflet.end");
            if (s->op == 1) {
                int tag = emit_load_mem(g, v, RES_TAG, 0, 8);
                emit_br(g, tag, s->else_s ? be : bend, bt);
                g->blk = bt;
                int base = emit_load_local(g, vs, 0);
                int val = emit_load_mem(g, base, RES_VAL, 0, 8);
                if (is_float_ty(sy->type)) val = bitcast_i2f(g, val);
                gen_bind(g, sy, val);
            } else {
                emit_br(g, v, bt, s->else_s ? be : bend);
                g->blk = bt;
                gen_bind(g, sy, unbox_opt(g, emit_load_local(g, vs, 0), s->a->type));
            }
            gen_block(g, s->then_s);
            emit_jmp(g, bend);
            if (s->else_s) {
                g->blk = be;
                if (s->else_s->kind == S_BLOCK) gen_block(g, s->else_s);
                else gen_stmt(g, s->else_s);
                emit_jmp(g, bend);
            }
            g->blk = bend;
            break;
        }
        case S_WHILE: {
            int bcond = ir_new_block(g->f, "while.cond");
            int bbody = ir_new_block(g->f, "while.body");
            int bend = ir_new_block(g->f, "while.end");
            emit_jmp(g, bcond);
            g->blk = bcond;
            int c = gen_expr(g, s->a);
            emit_br(g, c, bbody, bend);
            g->blk = bbody;
            vec_push(&g->break_stack, (void *)(intptr_t)bend);
            vec_push(&g->cont_stack, (void *)(intptr_t)bcond);
            gen_block(g, s->then_s);
            g->break_stack.len--; g->cont_stack.len--;
            emit_jmp(g, bcond);
            g->blk = bend;
            break;
        }
        case S_FOR: gen_for(g, s); break;
        case S_BLOCK: gen_block(g, s); break;
        case S_BREAK:
            if (g->break_stack.len)
                emit_jmp(g, (int)(intptr_t)g->break_stack.data[g->break_stack.len - 1]);
            break;
        case S_CONTINUE:
            if (g->cont_stack.len)
                emit_jmp(g, (int)(intptr_t)g->cont_stack.data[g->cont_stack.len - 1]);
            break;
        default: break;
    }
}

static void gen_block(Gen *g, Stmt *s) {
    if (!s) return;
    for (int i = 0; i < s->list.len; i++) gen_stmt(g, VEC_AT(&s->list, Stmt, i));
}

/* ------------------------------------------------------------------ */
/* driver                                                               */
/* ------------------------------------------------------------------ */

static void gen_fn(FnInst *fi) {
    if (!fi->decl || !fi->decl->body) return;
    Gen g;
    memset(&g, 0, sizeof g);
    g.f = fi;
    ir_new_block(fi, "entry");
    g.blk = 0;

    for (int i = 0; i < fi->param_syms.len; i++) {
        Sym *s = VEC_AT(&fi->param_syms, Sym, i);
        if (s && s->boxed) {
            int v = emit_load_local(&g, s->slot, is_float_ty(s->type));
            gen_make_box(&g, s, v);
        }
    }
    gen_block(&g, fi->decl->body);
    if (!blk_terminated(fi, g.blk)) {
        if (fi->ret && fi->ret->kind != TY_VOID) {
            int z = emit_const(&g, 0);
            IrIns *i = ir_emit(fi, g.blk, IR_RETV);
            i->a = z;
            i->is_float = is_float_ty(fi->ret);
        } else ir_emit(fi, g.blk, IR_RET);
    }
}

/* the synthetic `$init` function that evaluates module-level consts */
static FnInst *build_init(Unit *u) {
    FnInst *fi = NEW(FnInst);
    fi->name = intern("$init");
    fi->ret = ty_void;
    fi->index = u->fns.len;
    fi->nslots = 1;
    fi->reached = 1;
    vec_push(&u->fns, fi);

    Gen g;
    memset(&g, 0, sizeof g);
    g.f = fi;
    ir_new_block(fi, "entry");
    g.blk = 0;
    for (int i = 0; i < u->globals.len; i++) {
        Sym *s = VEC_AT(&u->globals, Sym, i);
        if (!s->decl || !s->decl->value) continue;
        if (s->decl->cfold) continue;             /* inlined at every use */
        int v = gen_expr(&g, s->decl->value);
        v = gen_coerce(&g, v, s->decl->value->type, s->type);
        if (is_float_ty(s->type)) v = bitcast_f2i(&g, v);
        IrIns *ga = g_ins(&g, IR_GLOBAL_ADDR);
        ga->dst = new_vreg(&g, 0);
        ga->target = s->slot;
        emit_store_mem(&g, ga->dst, 0, v, 8);
    }
    /* tell the collector how many globals it must scan */
    {
        IrIns *rb = g_ins(&g, IR_STACK_TOP);
        rb->dst = new_vreg(&g, 0);
        rb->imm = -1;
        int cnt = emit_const(&g, u->globals.len);
        IrIns *st = g_ins(&g, IR_STORE_MEM);
        st->a = rb->dst; st->b = cnt; st->imm = 360; st->size = 8;
    }
    ir_emit(fi, g.blk, IR_RET);
    return fi;
}

/* the `$testmain` entry point for `vela test` */
FnInst *build_test_main(Unit *u) {
    FnInst *fi = NEW(FnInst);
    fi->name = intern("$testmain");
    fi->ret = ty_int;
    fi->index = u->fns.len;
    fi->nslots = 1;
    fi->reached = 1;
    vec_push(&u->fns, fi);

    Gen g;
    memset(&g, 0, sizeof g);
    g.f = fi;
    ir_new_block(fi, "entry");
    g.blk = 0;

    for (int i = 0; i < u->tests.len; i++) {
        FnInst *t = VEC_AT(&u->tests, FnInst, i);
        const char *nm = t->test_name ? t->test_name : "test";
        int s = gen_str_lit(&g, nm, (int)strlen(nm));
        int a1[1] = { s };
        emit_call_n(&g, "test_begin", a1, 1);
        emit_call(&g, t, NULL, 0, 0);
        emit_call_n(&g, "test_pass", NULL, 0);
    }
    int r = emit_call_n(&g, "test_report", NULL, 0);
    IrIns *ret = g_ins(&g, IR_RETV);
    ret->a = r;
    return fi;
}

int irgen_run(Unit *u) {
    for (int i = 0; i < u->globals.len; i++)
        VEC_AT(&u->globals, Sym, i)->slot = i;
    int n = u->fns.len;
    for (int i = 0; i < n; i++) gen_fn(VEC_AT(&u->fns, FnInst, i));
    for (int i = n; i < u->fns.len; i++) gen_fn(VEC_AT(&u->fns, FnInst, i));
    u->init_fn = build_init(u);
    return 1;
}
