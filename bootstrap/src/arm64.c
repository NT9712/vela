/* arm64.c — Vela's AArch64 backend.
 *
 * Same shape as the x86-64 backend: a per-block linear scan over the
 * callee-saved registers, then direct instruction encoding. AArch64 is
 * pleasant to emit — every instruction is exactly four bytes — but its
 * immediates are narrow, so most of the work here is materialising constants
 * and frame offsets that do not fit an instruction field.
 *
 * Register use:
 *   x0-x7    arguments and return value
 *   x9-x12   scratch, inside a single instruction sequence only
 *   x19-x26  virtual registers (callee-saved, so a call cannot clobber one,
 *            which is also what makes the conservative stack scan sound)
 *   x29      frame pointer      x30  link register      sp  stack pointer
 *   d0-d3    floating-point scratch; float values live in frame slots
 */
#include "vela.h"

enum { X0=0, X1=1, X2=2, X3=3, X4=4, X5=5, X6=6, X7=7, X8=8,
       X9=9, X10=10, X11=11, X12=12, X13=13,
       X19=19, X20=20, X21=21, X22=22, X23=23, X24=24, X25=25, X26=26, X27=27, X28=28,
       X29=29, X30=30, XZR=31, SP=31 };

static const int alloc_regs[] = { X19, X20, X21, X22, X23, X24, X25, X26 };
#define NALLOC 8

static const int arg_regs[] = { X0, X1, X2, X3, X4, X5, X6, X7 };
#define NARGREG 8

#define SCR0 X9
#define SCR1 X10
#define SCR2 X11
#define SCR3 X12

static Buf T;
static uint64_t TEXT_VADDR, RO_VADDR, DATA_VADDR;

typedef struct { int at; int kind; int target; int addend; } Reloc;
/* kind 0: bl to function `target`
   kind 1: 4-instruction movz/movk of a function address
   kind 2: 4-instruction movz/movk of rodata base + addend
   kind 3: 4-instruction movz/movk of data base + addend */
static Vec relocs;
static uint64_t *fn_addr;
static int nfns;

static void rel(int at, int kind, int target, int addend) {
    Reloc *r = NEW(Reloc);
    r->at = at; r->kind = kind; r->target = target; r->addend = addend;
    vec_push(&relocs, r);
}

/* ------------------------------------------------------------------ */
/* encoding                                                             */
/* ------------------------------------------------------------------ */

static void w(uint32_t insn) { buf_u32(&T, insn); }
static int here(void) { return (int)T.len; }
static void patch(int at, uint32_t insn) { memcpy(T.data + at, &insn, 4); }

static void addsub_imm(int sub, int setflags, int d, int n, uint32_t imm, int sh12);

/* data processing, register form: sf op S Rm shift Rn Rd */
static void addsub_reg(int sub, int setflags, int d, int n, int m) {
    w(0x8B000000u | ((uint32_t)sub << 30) | ((uint32_t)setflags << 29) |
      ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
static void add_rr(int d, int n, int m) { addsub_reg(0, 0, d, n, m); }
static void sub_rr(int d, int n, int m) { addsub_reg(1, 0, d, n, m); }
static void cmp_rr(int n, int m)        { addsub_reg(1, 1, XZR, n, m); }

/* add/sub immediate, 12-bit unsigned, optional shift by 12 */
static void addsub_imm(int sub, int setflags, int d, int n, uint32_t imm, int sh12) {
    w(0x91000000u | ((uint32_t)sub << 30) | ((uint32_t)setflags << 29) |
      ((uint32_t)sh12 << 22) | ((imm & 0xFFF) << 10) | ((uint32_t)n << 5) | (uint32_t)d);
}

static void logic_reg(uint32_t opc, int d, int n, int m) {
    w(0x8A000000u | (opc << 29) | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
static void and_rr(int d, int n, int m) { logic_reg(0, d, n, m); }
static void orr_rr(int d, int n, int m) { logic_reg(1, d, n, m); }
static void eor_rr(int d, int n, int m) { logic_reg(2, d, n, m); }

/* `orr Xd, XZR, Xm` cannot name the stack pointer: register 31 means XZR in
   the shifted-register forms and SP only in the add/sub-immediate forms, so
   moving to or from SP has to go through `add Xd, Xn, #0`. */
static void mov_rr(int d, int m) { if (d != m) orr_rr(d, XZR, m); }
static void mov_from_sp(int d) { addsub_imm(0, 0, d, SP, 0, 0); }
static void mov_to_sp(int m)   { addsub_imm(0, 0, SP, m, 0, 0); }

/* add Xd, Xn, Xm, LSL #sh */
static void add_shifted(int d, int n, int m, int sh) {
    w(0x8B000000u | ((uint32_t)sh << 10) | ((uint32_t)m << 16) |
      ((uint32_t)n << 5) | (uint32_t)d);
}

static void mul_rr(int d, int n, int m) {
    w(0x9B007C00u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
static void msub(int d, int n, int m, int a) {
    w(0x9B008000u | ((uint32_t)m << 16) | ((uint32_t)a << 10) | ((uint32_t)n << 5) | (uint32_t)d);
}
static void sdiv_rr(int d, int n, int m) {
    w(0x9AC00C00u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
static void shift_rr(uint32_t op2, int d, int n, int m) {
    w(0x9AC02000u | (op2 << 10) | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
static void lslv(int d, int n, int m) { shift_rr(0, d, n, m); }
static void asrv(int d, int n, int m) { shift_rr(2, d, n, m); }

static void mvn_r(int d, int m) { w(0xAA2003E0u | ((uint32_t)m << 16) | (uint32_t)d); }
static void neg_r(int d, int m) { addsub_reg(1, 0, d, XZR, m); }

static void movz(int d, uint32_t imm16, int shift) {
    w(0xD2800000u | ((uint32_t)(shift / 16) << 21) | ((imm16 & 0xFFFF) << 5) | (uint32_t)d);
}
static void movk(int d, uint32_t imm16, int shift) {
    w(0xF2800000u | ((uint32_t)(shift / 16) << 21) | ((imm16 & 0xFFFF) << 5) | (uint32_t)d);
}
static void movn(int d, uint32_t imm16, int shift) {
    w(0x92800000u | ((uint32_t)(shift / 16) << 21) | ((imm16 & 0xFFFF) << 5) | (uint32_t)d);
}

/* Materialise a 64-bit constant in as few instructions as the value allows. */
static void mov_imm(int d, int64_t v) {
    uint64_t u = (uint64_t)v;
    if (u == 0) { mov_rr(d, XZR); return; }
    /* count non-zero and all-ones halfwords to pick movz or movn */
    int nz = 0, no = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t h = (uint32_t)((u >> (i * 16)) & 0xFFFF);
        if (h != 0) nz++;
        if (h != 0xFFFF) no++;
    }
    if (nz <= no) {
        int first = 1;
        for (int i = 0; i < 4; i++) {
            uint32_t h = (uint32_t)((u >> (i * 16)) & 0xFFFF);
            if (h == 0) continue;
            if (first) { movz(d, h, i * 16); first = 0; }
            else movk(d, h, i * 16);
        }
    } else {
        uint64_t n = ~u;
        int first = 1;
        for (int i = 0; i < 4; i++) {
            uint32_t h = (uint32_t)((n >> (i * 16)) & 0xFFFF);
            if (first) {
                if (h == 0) continue;
                movn(d, h, i * 16); first = 0;
            } else {
                uint32_t hu = (uint32_t)((u >> (i * 16)) & 0xFFFF);
                if (hu != 0xFFFF) movk(d, hu, i * 16);
            }
        }
        if (first) movn(d, 0, 0);
    }
}

/* A fixed four-instruction constant load, so relocations can patch it later. */
static int mov_imm64_patch(int d) {
    int at = here();
    movz(d, 0, 0); movk(d, 0, 16); movk(d, 0, 32); movk(d, 0, 48);
    return at;
}
static void patch_imm64(int at, int d, uint64_t v) {
    patch(at + 0, 0xD2800000u | ((uint32_t)((v >>  0) & 0xFFFF) << 5) | (uint32_t)d);
    patch(at + 4, 0xF2A00000u | ((uint32_t)((v >> 16) & 0xFFFF) << 5) | (uint32_t)d);
    patch(at + 8, 0xF2C00000u | ((uint32_t)((v >> 32) & 0xFFFF) << 5) | (uint32_t)d);
    patch(at + 12,0xF2E00000u | ((uint32_t)((v >> 48) & 0xFFFF) << 5) | (uint32_t)d);
}

/* loads and stores: unsigned scaled offset when it fits, else unscaled, else
   compute the address in a scratch register */
static void ldst(int store, int size, int t, int base, int64_t off, int scratch) {
    uint32_t sz = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
    int scale = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
    uint32_t opc = store ? 0 : 1;
    if (off >= 0 && (off & ((1 << scale) - 1)) == 0 && (off >> scale) < 4096) {
        w((sz << 30) | 0x39000000u | (opc << 22) | ((uint32_t)(off >> scale) << 10) |
          ((uint32_t)base << 5) | (uint32_t)t);
        return;
    }
    if (off >= -256 && off <= 255) {
        w((sz << 30) | 0x38000000u | (opc << 22) |
          (((uint32_t)off & 0x1FF) << 12) | ((uint32_t)base << 5) | (uint32_t)t);
        return;
    }
    mov_imm(scratch, off);
    add_rr(scratch, base, scratch);
    w((sz << 30) | 0x39000000u | (opc << 22) | ((uint32_t)scratch << 5) | (uint32_t)t);
}
static void ldr64(int t, int base, int64_t off)  { ldst(0, 8, t, base, off, SCR3); }
static void str64(int t, int base, int64_t off)  { ldst(1, 8, t, base, off, SCR3); }
static void ldr_sz(int t, int base, int64_t off, int size) { ldst(0, size, t, base, off, SCR3); }
static void str_sz(int t, int base, int64_t off, int size) { ldst(1, size, t, base, off, SCR3); }

/* stp/ldp with pre/post index, used only by the prologue and epilogue */
static void stp_pre(int t1, int t2, int base, int off) {
    w(0xA9800000u | ((((uint32_t)(off / 8)) & 0x7F) << 15) |
      ((uint32_t)t2 << 10) | ((uint32_t)base << 5) | (uint32_t)t1);
}
static void ldp_post(int t1, int t2, int base, int off) {
    w(0xA8C00000u | ((((uint32_t)(off / 8)) & 0x7F) << 15) |
      ((uint32_t)t2 << 10) | ((uint32_t)base << 5) | (uint32_t)t1);
}

/* condition codes */
enum { CC_EQ=0, CC_NE=1, CC_HS=2, CC_LO=3, CC_MI=4, CC_PL=5, CC_VS=6, CC_VC=7,
       CC_HI=8, CC_LS=9, CC_GE=10, CC_LT=11, CC_GT=12, CC_LE=13, CC_AL=14 };

static void cset(int d, int cc) {
    w(0x9A9F07E0u | ((uint32_t)(cc ^ 1) << 12) | (uint32_t)d);
}
static void ret_(void) { w(0xD65F03C0u); }
static void svc0(void) { w(0xD4000001u); }
static void brk_(void) { w(0xD4200000u); }
static void blr_(int r) { w(0xD63F0000u | ((uint32_t)r << 5)); }

/* floating point, double precision */
static void fldr(int d, int base, int64_t off) {
    if (off >= 0 && (off & 7) == 0 && (off >> 3) < 4096) {
        w(0xFD400000u | ((uint32_t)(off >> 3) << 10) | ((uint32_t)base << 5) | (uint32_t)d);
        return;
    }
    if (off >= -256 && off <= 255) {
        w(0xFC400000u | (((uint32_t)off & 0x1FF) << 12) | ((uint32_t)base << 5) | (uint32_t)d);
        return;
    }
    mov_imm(SCR3, off);
    add_rr(SCR3, base, SCR3);
    w(0xFD400000u | ((uint32_t)SCR3 << 5) | (uint32_t)d);
}
static void fstr(int d, int base, int64_t off) {
    if (off >= 0 && (off & 7) == 0 && (off >> 3) < 4096) {
        w(0xFD000000u | ((uint32_t)(off >> 3) << 10) | ((uint32_t)base << 5) | (uint32_t)d);
        return;
    }
    if (off >= -256 && off <= 255) {
        w(0xFC000000u | (((uint32_t)off & 0x1FF) << 12) | ((uint32_t)base << 5) | (uint32_t)d);
        return;
    }
    mov_imm(SCR3, off);
    add_rr(SCR3, base, SCR3);
    w(0xFD000000u | ((uint32_t)SCR3 << 5) | (uint32_t)d);
}
/* FP data-processing, two sources: 0001 1110 ftype 1 Rm opcode 10 Rn Rd.
   ftype=01 selects double; bits 11:10 are fixed at 0b10. */
static void fop(uint32_t op, int d, int n, int m) {
    w(0x1E600800u | (op << 12) | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d);
}
static void fadd_d(int d, int n, int m) { fop(0x2, d, n, m); }
static void fsub_d(int d, int n, int m) { fop(0x3, d, n, m); }
static void fmul_d(int d, int n, int m) { fop(0x0, d, n, m); }
static void fdiv_d(int d, int n, int m) { fop(0x1, d, n, m); }
static void fsqrt_d(int d, int n) { w(0x1E61C000u | ((uint32_t)n << 5) | (uint32_t)d); }
static void fcmp_d(int n, int m) { w(0x1E602000u | ((uint32_t)m << 16) | ((uint32_t)n << 5)); }
static void scvtf_d(int d, int n) { w(0x9E620000u | ((uint32_t)n << 5) | (uint32_t)d); }
static void fcvtzs_x(int d, int n) { w(0x9E780000u | ((uint32_t)n << 5) | (uint32_t)d); }

/* ------------------------------------------------------------------ */
/* per-function code generation                                        */
/* ------------------------------------------------------------------ */

#define LOC_REG  0
#define LOC_SLOT 1

typedef struct {
    FnInst *f;
    int *kind, *where;
    int  nslots, nspill, framesize, used_mask, save_at;
    int *blk_off;
    Vec  jumpfix;
} FGen;

typedef struct { int at; int blk; int cond; } JFix;

static int cur_block_id;

static int64_t slot_off(FGen *fg, int slot) { return -8 * (int64_t)(slot + 1); }
static int64_t spill_off(FGen *fg, int idx)  { return -8 * (int64_t)(fg->nslots + idx + 1); }
static int64_t vloc_off(FGen *fg, int v)     { return spill_off(fg, fg->where[v]); }

static int getr(FGen *fg, int v, int scratch) {
    if (v < 0) { mov_rr(scratch, XZR); return scratch; }
    if (fg->kind[v] == LOC_REG) return fg->where[v];
    ldr64(scratch, X29, vloc_off(fg, v));
    return scratch;
}
static int dstr(FGen *fg, int v, int scratch) {
    if (v < 0) return scratch;
    if (fg->kind[v] == LOC_REG) return fg->where[v];
    return scratch;
}
static void putr(FGen *fg, int v, int r) {
    if (v < 0) return;
    if (fg->kind[v] == LOC_REG) { mov_rr(fg->where[v], r); return; }
    str64(r, X29, vloc_off(fg, v));
}
static int64_t foff(FGen *fg, int v) { return vloc_off(fg, v); }
static void fload(FGen *fg, int d, int v) { fldr(d, X29, foff(fg, v)); }
static void fstore(FGen *fg, int v, int d) { fstr(d, X29, foff(fg, v)); }

/* ---- register allocation (identical strategy to the x86-64 backend) ---- */

static void alloc_block(FGen *fg, IrBlock *b) {
    FnInst *f = fg->f;
    int n = b->ins.len;
    int *lastuse = NEWN(int, f->nvregs > 0 ? f->nvregs : 1);
    for (int i = 0; i < f->nvregs; i++) lastuse[i] = -1;
    for (int i = 0; i < n; i++) {
        IrIns *in = VEC_AT(&b->ins, IrIns, i);
        if (in->a >= 0) lastuse[in->a] = i;
        if (in->b >= 0) lastuse[in->b] = i;
        if (in->op == IR_LIST_SET && in->target2 >= 0) lastuse[in->target2] = i;
        for (int k = 0; k < in->args.len; k++) {
            int v = (int)(intptr_t)in->args.data[k];
            if (v >= 0) lastuse[v] = i;
        }
    }
    int busy[NALLOC], owner[NALLOC];
    for (int i = 0; i < NALLOC; i++) { busy[i] = 0; owner[i] = -1; }
    int spill_free = 0, spill_owner[512], spill_end[512];
    for (int i = 0; i < 512; i++) { spill_owner[i] = -1; spill_end[i] = -1; }

    for (int i = 0; i < n; i++) {
        for (int r = 0; r < NALLOC; r++)
            if (busy[r] && lastuse[owner[r]] < i) { busy[r] = 0; owner[r] = -1; }
        for (int s = 0; s < spill_free; s++)
            if (spill_owner[s] >= 0 && spill_end[s] < i) spill_owner[s] = -1;

        IrIns *in = VEC_AT(&b->ins, IrIns, i);
        if (in->dst < 0) continue;
        int v = in->dst;
        if (f->vreg_float && f->vreg_float[v]) {
            int s = -1;
            for (int k = 0; k < spill_free; k++) if (spill_owner[k] < 0) { s = k; break; }
            if (s < 0) { s = spill_free++; if (spill_free > 500) s = 499; }
            spill_owner[s] = v; spill_end[s] = lastuse[v] < 0 ? i : lastuse[v];
            fg->kind[v] = LOC_SLOT; fg->where[v] = s;
            continue;
        }
        int got = -1;
        for (int r = 0; r < NALLOC; r++) if (!busy[r]) { got = r; break; }
        if (got >= 0) {
            busy[got] = 1; owner[got] = v;
            fg->kind[v] = LOC_REG; fg->where[v] = alloc_regs[got];
            fg->used_mask |= 1 << alloc_regs[got];
        } else {
            int s = -1;
            for (int k = 0; k < spill_free; k++) if (spill_owner[k] < 0) { s = k; break; }
            if (s < 0) { s = spill_free++; if (spill_free > 500) s = 499; }
            spill_owner[s] = v; spill_end[s] = lastuse[v] < 0 ? i : lastuse[v];
            fg->kind[v] = LOC_SLOT; fg->where[v] = s;
        }
    }
    if (spill_free > fg->nspill) fg->nspill = spill_free;
}

/* ---- branches ---- */

static void jump_to(FGen *fg, int blk) {
    if (blk == cur_block_id + 1) return;
    JFix *jf = NEW(JFix);
    jf->at = here(); jf->blk = blk; jf->cond = -1;
    vec_push(&fg->jumpfix, jf);
    w(0x14000000u);
}
static void jcc_to(FGen *fg, int cc, int blk) {
    JFix *jf = NEW(JFix);
    jf->at = here(); jf->blk = blk; jf->cond = cc;
    vec_push(&fg->jumpfix, jf);
    w(0x54000000u | (uint32_t)cc);
}

static int cmp_cc(IrOp op) {
    switch (op) {
        case IR_EQ: return CC_EQ;
        case IR_NE: return CC_NE;
        case IR_LT: return CC_LT;
        case IR_LE: return CC_LE;
        case IR_GT: return CC_GT;
        case IR_GE: return CC_GE;
        default: return CC_EQ;
    }
}

static int fusable_cc(IrOp op) {
    switch (op) {
        case IR_EQ: return CC_EQ;
        case IR_NE: return CC_NE;
        case IR_LT: return CC_LT;
        case IR_LE: return CC_LE;
        case IR_GT: return CC_GT;
        case IR_GE: return CC_GE;
        /* fcmp sets the flags so that unordered fails every ordered test */
        case IR_FLT: return CC_MI;
        case IR_FLE: return CC_LS;
        case IR_FGT: return CC_GT;
        case IR_FGE: return CC_GE;
        default: return -1;
    }
}
static int invert_cc(int cc) { return cc ^ 1; }

static void emit_epilogue(FGen *fg) {
    int k = 0;
    for (int r = 0; r < 32; r++) {
        if (!(fg->used_mask & (1 << r))) continue;
        ldr64(r, X29, -8 * (int64_t)(fg->save_at + k + 1));
        k++;
    }
    mov_to_sp(X29);
    ldp_post(X29, X30, SP, 16);
    ret_();
}

static void emit_ins(FGen *fg, IrIns *in) {
    FnInst *f = fg->f;
    switch (in->op) {
        case IR_CONST: {
            int d = dstr(fg, in->dst, SCR0);
            mov_imm(d, in->imm);
            putr(fg, in->dst, d);
            break;
        }
        case IR_CONSTF: {
            uint64_t bits; double dv = in->fimm;
            memcpy(&bits, &dv, 8);
            mov_imm(SCR0, (int64_t)bits);
            str64(SCR0, X29, foff(fg, in->dst));
            break;
        }
        case IR_RODATA_ADDR: {
            int d = dstr(fg, in->dst, SCR0);
            rel(mov_imm64_patch(d), 2, d, in->target);
            putr(fg, in->dst, d);
            break;
        }
        case IR_GLOBAL_ADDR: {
            int d = dstr(fg, in->dst, SCR0);
            rel(mov_imm64_patch(d), 3, d, 512 + in->target * 8);
            putr(fg, in->dst, d);
            break;
        }
        case IR_FN_ADDR: {
            int d = dstr(fg, in->dst, SCR0);
            rel(mov_imm64_patch(d), 1, in->target, d);
            putr(fg, in->dst, d);
            break;
        }
        case IR_STACK_TOP: {
            int d = dstr(fg, in->dst, SCR0);
            if (in->imm < 0) {
                rel(mov_imm64_patch(d), 3, d, 0);
            } else {
                rel(mov_imm64_patch(SCR1), 3, SCR1, (int)in->imm * 8);
                ldr64(d, SCR1, 0);
            }
            putr(fg, in->dst, d);
            break;
        }
        case IR_LOAD_LOCAL: {
            if (f->vreg_float && f->vreg_float[in->dst]) {
                ldr64(SCR0, X29, slot_off(fg, in->target));
                str64(SCR0, X29, foff(fg, in->dst));
            } else {
                int d = dstr(fg, in->dst, SCR0);
                ldr64(d, X29, slot_off(fg, in->target));
                putr(fg, in->dst, d);
            }
            break;
        }
        case IR_STORE_LOCAL: {
            int s;
            if (f->vreg_float && in->a >= 0 && f->vreg_float[in->a]) {
                ldr64(SCR0, X29, foff(fg, in->a)); s = SCR0;
            } else s = getr(fg, in->a, SCR0);
            str64(s, X29, slot_off(fg, in->target));
            break;
        }
        case IR_LOAD_MEM: {
            int base = getr(fg, in->a, SCR1);
            if (f->vreg_float && f->vreg_float[in->dst]) {
                ldr_sz(SCR0, base, in->imm, in->size);
                str64(SCR0, X29, foff(fg, in->dst));
            } else {
                int d = dstr(fg, in->dst, SCR0);
                ldr_sz(d, base, in->imm, in->size);
                putr(fg, in->dst, d);
            }
            break;
        }
        case IR_STORE_MEM: {
            int base = getr(fg, in->a, SCR1);
            int s;
            if (f->vreg_float && in->b >= 0 && f->vreg_float[in->b]) {
                ldr64(SCR0, X29, foff(fg, in->b)); s = SCR0;
            } else s = getr(fg, in->b, SCR0);
            if (s == base) { mov_rr(SCR2, s); s = SCR2; }
            str_sz(s, base, in->imm, in->size);
            break;
        }
        case IR_MOV: putr(fg, in->dst, getr(fg, in->a, SCR0)); break;

        case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR:
        case IR_MUL: {
            int a = getr(fg, in->a, SCR0);
            int b = getr(fg, in->b, SCR1);
            int d = dstr(fg, in->dst, SCR2);
            switch (in->op) {
                case IR_ADD: add_rr(d, a, b); break;
                case IR_SUB: sub_rr(d, a, b); break;
                case IR_AND: and_rr(d, a, b); break;
                case IR_OR:  orr_rr(d, a, b); break;
                case IR_XOR: eor_rr(d, a, b); break;
                default:     mul_rr(d, a, b); break;
            }
            putr(fg, in->dst, d);
            break;
        }
        case IR_DIV: case IR_MOD: {
            int a = getr(fg, in->a, SCR0);
            int b = getr(fg, in->b, SCR1);
            if (b != SCR1) { mov_rr(SCR1, b); b = SCR1; }
            if (a != SCR0) { mov_rr(SCR0, a); a = SCR0; }
            /* trap on a zero divisor */
            cmp_rr(b, XZR);
            int skip = here();
            w(0x54000001u);                       /* b.ne over the trap */
            mov_rr(X0, b);
            int at = here(); rel(at, 0, -1000, 0); w(0x94000000u);
            brk_();
            patch(skip, 0x54000001u | ((uint32_t)(((here() - skip) / 4) & 0x7FFFF) << 5));
            int d = dstr(fg, in->dst, SCR2);
            sdiv_rr(SCR2, a, b);
            if (in->op == IR_MOD) msub(d, SCR2, b, a);
            else mov_rr(d, SCR2);
            putr(fg, in->dst, d);
            break;
        }
        case IR_SHL: case IR_SHR: case IR_SAR_HACK: {
            int a = getr(fg, in->a, SCR0);
            int b = getr(fg, in->b, SCR1);
            int d = dstr(fg, in->dst, SCR2);
            if (in->op == IR_SHL) lslv(d, a, b);
            else asrv(d, a, b);
            putr(fg, in->dst, d);
            break;
        }
        case IR_NEG: {
            int a = getr(fg, in->a, SCR0);
            int d = dstr(fg, in->dst, SCR1);
            neg_r(d, a);
            putr(fg, in->dst, d);
            break;
        }
        case IR_NOT: {
            int a = getr(fg, in->a, SCR0);
            int d = dstr(fg, in->dst, SCR1);
            mvn_r(d, a);
            putr(fg, in->dst, d);
            break;
        }
        case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE: {
            int a = getr(fg, in->a, SCR0);
            int b = getr(fg, in->b, SCR1);
            cmp_rr(a, b);
            int d = dstr(fg, in->dst, SCR2);
            cset(d, cmp_cc(in->op));
            putr(fg, in->dst, d);
            break;
        }
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: {
            fload(fg, 0, in->a);
            fload(fg, 1, in->b);
            switch (in->op) {
                case IR_FADD: fadd_d(0, 0, 1); break;
                case IR_FSUB: fsub_d(0, 0, 1); break;
                case IR_FMUL: fmul_d(0, 0, 1); break;
                default:      fdiv_d(0, 0, 1); break;
            }
            fstore(fg, in->dst, 0);
            break;
        }
        case IR_FSQRT: {
            fload(fg, 0, in->a);
            fsqrt_d(0, 0);
            fstore(fg, in->dst, 0);
            break;
        }
        case IR_FNEG: {
            ldr64(SCR0, X29, foff(fg, in->a));
            mov_imm(SCR1, (int64_t)0x8000000000000000ULL);
            eor_rr(SCR0, SCR0, SCR1);
            str64(SCR0, X29, foff(fg, in->dst));
            break;
        }
        case IR_FEQ: case IR_FNE: case IR_FLT: case IR_FLE: case IR_FGT: case IR_FGE: {
            fload(fg, 0, in->a);
            fload(fg, 1, in->b);
            fcmp_d(0, 1);
            int d = dstr(fg, in->dst, SCR2);
            int cc;
            switch (in->op) {
                case IR_FEQ: cc = CC_EQ; break;
                case IR_FNE: cc = CC_NE; break;
                case IR_FLT: cc = CC_MI; break;
                case IR_FLE: cc = CC_LS; break;
                case IR_FGT: cc = CC_GT; break;
                default:     cc = CC_GE; break;
            }
            cset(d, cc);
            putr(fg, in->dst, d);
            break;
        }
        case IR_I2F: {
            if (in->imm) {
                int a = getr(fg, in->a, SCR0);
                str64(a, X29, foff(fg, in->dst));
            } else {
                int a = getr(fg, in->a, SCR0);
                scvtf_d(0, a);
                fstore(fg, in->dst, 0);
            }
            break;
        }
        case IR_F2I: {
            if (in->imm) {
                ldr64(SCR0, X29, foff(fg, in->a));
                putr(fg, in->dst, SCR0);
            } else {
                fload(fg, 0, in->a);
                fcvtzs_x(SCR0, 0);
                putr(fg, in->dst, SCR0);
            }
            break;
        }
        case IR_CALL: case IR_CALL_IND: {
            int n = in->args.len;
            int total = n + 1;                       /* env plus the arguments */
            int nreg = total < NARGREG ? total : NARGREG;
            int nstack = total - nreg;
            int pad = (nstack & 1) ? 8 : 0;
            int frame = nstack * 8 + pad;
            if (frame) addsub_imm(1, 0, SP, SP, (uint32_t)frame, 0);
            for (int i = nreg - 1; i < n; i++) {
                int v = (int)(intptr_t)in->args.data[i];
                int r;
                if (f->vreg_float && v >= 0 && f->vreg_float[v]) {
                    ldr64(SCR0, X29, foff(fg, v)); r = SCR0;
                } else r = getr(fg, v, SCR0);
                str64(r, SP, (int64_t)(i - (nreg - 1)) * 8);
            }
            int fnreg = -1;
            if (in->op == IR_CALL_IND) {
                int code = getr(fg, in->a, SCR2);
                if (code != SCR2) mov_rr(SCR2, code);
                fnreg = SCR2;
            }
            for (int i = nreg - 2; i >= 0; i--) {
                int v = (int)(intptr_t)in->args.data[i];
                int r;
                if (f->vreg_float && v >= 0 && f->vreg_float[v]) {
                    ldr64(SCR0, X29, foff(fg, v)); r = SCR0;
                } else r = getr(fg, v, SCR0);
                mov_rr(arg_regs[i + 1], r);
            }
            if (in->op == IR_CALL_IND) {
                int env = getr(fg, in->b, SCR0);
                mov_rr(X0, env);
                blr_(fnreg);
            } else {
                mov_rr(X0, XZR);
                int at = here();
                rel(at, 0, in->target, 0);
                w(0x94000000u);
            }
            if (frame) addsub_imm(0, 0, SP, SP, (uint32_t)frame, 0);
            if (in->dst >= 0) {
                if (f->vreg_float && f->vreg_float[in->dst])
                    str64(X0, X29, foff(fg, in->dst));
                else putr(fg, in->dst, X0);
            }
            break;
        }
        case IR_SYSCALL: {
            /* x8 holds the number; x0-x5 hold the arguments */
            static const int sysregs[] = { X8, X0, X1, X2, X3, X4, X5 };
            int n = in->args.len;
            if (n > 7) n = 7;
            for (int i = n - 1; i >= 0; i--) {
                int v = (int)(intptr_t)in->args.data[i];
                int r = getr(fg, v, sysregs[i]);
                if (r != sysregs[i]) mov_rr(sysregs[i], r);
            }
            svc0();
            if (in->dst >= 0) putr(fg, in->dst, X0);
            break;
        }
        case IR_SAVE_REGS: {
            /* spill the callee-saved registers so the collector can see them */
            stp_pre(X19, X20, SP, -16);
            stp_pre(X21, X22, SP, -16);
            stp_pre(X23, X24, SP, -16);
            stp_pre(X25, X26, SP, -16);
            stp_pre(X27, X28, SP, -16);
            int d = dstr(fg, in->dst, SCR0);
            mov_from_sp(d);
            putr(fg, in->dst, d);
            break;
        }
        case IR_RESTORE_REGS:
            addsub_imm(0, 0, SP, SP, 80, 0);
            break;
        case IR_TRAP: brk_(); break;

        case IR_LIST_GET: case IR_STR_IDX: case IR_LIST_SET: {
            int base = getr(fg, in->a, SCR1);
            if (base != SCR1) { mov_rr(SCR1, base); base = SCR1; }
            int idx = getr(fg, in->b, SCR2);
            if (idx != SCR2) { mov_rr(SCR2, idx); idx = SCR2; }
            ldr64(SCR0, SCR1, 8);                    /* length */
            cmp_rr(SCR2, SCR0);
            int skip = here();
            w(0x54000003u);                          /* b.lo past the trap */
            mov_rr(X0, SCR2);
            int at = here(); rel(at, 0, -1001, 0); w(0x94000000u);
            brk_();
            patch(skip, 0x54000003u | ((uint32_t)(((here() - skip) / 4) & 0x7FFFF) << 5));
            if (in->op == IR_STR_IDX) {
                int d = dstr(fg, in->dst, SCR0);
                addsub_imm(0, 0, SCR0, SCR1, 16, 0);
                w(0x38606800u | ((uint32_t)SCR2 << 16) | ((uint32_t)SCR0 << 5) | (uint32_t)d);
                putr(fg, in->dst, d);
                break;
            }
            ldr64(SCR1, SCR1, 24);                   /* element block */
            addsub_imm(0, 0, SCR1, SCR1, 16, 0);
            if (in->op == IR_LIST_GET) {
                int d = dstr(fg, in->dst, SCR0);
                w(0xF8607800u | ((uint32_t)SCR2 << 16) | ((uint32_t)SCR1 << 5) | (uint32_t)d);
                putr(fg, in->dst, d);
            } else {
                int v = getr(fg, in->target2, SCR0);
                w(0xF8207800u | ((uint32_t)SCR2 << 16) | ((uint32_t)SCR1 << 5) | (uint32_t)v);
            }
            break;
        }
        case IR_JMP: jump_to(fg, in->target); break;
        case IR_BR: {
            int c = getr(fg, in->a, SCR0);
            cmp_rr(c, XZR);
            if (in->target2 == cur_block_id + 1) {
                jcc_to(fg, CC_NE, in->target);
            } else if (in->target == cur_block_id + 1) {
                jcc_to(fg, CC_EQ, in->target2);
            } else {
                jcc_to(fg, CC_NE, in->target);
                jump_to(fg, in->target2);
            }
            break;
        }
        case IR_RET: case IR_RETV: {
            if (in->op == IR_RETV) {
                if (f->vreg_float && in->a >= 0 && f->vreg_float[in->a])
                    ldr64(X0, X29, foff(fg, in->a));
                else { int s = getr(fg, in->a, X0); mov_rr(X0, s); }
            } else mov_rr(X0, XZR);
            emit_epilogue(fg);
            break;
        }
        default: break;
    }
}

static void emit_fused_branch(FGen *fg, IrIns *cmp, IrIns *br, int cc) {
    if (cmp->op >= IR_FEQ && cmp->op <= IR_FGE) {
        fload(fg, 0, cmp->a);
        fload(fg, 1, cmp->b);
        fcmp_d(0, 1);
    } else {
        int a = getr(fg, cmp->a, SCR0);
        int b = getr(fg, cmp->b, SCR1);
        cmp_rr(a, b);
    }
    if (br->target2 == cur_block_id + 1) {
        jcc_to(fg, cc, br->target);
    } else if (br->target == cur_block_id + 1) {
        jcc_to(fg, invert_cc(cc), br->target2);
    } else {
        jcc_to(fg, cc, br->target);
        jump_to(fg, br->target2);
    }
}

static void gen_function(FnInst *f) {
    if (!f->blocks.len) { f->code_off = -1; return; }
    FGen fg;
    memset(&fg, 0, sizeof fg);
    fg.f = f;
    int nv = f->nvregs > 0 ? f->nvregs : 1;
    fg.kind = NEWN(int, nv);
    fg.where = NEWN(int, nv);
    for (int i = 0; i < nv; i++) { fg.kind[i] = LOC_SLOT; fg.where[i] = 0; }
    fg.nslots = f->nslots;
    for (int i = 0; i < f->blocks.len; i++) alloc_block(&fg, VEC_AT(&f->blocks, IrBlock, i));

    fg.save_at = fg.nslots + fg.nspill;
    int nsaved = 0;
    for (int r = 0; r < 32; r++) if (fg.used_mask & (1 << r)) nsaved++;
    fg.framesize = 8 * (fg.nslots + fg.nspill + nsaved);
    fg.framesize = (fg.framesize + 15) & ~15;

    f->code_off = here();
    stp_pre(X29, X30, SP, -16);
    mov_from_sp(X29);
    if (fg.framesize) {
        if (fg.framesize < 4096) addsub_imm(1, 0, SP, SP, (uint32_t)fg.framesize, 0);
        else { mov_imm(SCR0, fg.framesize); sub_rr(SCR0, X29, SCR0); mov_to_sp(SCR0); }
    }
    int k = 0;
    for (int r = 0; r < 32; r++) {
        if (!(fg.used_mask & (1 << r))) continue;
        str64(r, X29, -8 * (int64_t)(fg.save_at + k + 1));
        k++;
    }
    {
        int nparams = f->nparams;
        int total = nparams + 1;
        int nreg = total < NARGREG ? total : NARGREG;
        for (int i = 0; i < nreg; i++) str64(arg_regs[i], X29, slot_off(&fg, i));
        for (int i = nreg; i < total; i++) {
            ldr64(SCR0, X29, 16 + 8 * (int64_t)(i - nreg));
            str64(SCR0, X29, slot_off(&fg, i));
        }
    }

    fg.blk_off = NEWN(int, f->blocks.len);
    for (int i = 0; i < f->blocks.len; i++) {
        IrBlock *b = VEC_AT(&f->blocks, IrBlock, i);
        fg.blk_off[i] = here();
        cur_block_id = i;
        for (int j = 0; j < b->ins.len; j++) {
            IrIns *in = VEC_AT(&b->ins, IrIns, j);
            if (j + 2 == b->ins.len) {
                IrIns *nx = VEC_AT(&b->ins, IrIns, j + 1);
                int cc = fusable_cc(in->op);
                if (cc >= 0 && nx->op == IR_BR && nx->a == in->dst && in->dst >= 0) {
                    emit_fused_branch(&fg, in, nx, cc);
                    j++;
                    continue;
                }
            }
            emit_ins(&fg, in);
        }
        int term = 0;
        if (b->ins.len) {
            IrIns *last = VEC_AT(&b->ins, IrIns, b->ins.len - 1);
            term = last->op == IR_JMP || last->op == IR_BR ||
                   last->op == IR_RET || last->op == IR_RETV;
        }
        if (!term) {
            if (i + 1 < f->blocks.len) jump_to(&fg, i + 1);
            else { mov_rr(X0, XZR); emit_epilogue(&fg); }
        }
    }
    for (int i = 0; i < fg.jumpfix.len; i++) {
        JFix *jf = VEC_AT(&fg.jumpfix, JFix, i);
        int32_t d = (int32_t)((fg.blk_off[jf->blk] - jf->at) / 4);
        if (jf->cond < 0) patch(jf->at, 0x14000000u | ((uint32_t)d & 0x03FFFFFF));
        else patch(jf->at, 0x54000000u | (uint32_t)jf->cond | (((uint32_t)d & 0x7FFFF) << 5));
    }
}

/* ------------------------------------------------------------------ */
/* entry point and image layout                                        */
/* ------------------------------------------------------------------ */

static FnInst *find_fn(const char *name) {
    const char *n = intern(name);
    for (int i = 0; i < g_unit.fns.len; i++) {
        FnInst *f = VEC_AT(&g_unit.fns, FnInst, i);
        if (f->name == n) return f;
    }
    return NULL;
}

static void emit_start(int *start_off, FnInst *main_fn, FnInst *init_fn,
                       FnInst *rt_init, FnInst *finish) {
    *start_off = here();
    /* On entry sp points at argc, then argv, then a null, then envp. */
    rel(mov_imm64_patch(X9), 3, X9, 0);
    mov_from_sp(X10);
    str64(X10, X9, 0);                   /* stack top, for the collector */
    ldr64(X11, SP, 0);                   /* argc */
    str64(X11, X9, 8);
    addsub_imm(0, 0, X12, SP, 8, 0);     /* argv */
    str64(X12, X9, 16);
    /* envp = argv + 8*(argc+1) */
    add_shifted(X12, X12, X11, 3);       /* argv + argc*8 */
    addsub_imm(0, 0, X12, X12, 8, 0);    /* skip the terminating null */
    str64(X12, X9, 24);
    /* keep the stack 16-byte aligned */
    mov_imm(X9, -16);
    and_rr(X9, X10, X9);
    mov_to_sp(X9);

    if (rt_init) { mov_rr(X0, XZR); rel(here(), 0, rt_init->index, 0); w(0x94000000u); }
    if (init_fn) { mov_rr(X0, XZR); rel(here(), 0, init_fn->index, 0); w(0x94000000u); }
    mov_rr(X0, XZR);
    rel(here(), 0, main_fn->index, 0); w(0x94000000u);
    if (finish) {
        mov_rr(X1, X0);
        mov_rr(X0, XZR);
        rel(here(), 0, finish->index, 0); w(0x94000000u);
    } else if (!main_fn->ret || main_fn->ret->kind != TY_INT) {
        mov_rr(X0, XZR);
    }
    mov_imm(X8, 94);                     /* exit_group */
    svc0();
    brk_();
}

void rodata_relocate(uint64_t base, uint64_t *fn_addrs, int nfn);

int codegen_arm64(Unit *u, const char *outpath) {
    memset(&T, 0, sizeof T);
    relocs.len = 0;

    FnInst *main_fn = NULL;
    if (u->build_tests) main_fn = find_fn("$testmain");
    if (!main_fn) main_fn = u->entry;
    if (!main_fn) { fatal("no `main` function found"); return 0; }

    FnInst *rt_init = find_fn("core.rt_init");
    FnInst *finish = NULL;
    if (main_fn->ret && main_fn->ret->kind == TY_RES) finish = find_fn("core.finish_result");

    nfns = u->fns.len;
    fn_addr = (uint64_t *)calloc((size_t)nfns + 1, sizeof(uint64_t));

    int start_off = 0;
    emit_start(&start_off, main_fn, u->init_fn, rt_init, finish);

    for (int i = 0; i < u->fns.len; i++) {
        FnInst *f = VEC_AT(&u->fns, FnInst, i);
        if (!f->reached) { f->code_off = -1; continue; }
        gen_function(f);
    }

    size_t text_size = T.len;
    size_t ro_off = (text_size + 15) & ~(size_t)15;
    TEXT_VADDR = 0x400000 + 0x1000;
    RO_VADDR = TEXT_VADDR + ro_off;
    size_t rw_size = 512 + (size_t)u->globals.len * 8;
    size_t seg1_end = ro_off + g_rodata.data.len;
    DATA_VADDR = ((TEXT_VADDR + seg1_end + 0xFFF) & ~(uint64_t)0xFFF) + 0x1000;

    for (int i = 0; i < u->fns.len; i++) {
        FnInst *f = VEC_AT(&u->fns, FnInst, i);
        fn_addr[i] = (f->code_off >= 0) ? TEXT_VADDR + (uint64_t)f->code_off : 0;
    }

    for (int i = 0; i < relocs.len; i++) {
        Reloc *r = VEC_AT(&relocs, Reloc, i);
        if (r->kind == 0) {
            int tgt = r->target;
            uint64_t dst;
            if (tgt == -1000) { FnInst *g = find_fn("core.divzero"); dst = g ? fn_addr[g->index] : 0; }
            else if (tgt == -1001) { FnInst *g = find_fn("core.oob1"); dst = g ? fn_addr[g->index] : 0; }
            else if (tgt >= 0 && tgt < nfns) dst = fn_addr[tgt];
            else dst = 0;
            if (dst == 0) dst = TEXT_VADDR + (uint64_t)start_off;
            int64_t d = ((int64_t)dst - (int64_t)(TEXT_VADDR + (uint64_t)r->at)) / 4;
            patch(r->at, 0x94000000u | ((uint32_t)d & 0x03FFFFFF));
        } else if (r->kind == 1) {
            uint64_t v = (r->target >= 0 && r->target < nfns) ? fn_addr[r->target] : 0;
            patch_imm64(r->at, r->addend, v);
        } else if (r->kind == 2) {
            patch_imm64(r->at, r->target, RO_VADDR + (uint64_t)r->addend);
        } else {
            patch_imm64(r->at, r->target, DATA_VADDR + (uint64_t)r->addend);
        }
    }
    rodata_relocate(RO_VADDR, fn_addr, nfns);

    ElfImage img;
    memset(&img, 0, sizeof img);
    img.text = &T;
    img.text_vaddr = TEXT_VADDR;
    img.ro_off = ro_off;
    img.ro_vaddr = RO_VADDR;
    img.data_vaddr = DATA_VADDR;
    img.rw_size = rw_size;
    img.entry = TEXT_VADDR + (uint64_t)start_off;
    img.machine = 183;                    /* EM_AARCH64 */
    return elf_write(&img, outpath);
}

int codegen_x64(Unit *u, const char *outpath);

int codegen_run(Unit *u, const char *outpath) {
    if (g_target == TARGET_ARM64) return codegen_arm64(u, outpath);
    return codegen_x64(u, outpath);
}
