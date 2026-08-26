/* x64.c — Vela's x86-64 backend.
 *
 * Emits machine code directly: no assembler, no linker, no libc. Each IR block
 * gets a per-block linear-scan register allocation over the callee-saved
 * registers (rbx, r12-r15), which is sound because IR virtual registers never
 * cross a block boundary. Caller-saved registers are used only as scratch
 * inside a single instruction sequence, so a call can never clobber a live
 * value -- which is also what makes the conservative stack-scanning GC safe.
 */
#include "vela.h"
#include <sys/stat.h>

static void chmod_exec(const char *p) { chmod(p, 0755); }

/* ---- registers ---- */
enum { RAX=0, RCX=1, RDX=2, RBX=3, RSP=4, RBP=5, RSI=6, RDI=7,
       R8=8, R9=9, R10=10, R11=11, R12=12, R13=13, R14=14, R15=15 };

static const int alloc_regs[] = { RBX, R12, R13, R14, R15 };
#define NALLOC 5

/* argument registers: env goes in rdi, then user args */
static const int arg_regs[] = { RDI, RSI, RDX, RCX, R8, R9 };
#define NARGREG 6

typedef struct { Buf code; } Emitter;

static Buf T;                       /* text buffer */
static uint64_t TEXT_VADDR;
static uint64_t RO_VADDR;
static uint64_t DATA_VADDR;

typedef struct { int at; int kind; int target; int addend; } Reloc;
/* kind 0: rel32 call/jmp to function `target`
   kind 1: abs64 address of function `target`
   kind 2: abs64 rodata base + addend
   kind 3: abs64 data base + addend */
static Vec relocs;

static uint64_t *fn_addr;
static int nfns;

static void rel(int at, int kind, int target, int addend) {
    Reloc *r = NEW(Reloc);
    r->at = at; r->kind = kind; r->target = target; r->addend = addend;
    vec_push(&relocs, r);
}

/* ---- raw encoding helpers ---- */

static void b1(uint8_t v) { buf_u8(&T, v); }
static void b4(uint32_t v) { buf_u32(&T, v); }
static void b8u(uint64_t v) { buf_u64(&T, v); }
static int  here(void) { return (int)T.len; }

static void rex(int w, int r, int x, int b) {
    uint8_t v = 0x40 | (uint8_t)((w & 1) << 3) | (uint8_t)(((r >> 3) & 1) << 2) |
                (uint8_t)(((x >> 3) & 1) << 1) | (uint8_t)((b >> 3) & 1);
    if (v != 0x40) b1(v);
}
static void rex_always(int w, int r, int x, int b) {
    b1(0x40 | (uint8_t)((w & 1) << 3) | (uint8_t)(((r >> 3) & 1) << 2) |
       (uint8_t)(((x >> 3) & 1) << 1) | (uint8_t)((b >> 3) & 1));
}
static void modrm(int mod, int reg, int rm) {
    b1((uint8_t)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
}

/* [base + index*8 + disp] operand */
static void mem_idx(int reg, int base, int index, int32_t disp) {
    int mod = (disp == 0 && (base & 7) != 5) ? 0 : (disp >= -128 && disp <= 127 ? 1 : 2);
    modrm(mod, reg, 4);                     /* rm = SIB */
    b1((uint8_t)((3 << 6) | ((index & 7) << 3) | (base & 7)));   /* scale 8 */
    if (mod == 1) b1((uint8_t)(int8_t)disp);
    else if (mod == 2) b4((uint32_t)disp);
}

/* [base + disp] operand */
static void mem(int reg, int base, int32_t disp) {
    int rm = base & 7;
    int mod;
    if (disp == 0 && rm != 5) mod = 0;
    else if (disp >= -128 && disp <= 127) mod = 1;
    else mod = 2;
    modrm(mod, reg, base);
    if (rm == 4) b1(0x24);           /* SIB: base=rsp/r12, index=none */
    if (mod == 1) b1((uint8_t)(int8_t)disp);
    else if (mod == 2) b4((uint32_t)disp);
}

/* mov r64, r64 */
static void mov_rr(int d, int s) {
    if (d == s) return;
    rex(1, s, 0, d); b1(0x89); modrm(3, s, d);
}
/* mov r64, imm64 (uses the shortest encoding) */
static void mov_ri(int d, int64_t v) {
    if (v == 0) { rex(0, d, 0, d); b1(0x31); modrm(3, d, d); return; }
    if (v > 0 && v <= 0xFFFFFFFFLL) { rex(0, 0, 0, d); b1((uint8_t)(0xB8 + (d & 7))); b4((uint32_t)v); return; }
    if (v >= INT32_MIN && v <= INT32_MAX) { rex(1, 0, 0, d); b1(0xC7); modrm(3, 0, d); b4((uint32_t)(int32_t)v); return; }
    rex(1, 0, 0, d); b1((uint8_t)(0xB8 + (d & 7))); b8u((uint64_t)v);
}
/* mov r64, imm64 always in the 10-byte form (patchable) */
static int mov_ri64_patch(int d) {
    rex(1, 0, 0, d); b1((uint8_t)(0xB8 + (d & 7)));
    int at = here();
    b8u(0);
    return at;
}
/* mov r, [base+disp] with width */
static void load_rm(int d, int base, int32_t disp, int width, int sign) {
    switch (width) {
        case 1: rex(1, d, 0, base); b1(0x0F); b1(sign ? 0xBE : 0xB6); mem(d, base, disp); break;
        case 2: rex(1, d, 0, base); b1(0x0F); b1(sign ? 0xBF : 0xB7); mem(d, base, disp); break;
        case 4: if (sign) { rex(1, d, 0, base); b1(0x63); mem(d, base, disp); }
                else { rex(0, d, 0, base); b1(0x8B); mem(d, base, disp); } break;
        default: rex(1, d, 0, base); b1(0x8B); mem(d, base, disp); break;
    }
}
/* mov [base+disp], r with width */
static void store_rm(int base, int32_t disp, int s, int width) {
    switch (width) {
        case 1:
            if (s >= 4 && s <= 7) rex_always(0, s, 0, base); else rex(0, s, 0, base);
            b1(0x88); mem(s, base, disp); break;
        case 2: b1(0x66); rex(0, s, 0, base); b1(0x89); mem(s, base, disp); break;
        case 4: rex(0, s, 0, base); b1(0x89); mem(s, base, disp); break;
        default: rex(1, s, 0, base); b1(0x89); mem(s, base, disp); break;
    }
}
static void alu_rr(uint8_t op, int d, int s) { rex(1, s, 0, d); b1(op); modrm(3, s, d); }
static void add_ri(int d, int64_t v) {
    if (v == 0) return;
    if (v >= -128 && v <= 127) { rex(1, 0, 0, d); b1(0x83); modrm(3, 0, d); b1((uint8_t)(int8_t)v); }
    else { rex(1, 0, 0, d); b1(0x81); modrm(3, 0, d); b4((uint32_t)(int32_t)v); }
}
static void sub_ri(int d, int64_t v) {
    if (v == 0) return;
    if (v >= -128 && v <= 127) { rex(1, 0, 0, d); b1(0x83); modrm(3, 5, d); b1((uint8_t)(int8_t)v); }
    else { rex(1, 0, 0, d); b1(0x81); modrm(3, 5, d); b4((uint32_t)(int32_t)v); }
}
static void cmp_rr(int a, int b) { rex(1, b, 0, a); b1(0x39); modrm(3, b, a); }
static void test_rr(int a, int b) { rex(1, b, 0, a); b1(0x85); modrm(3, b, a); }
static void imul_rr(int d, int s) { rex(1, d, 0, s); b1(0x0F); b1(0xAF); modrm(3, d, s); }
static void neg_r(int d) { rex(1, 0, 0, d); b1(0xF7); modrm(3, 3, d); }
static void not_r(int d) { rex(1, 0, 0, d); b1(0xF7); modrm(3, 2, d); }
static void idiv_r(int d) { rex(1, 0, 0, d); b1(0xF7); modrm(3, 7, d); }
static void cqo(void) { b1(0x48); b1(0x99); }
static void shift_cl(int d, int which) { rex(1, 0, 0, d); b1(0xD3); modrm(3, which, d); }
static void setcc(int cc, int d) {
    if (d >= 4 && d <= 7) rex_always(0, 0, 0, d); else rex(0, 0, 0, d);
    b1(0x0F); b1((uint8_t)(0x90 + cc)); modrm(3, 0, d);
}
static void movzx8(int d, int s) { rex(1, d, 0, s); b1(0x0F); b1(0xB6); modrm(3, d, s); }
static void push_r(int r) { if (r >= 8) b1(0x41); b1((uint8_t)(0x50 + (r & 7))); }
static void pop_r(int r) { if (r >= 8) b1(0x41); b1((uint8_t)(0x58 + (r & 7))); }
static void ret_(void) { b1(0xC3); }
static void syscall_(void) { b1(0x0F); b1(0x05); }
static void ud2_(void) { b1(0x0F); b1(0x0B); }
static void lea(int d, int base, int32_t disp) { rex(1, d, 0, base); b1(0x8D); mem(d, base, disp); }

/* SSE: xmm registers 0..15 */
static void movsd_load(int x, int base, int32_t disp) {
    b1(0xF2); rex(0, x, 0, base); b1(0x0F); b1(0x10); mem(x, base, disp);
}
static void movsd_store(int base, int32_t disp, int x) {
    b1(0xF2); rex(0, x, 0, base); b1(0x0F); b1(0x11); mem(x, base, disp);
}
static void sse_rr(uint8_t op, int d, int s) {
    b1(0xF2); rex(0, d, 0, s); b1(0x0F); b1(op); modrm(3, d, s);
}
static void ucomisd(int a, int b) { b1(0x66); rex(0, a, 0, b); b1(0x0F); b1(0x2E); modrm(3, a, b); }
static void cvtsi2sd(int x, int r) { b1(0xF2); rex(1, x, 0, r); b1(0x0F); b1(0x2A); modrm(3, x, r); }
static void cvttsd2si(int r, int x) { b1(0xF2); rex(1, r, 0, x); b1(0x0F); b1(0x2C); modrm(3, r, x); }

/* condition codes */
enum { CC_O=0, CC_NO=1, CC_B=2, CC_AE=3, CC_E=4, CC_NE=5, CC_BE=6, CC_A=7,
       CC_S=8, CC_NS=9, CC_P=10, CC_NP=11, CC_L=12, CC_GE=13, CC_LE=14, CC_G=15 };

/* ------------------------------------------------------------------ */
/* per-function code generation                                        */
/* ------------------------------------------------------------------ */

#define LOC_REG  0
#define LOC_SLOT 1

typedef struct {
    FnInst *f;
    int *kind;        /* LOC_REG / LOC_SLOT per vreg */
    int *where;       /* register number or slot index */
    int  nslots;      /* locals */
    int  nspill;
    int  framesize;
    int  used_mask;
    int *blk_off;
    Vec  jumpfix;     /* JFix* */
    int  save_at;     /* frame offset for callee-saved storage */
} FGen;

typedef struct { int at; int blk; } JFix;

static int cur_block_id;      /* block being emitted, for jump elision */

static int32_t slot_off(FGen *fg, int slot) { return -8 * (slot + 1); }
static int32_t spill_off(FGen *fg, int idx) { return -8 * (fg->nslots + idx + 1); }

static int32_t vloc_off(FGen *fg, int v) {
    return spill_off(fg, fg->where[v]);
}

/* Get a vreg into a physical register (loading from its spill slot if needed). */
static int getr(FGen *fg, int v, int scratch) {
    if (v < 0) { mov_ri(scratch, 0); return scratch; }
    if (fg->kind[v] == LOC_REG) return fg->where[v];
    load_rm(scratch, RBP, vloc_off(fg, v), 8, 0);
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
    store_rm(RBP, vloc_off(fg, v), r, 8);
}

/* float vregs always live in spill slots */
static int32_t foff(FGen *fg, int v) { return vloc_off(fg, v); }
static void fload(FGen *fg, int x, int v) { movsd_load(x, RBP, foff(fg, v)); }
static void fstore(FGen *fg, int v, int x) { movsd_store(RBP, foff(fg, v), x); }

/* ---- register allocation ---- */

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
    int busy[NALLOC];
    int owner[NALLOC];
    for (int i = 0; i < NALLOC; i++) { busy[i] = 0; owner[i] = -1; }
    int spill_free = 0;
    int spill_owner[512];
    int spill_end[512];
    for (int i = 0; i < 512; i++) { spill_owner[i] = -1; spill_end[i] = -1; }

    for (int i = 0; i < n; i++) {
        /* expire */
        for (int r = 0; r < NALLOC; r++)
            if (busy[r] && lastuse[owner[r]] < i) { busy[r] = 0; owner[r] = -1; }
        for (int s = 0; s < spill_free; s++)
            if (spill_owner[s] >= 0 && spill_end[s] < i) spill_owner[s] = -1;

        IrIns *in = VEC_AT(&b->ins, IrIns, i);
        if (in->dst < 0) continue;
        int v = in->dst;
        if (f->vreg_float && f->vreg_float[v]) {
            /* floats live in memory */
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

/* ---- instruction emission ---- */

static void jump_to(FGen *fg, int blk) {
    if (blk == cur_block_id + 1) return;        /* falls through */
    b1(0xE9);
    JFix *jf = NEW(JFix);
    jf->at = here(); jf->blk = blk;
    vec_push(&fg->jumpfix, jf);
    b4(0);
}
static void jcc_to(FGen *fg, int cc, int blk) {
    b1(0x0F); b1((uint8_t)(0x80 + cc));
    JFix *jf = NEW(JFix);
    jf->at = here(); jf->blk = blk;
    vec_push(&fg->jumpfix, jf);
    b4(0);
}

static int cmp_cc(IrOp op) {
    switch (op) {
        case IR_EQ: return CC_E;
        case IR_NE: return CC_NE;
        case IR_LT: return CC_L;
        case IR_LE: return CC_LE;
        case IR_GT: return CC_G;
        case IR_GE: return CC_GE;
        default: return CC_E;
    }
}

/* The condition code a comparison can be folded into a branch as, or -1. */
static int fusable_cc(IrOp op) {
    switch (op) {
        case IR_EQ: return CC_E;
        case IR_NE: return CC_NE;
        case IR_LT: return CC_L;
        case IR_LE: return CC_LE;
        case IR_GT: return CC_G;
        case IR_GE: return CC_GE;
        case IR_FLT: case IR_FGT: return CC_A;
        case IR_FLE: case IR_FGE: return CC_AE;
        default: return -1;
    }
}

static int invert_cc(int cc) {
    switch (cc) {
        case CC_E: return CC_NE;   case CC_NE: return CC_E;
        case CC_L: return CC_GE;   case CC_GE: return CC_L;
        case CC_LE: return CC_G;   case CC_G: return CC_LE;
        case CC_A: return CC_BE;   case CC_BE: return CC_A;
        case CC_AE: return CC_B;   case CC_B: return CC_AE;
        default: return cc;
    }
}

static void emit_ins(FGen *fg, IrIns *in);

static void emit_fused_branch(FGen *fg, IrIns *cmp, IrIns *br, int cc) {
    if (cmp->op >= IR_FEQ && cmp->op <= IR_FGE) {
        if (cmp->op == IR_FLT || cmp->op == IR_FLE) { fload(fg, 0, cmp->b); fload(fg, 1, cmp->a); }
        else { fload(fg, 0, cmp->a); fload(fg, 1, cmp->b); }
        ucomisd(0, 1);
    } else {
        int a = getr(fg, cmp->a, RAX);
        int b = getr(fg, cmp->b, RCX);
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

static void emit_ins(FGen *fg, IrIns *in) {
    FnInst *f = fg->f;
    switch (in->op) {
        case IR_CONST: {
            int d = dstr(fg, in->dst, RAX);
            mov_ri(d, in->imm);
            putr(fg, in->dst, d);
            break;
        }
        case IR_CONSTF: {
            uint64_t bits;
            double dv = in->fimm;
            memcpy(&bits, &dv, 8);
            mov_ri(RAX, (int64_t)bits);
            store_rm(RBP, foff(fg, in->dst), RAX, 8);
            break;
        }
        case IR_RODATA_ADDR: {
            int d = dstr(fg, in->dst, RAX);
            int at = mov_ri64_patch(d);
            rel(at, 2, 0, in->target);
            putr(fg, in->dst, d);
            break;
        }
        case IR_GLOBAL_ADDR: {
            int d = dstr(fg, in->dst, RAX);
            int at = mov_ri64_patch(d);
            rel(at, 3, 0, 512 + in->target * 8);
            putr(fg, in->dst, d);
            break;
        }
        case IR_FN_ADDR: {
            int d = dstr(fg, in->dst, RAX);
            int at = mov_ri64_patch(d);
            rel(at, 1, in->target, 0);
            putr(fg, in->dst, d);
            break;
        }
        case IR_STACK_TOP: {
            int d = dstr(fg, in->dst, RAX);
            if (in->imm < 0) {
                int at = mov_ri64_patch(d);
                rel(at, 3, 0, 0);
            } else {
                int at = mov_ri64_patch(RCX);
                rel(at, 3, 0, (int)in->imm * 8);
                load_rm(d, RCX, 0, 8, 0);
            }
            putr(fg, in->dst, d);
            break;
        }
        case IR_LOAD_LOCAL: {
            if (f->vreg_float && f->vreg_float[in->dst]) {
                load_rm(RAX, RBP, slot_off(fg, in->target), 8, 0);
                store_rm(RBP, foff(fg, in->dst), RAX, 8);
            } else {
                int d = dstr(fg, in->dst, RAX);
                load_rm(d, RBP, slot_off(fg, in->target), 8, 0);
                putr(fg, in->dst, d);
            }
            break;
        }
        case IR_STORE_LOCAL: {
            int s;
            if (f->vreg_float && in->a >= 0 && f->vreg_float[in->a]) {
                load_rm(RAX, RBP, foff(fg, in->a), 8, 0);
                s = RAX;
            } else s = getr(fg, in->a, RAX);
            store_rm(RBP, slot_off(fg, in->target), s, 8);
            break;
        }
        case IR_LOAD_MEM: {
            int base = getr(fg, in->a, RCX);
            if (f->vreg_float && f->vreg_float[in->dst]) {
                load_rm(RAX, base, (int32_t)in->imm, in->size, 0);
                store_rm(RBP, foff(fg, in->dst), RAX, 8);
            } else {
                int d = dstr(fg, in->dst, RAX);
                load_rm(d, base, (int32_t)in->imm, in->size, 0);
                putr(fg, in->dst, d);
            }
            break;
        }
        case IR_STORE_MEM: {
            int base = getr(fg, in->a, RCX);
            int s;
            if (f->vreg_float && in->b >= 0 && f->vreg_float[in->b]) {
                load_rm(RAX, RBP, foff(fg, in->b), 8, 0);
                s = RAX;
            } else s = getr(fg, in->b, RAX);
            if (s == base) { mov_rr(RDX, s); s = RDX; }
            store_rm(base, (int32_t)in->imm, s, in->size);
            break;
        }
        case IR_MOV: {
            int s = getr(fg, in->a, RAX);
            putr(fg, in->dst, s);
            break;
        }
        case IR_ADD: case IR_SUB: case IR_AND: case IR_OR: case IR_XOR: {
            uint8_t op = in->op == IR_ADD ? 0x01 : in->op == IR_SUB ? 0x29 :
                         in->op == IR_AND ? 0x21 : in->op == IR_OR ? 0x09 : 0x31;
            int a = getr(fg, in->a, RAX);
            int b = getr(fg, in->b, RCX);
            int d = dstr(fg, in->dst, RAX);
            if (d == b && in->op != IR_SUB) { alu_rr(op, d, a); putr(fg, in->dst, d); break; }
            if (d == b) { mov_rr(RDX, b); b = RDX; }
            mov_rr(d, a);
            alu_rr(op, d, b);
            putr(fg, in->dst, d);
            break;
        }
        case IR_MUL: {
            int a = getr(fg, in->a, RAX);
            int b = getr(fg, in->b, RCX);
            int d = dstr(fg, in->dst, RAX);
            if (d == b) { mov_rr(RDX, b); b = RDX; }
            mov_rr(d, a);
            imul_rr(d, b);
            putr(fg, in->dst, d);
            break;
        }
        case IR_DIV: case IR_MOD: {
            int a = getr(fg, in->a, RAX);
            int b = getr(fg, in->b, RCX);
            if (b == RAX || b == RDX) { mov_rr(RCX, b); b = RCX; }
            mov_rr(RAX, a);
            /* divide-by-zero check: skip the trap when the divisor is non-zero */
            test_rr(b, b);
            b1(0x0F); b1(0x85);           /* jne over-the-trap */
            int patch = here(); b4(0);
            mov_rr(RDI, b);
            b1(0xE8); { int at = here(); rel(at, 0, -1000, 0); b4(0); }   /* core.divzero */
            ud2_();
            {
                int32_t d = (int32_t)(here() - (patch + 4));
                memcpy(T.data + patch, &d, 4);
            }
            cqo();
            idiv_r(b);
            int d = dstr(fg, in->dst, RAX);
            mov_rr(d, in->op == IR_DIV ? RAX : RDX);
            putr(fg, in->dst, d);
            break;
        }
        case IR_SHL: case IR_SHR: case IR_SAR_HACK: {
            int a = getr(fg, in->a, RAX);
            int b = getr(fg, in->b, RCX);
            if (a == RCX) { mov_rr(RDX, a); a = RDX; }
            mov_rr(RCX, b);
            int d = dstr(fg, in->dst, RAX);
            if (d == RCX) d = RAX;
            mov_rr(d, a);
            shift_cl(d, in->op == IR_SHL ? 4 : 7);
            putr(fg, in->dst, d);
            break;
        }
        case IR_NEG: {
            int a = getr(fg, in->a, RAX);
            int d = dstr(fg, in->dst, RAX);
            mov_rr(d, a);
            neg_r(d);
            putr(fg, in->dst, d);
            break;
        }
        case IR_NOT: {
            int a = getr(fg, in->a, RAX);
            int d = dstr(fg, in->dst, RAX);
            mov_rr(d, a);
            not_r(d);
            putr(fg, in->dst, d);
            break;
        }
        case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE: {
            int a = getr(fg, in->a, RAX);
            int b = getr(fg, in->b, RCX);
            cmp_rr(a, b);
            setcc(cmp_cc(in->op), RDX);
            movzx8(RDX, RDX);
            putr(fg, in->dst, RDX);
            break;
        }
        case IR_FADD: case IR_FSUB: case IR_FMUL: case IR_FDIV: {
            uint8_t op = in->op == IR_FADD ? 0x58 : in->op == IR_FSUB ? 0x5C :
                         in->op == IR_FMUL ? 0x59 : 0x5E;
            fload(fg, 0, in->a);
            fload(fg, 1, in->b);
            sse_rr(op, 0, 1);
            fstore(fg, in->dst, 0);
            break;
        }
        case IR_FSQRT: {
            fload(fg, 0, in->a);
            sse_rr(0x51, 0, 0);          /* sqrtsd xmm0, xmm0 */
            fstore(fg, in->dst, 0);
            break;
        }
        case IR_FNEG: {
            load_rm(RAX, RBP, foff(fg, in->a), 8, 0);
            mov_ri(RCX, (int64_t)0x8000000000000000ULL);
            alu_rr(0x31, RAX, RCX);
            store_rm(RBP, foff(fg, in->dst), RAX, 8);
            break;
        }
        case IR_FEQ: case IR_FNE: case IR_FLT: case IR_FLE: case IR_FGT: case IR_FGE: {
            if (in->op == IR_FLT || in->op == IR_FLE) { fload(fg, 0, in->b); fload(fg, 1, in->a); }
            else { fload(fg, 0, in->a); fload(fg, 1, in->b); }
            ucomisd(0, 1);
            if (in->op == IR_FEQ) {
                setcc(CC_E, RAX); setcc(CC_NP, RCX);
                rex(0, RAX, 0, RCX); b1(0x20); modrm(3, RCX, RAX);   /* and al, cl */
                movzx8(RDX, RAX);
            } else if (in->op == IR_FNE) {
                setcc(CC_NE, RAX); setcc(CC_P, RCX);
                rex(0, RAX, 0, RCX); b1(0x08); modrm(3, RCX, RAX);   /* or al, cl */
                movzx8(RDX, RAX);
            } else {
                int cc = (in->op == IR_FLT || in->op == IR_FGT) ? CC_A : CC_AE;
                setcc(cc, RDX);
                movzx8(RDX, RDX);
            }
            putr(fg, in->dst, RDX);
            break;
        }
        case IR_I2F: {
            if (in->imm) {   /* bitcast */
                int a = getr(fg, in->a, RAX);
                store_rm(RBP, foff(fg, in->dst), a, 8);
            } else {
                int a = getr(fg, in->a, RAX);
                cvtsi2sd(0, a);
                fstore(fg, in->dst, 0);
            }
            break;
        }
        case IR_F2I: {
            if (in->imm) {
                load_rm(RAX, RBP, foff(fg, in->a), 8, 0);
                putr(fg, in->dst, RAX);
            } else {
                fload(fg, 0, in->a);
                cvttsd2si(RAX, 0);
                putr(fg, in->dst, RAX);
            }
            break;
        }
        case IR_CALL: case IR_CALL_IND: {
            int n = in->args.len;
            int nreg = n + 1 < NARGREG ? n + 1 : NARGREG;   /* env + args */
            int nstack = (n + 1) - nreg;
            /* stack arguments, pushed right to left */
            int pad = (nstack & 1) ? 8 : 0;
            if (pad) sub_ri(RSP, 8);
            for (int i = n - 1; i >= nreg - 1; i--) {
                int v = (int)(intptr_t)in->args.data[i];
                int r;
                if (f->vreg_float && v >= 0 && f->vreg_float[v]) {
                    load_rm(RAX, RBP, foff(fg, v), 8, 0); r = RAX;
                } else r = getr(fg, v, RAX);
                push_r(r);
            }
            /* register arguments */
            int fnreg = -1;
            if (in->op == IR_CALL_IND) {
                int code = getr(fg, in->a, R10);
                if (code != R10) { mov_rr(R10, code); }
                fnreg = R10;
            }
            for (int i = nreg - 2; i >= 0; i--) {
                int v = (int)(intptr_t)in->args.data[i];
                int r;
                if (f->vreg_float && v >= 0 && f->vreg_float[v]) {
                    load_rm(RAX, RBP, foff(fg, v), 8, 0); r = RAX;
                } else r = getr(fg, v, RAX);
                mov_rr(arg_regs[i + 1], r);
            }
            if (in->op == IR_CALL_IND) {
                int env = getr(fg, in->b, RAX);
                mov_rr(RDI, env);
                rex(0, 0, 0, fnreg); b1(0xFF); modrm(3, 2, fnreg);
            } else {
                mov_ri(RDI, 0);
                b1(0xE8);
                int at = here();
                rel(at, 0, in->target, 0);
                b4(0);
            }
            if (nstack + (pad ? 1 : 0) > 0) add_ri(RSP, 8 * (nstack) + pad);
            if (in->dst >= 0) {
                if (f->vreg_float && f->vreg_float[in->dst])
                    store_rm(RBP, foff(fg, in->dst), RAX, 8);
                else putr(fg, in->dst, RAX);
            }
            break;
        }
        case IR_SYSCALL: {
            static const int sysregs[] = { RAX, RDI, RSI, RDX, R10, R8, R9 };
            int n = in->args.len;
            if (n > 7) n = 7;
            for (int i = n - 1; i >= 0; i--) {
                int v = (int)(intptr_t)in->args.data[i];
                int r = getr(fg, v, sysregs[i]);
                if (r != sysregs[i]) mov_rr(sysregs[i], r);
            }
            syscall_();
            if (in->dst >= 0) putr(fg, in->dst, RAX);
            break;
        }
        case IR_SAVE_REGS: {
            push_r(RBX); push_r(R12); push_r(R13); push_r(R14); push_r(R15);
            push_r(RAX);   /* padding to keep 16-byte alignment */
            int d = dstr(fg, in->dst, RAX);
            mov_rr(d, RSP);
            putr(fg, in->dst, d);
            break;
        }
        case IR_RESTORE_REGS:
            add_ri(RSP, 48);
            break;
        case IR_LIST_GET: case IR_STR_IDX: case IR_LIST_SET: {
            /* Bounds-checked element access, expanded inline so a hot loop is
               a handful of instructions rather than a call. */
            int base = getr(fg, in->a, RCX);
            if (base != RCX) { mov_rr(RCX, base); base = RCX; }
            int idx = getr(fg, in->b, RSI);
            if (idx != RSI) { mov_rr(RSI, idx); idx = RSI; }
            load_rm(RDX, RCX, 8, 8, 0);              /* rdx = length */
            cmp_rr(RSI, RDX);
            b1(0x0F); b1(0x82);                      /* jb ok  (unsigned) */
            int patch = here(); b4(0);
            mov_rr(RDI, RSI);
            b1(0xE8); { int at = here(); rel(at, 0, -1001, 0); b4(0); }  /* core.oob */
            ud2_();
            { int32_t d = (int32_t)(here() - (patch + 4)); memcpy(T.data + patch, &d, 4); }
            if (in->op == IR_STR_IDX) {
                /* movzx dst, byte [rcx + rsi + 16] */
                int d = dstr(fg, in->dst, RAX);
                rex(1, d, RSI, RCX); b1(0x0F); b1(0xB6);
                modrm(1, d, 4); b1((uint8_t)((0 << 6) | (RSI << 3) | RCX)); b1(16);
                putr(fg, in->dst, d);
                break;
            }
            load_rm(RCX, RCX, 24, 8, 0);             /* rcx = element block */
            if (in->op == IR_LIST_GET) {
                int d = dstr(fg, in->dst, RAX);
                rex(1, d, RSI, RCX); b1(0x8B); mem_idx(d, RCX, RSI, 16);
                putr(fg, in->dst, d);
            } else {
                int v = getr(fg, in->target2, RAX);
                rex(1, v, RSI, RCX); b1(0x89); mem_idx(v, RCX, RSI, 16);
            }
            break;
        }
        case IR_TRAP: ud2_(); break;
        case IR_JMP: jump_to(fg, in->target); break;
        case IR_BR: {
            int c = getr(fg, in->a, RAX);
            test_rr(c, c);
            if (in->target2 == cur_block_id + 1) {
                jcc_to(fg, CC_NE, in->target);
            } else if (in->target == cur_block_id + 1) {
                jcc_to(fg, CC_E, in->target2);
            } else {
                jcc_to(fg, CC_NE, in->target);
                jump_to(fg, in->target2);
            }
            break;
        }
        case IR_RET: case IR_RETV: {
            if (in->op == IR_RETV) {
                if (f->vreg_float && in->a >= 0 && f->vreg_float[in->a])
                    load_rm(RAX, RBP, foff(fg, in->a), 8, 0);
                else { int s = getr(fg, in->a, RAX); mov_rr(RAX, s); }
            } else mov_ri(RAX, 0);
            /* restore callee-saved registers */
            int k = 0;
            for (int r = 0; r < 16; r++) {
                if (!(fg->used_mask & (1 << r))) continue;
                load_rm(r, RBP, -8 * (fg->save_at + k + 1), 8, 0);
                k++;
            }
            mov_rr(RSP, RBP);
            pop_r(RBP);
            ret_();
            break;
        }
        default: break;
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
    for (int r = 0; r < 16; r++) if (fg.used_mask & (1 << r)) nsaved++;
    fg.framesize = 8 * (fg.nslots + fg.nspill + nsaved);
    fg.framesize = (fg.framesize + 15) & ~15;

    f->code_off = here();
    /* prologue */
    push_r(RBP);
    mov_rr(RBP, RSP);
    if (fg.framesize) sub_ri(RSP, fg.framesize);
    int k = 0;
    for (int r = 0; r < 16; r++) {
        if (!(fg.used_mask & (1 << r))) continue;
        store_rm(RBP, -8 * (fg.save_at + k + 1), r, 8);
        k++;
    }
    /* spill incoming arguments to their frame slots (slot 0 = env) */
    {
        int nparams = f->nparams;
        int total = nparams + 1;
        int nreg = total < NARGREG ? total : NARGREG;
        for (int i = 0; i < nreg; i++) store_rm(RBP, slot_off(&fg, i), arg_regs[i], 8);
        for (int i = nreg; i < total; i++) {
            /* stack args start at [rbp+16] */
            load_rm(RAX, RBP, 16 + 8 * (i - nreg), 8, 0);
            store_rm(RBP, slot_off(&fg, i), RAX, 8);
        }
    }

    fg.blk_off = NEWN(int, f->blocks.len);
    for (int i = 0; i < f->blocks.len; i++) {
        IrBlock *b = VEC_AT(&f->blocks, IrBlock, i);
        fg.blk_off[i] = here();
        cur_block_id = i;
        for (int j = 0; j < b->ins.len; j++) {
            IrIns *in = VEC_AT(&b->ins, IrIns, j);
            /* Fuse `cmp` with the branch that immediately consumes it: the
               comparison is the last instruction before the terminator, so its
               result cannot be used anywhere else. */
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
        /* fall through to the next block if not terminated */
        int term = 0;
        if (b->ins.len) {
            IrIns *last = VEC_AT(&b->ins, IrIns, b->ins.len - 1);
            term = last->op == IR_JMP || last->op == IR_BR ||
                   last->op == IR_RET || last->op == IR_RETV;
        }
        if (!term) {
            if (i + 1 < f->blocks.len) jump_to(&fg, i + 1);
            else {
                mov_ri(RAX, 0);
                int kk = 0;
                for (int r = 0; r < 16; r++) {
                    if (!(fg.used_mask & (1 << r))) continue;
                    load_rm(r, RBP, -8 * (fg.save_at + kk + 1), 8, 0);
                    kk++;
                }
                mov_rr(RSP, RBP);
                pop_r(RBP);
                ret_();
            }
        }
    }
    /* patch intra-function jumps */
    for (int i = 0; i < fg.jumpfix.len; i++) {
        JFix *jf = VEC_AT(&fg.jumpfix, JFix, i);
        int32_t d = (int32_t)(fg.blk_off[jf->blk] - (jf->at + 4));
        memcpy(T.data + jf->at, &d, 4);
    }
}

/* ------------------------------------------------------------------ */
/* ELF64 output                                                         */
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
    /* record the stack base for the collector */
    int at = mov_ri64_patch(RCX); rel(at, 3, 0, 0);
    store_rm(RCX, 0, RSP, 8);
    /* argc */
    load_rm(RAX, RSP, 0, 8, 0);
    store_rm(RCX, 8, RAX, 8);
    /* argv */
    lea(RDX, RSP, 8);
    store_rm(RCX, 16, RDX, 8);
    /* envp = argv + 8*(argc+1) */
    lea(RDX, RSP, 16);
    /* rdx = rsp+16 ; rdx += argc*8 */
    b1(0x48); b1(0x8D); b1(0x14); b1(0xC2);      /* lea rdx, [rdx + rax*8] */
    store_rm(RCX, 24, RDX, 8);
    /* align the stack */
    b1(0x48); b1(0x83); b1(0xE4); b1(0xF0);      /* and rsp, -16 */

    if (rt_init) { mov_ri(RDI, 0); b1(0xE8); { int a2 = here(); rel(a2, 0, rt_init->index, 0); b4(0); } }
    if (init_fn) { mov_ri(RDI, 0); b1(0xE8); { int a2 = here(); rel(a2, 0, init_fn->index, 0); b4(0); } }
    mov_ri(RDI, 0);
    b1(0xE8); { int a2 = here(); rel(a2, 0, main_fn->index, 0); b4(0); }
    if (finish) {
        mov_rr(RSI, RAX);
        mov_ri(RDI, 0);
        b1(0xE8); { int a2 = here(); rel(a2, 0, finish->index, 0); b4(0); }
    } else if (!main_fn->ret || main_fn->ret->kind != TY_INT) {
        mov_ri(RAX, 0);
    }
    mov_rr(RDI, RAX);
    mov_ri(RAX, 60);
    syscall_();
    ud2_();
}

void rodata_relocate(uint64_t base, uint64_t *fn_addrs, int nfn);

int codegen_run(Unit *u, const char *outpath) {
    memset(&T, 0, sizeof T);
    relocs.len = 0;

    FnInst *main_fn = NULL;
    if (u->build_tests) main_fn = find_fn("$testmain");
    if (!main_fn) main_fn = u->entry;
    if (!main_fn) { fatal("no `main` function found"); return 0; }

    FnInst *rt_init = find_fn("core.rt_init");
    FnInst *finish = NULL;
    if (main_fn->ret && main_fn->ret->kind == TY_RES) finish = find_fn("core.finish_result");
    else if (main_fn->ret && main_fn->ret->kind == TY_INT) finish = NULL;

    nfns = u->fns.len;
    fn_addr = (uint64_t *)calloc((size_t)nfns + 1, sizeof(uint64_t));

    /* reserve space for _start at the beginning */
    int start_off = 0;
    emit_start(&start_off, main_fn, u->init_fn, rt_init, finish);

    for (int i = 0; i < u->fns.len; i++) {
        FnInst *f = VEC_AT(&u->fns, FnInst, i);
        if (!f->reached) { f->code_off = -1; continue; }
        gen_function(f);
    }

    /* --- layout --- */
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

    /* --- relocations in text --- */
    for (int i = 0; i < relocs.len; i++) {
        Reloc *r = VEC_AT(&relocs, Reloc, i);
        if (r->kind == 0) {
            int tgt = r->target;
            uint64_t dst;
            if (tgt == -1000) {
                FnInst *dz = find_fn("core.divzero");
                dst = dz ? fn_addr[dz->index] : 0;
            } else if (tgt == -1001) {
                FnInst *ob = find_fn("core.oob1");
                dst = ob ? fn_addr[ob->index] : 0;
            } else if (tgt >= 0 && tgt < nfns) dst = fn_addr[tgt];
            else dst = 0;
            if (dst == 0) {
                /* calling a function that was eliminated: trap instead */
                dst = TEXT_VADDR + (uint64_t)start_off;
            }
            int32_t d = (int32_t)((int64_t)dst - (int64_t)(TEXT_VADDR + (uint64_t)r->at + 4));
            memcpy(T.data + r->at, &d, 4);
        } else if (r->kind == 1) {
            uint64_t v = (r->target >= 0 && r->target < nfns) ? fn_addr[r->target] : 0;
            memcpy(T.data + r->at, &v, 8);
        } else if (r->kind == 2) {
            uint64_t v = RO_VADDR + (uint64_t)r->addend;
            memcpy(T.data + r->at, &v, 8);
        } else {
            uint64_t v = DATA_VADDR + (uint64_t)r->addend;
            memcpy(T.data + r->at, &v, 8);
        }
    }
    rodata_relocate(RO_VADDR, fn_addr, nfns);

    /* --- write the ELF --- */
    Buf out;
    memset(&out, 0, sizeof out);
    /* e_ident */
    buf_u8(&out, 0x7f); buf_str(&out, "ELF");
    buf_u8(&out, 2); buf_u8(&out, 1); buf_u8(&out, 1); buf_u8(&out, 0);
    for (int i = 0; i < 8; i++) buf_u8(&out, 0);
    buf_u16(&out, 2);            /* ET_EXEC */
    buf_u16(&out, 0x3E);         /* x86-64 */
    buf_u32(&out, 1);
    buf_u64(&out, TEXT_VADDR + (uint64_t)start_off);   /* entry */
    buf_u64(&out, 64);           /* phoff */
    buf_u64(&out, 0);            /* shoff */
    buf_u32(&out, 0);
    buf_u16(&out, 64);           /* ehsize */
    buf_u16(&out, 56);           /* phentsize */
    buf_u16(&out, 2);            /* phnum */
    buf_u16(&out, 64);           /* shentsize */
    buf_u16(&out, 0);
    buf_u16(&out, 0);

    /* PT_LOAD 1: text + rodata (R+X) */
    buf_u32(&out, 1); buf_u32(&out, 5);
    buf_u64(&out, 0x1000);
    buf_u64(&out, TEXT_VADDR);
    buf_u64(&out, TEXT_VADDR);
    buf_u64(&out, seg1_end);
    buf_u64(&out, seg1_end);
    buf_u64(&out, 0x1000);

    /* PT_LOAD 2: data (R+W), zero-filled */
    uint64_t data_fileoff = 0x1000 + ((seg1_end + 0xFFF) & ~(uint64_t)0xFFF);
    buf_u32(&out, 1); buf_u32(&out, 6);
    buf_u64(&out, data_fileoff);
    buf_u64(&out, DATA_VADDR);
    buf_u64(&out, DATA_VADDR);
    buf_u64(&out, 0);
    buf_u64(&out, rw_size + 4096);
    buf_u64(&out, 0x1000);

    while (out.len < 0x1000) buf_u8(&out, 0);
    buf_put(&out, T.data, T.len);
    while (out.len < 0x1000 + ro_off) buf_u8(&out, 0);
    buf_put(&out, g_rodata.data.data, g_rodata.data.len);

    FILE *fp = fopen(outpath, "wb");
    if (!fp) { fatal("cannot write `%s`", outpath); return 0; }
    fwrite(out.data, 1, out.len, fp);
    fclose(fp);
    chmod_exec(outpath);
    buf_free(&out);
    return 1;
}
