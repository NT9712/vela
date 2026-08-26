/* opt.c — IR optimisation and dumping.
 *
 * Deliberately modest: the passes here are the ones that pay for themselves on
 * code produced by a straightforward lowering. Constant folding and copy
 * propagation clean up the redundancy introduced by desugaring; dead-code
 * elimination removes the temporaries that leaves behind; unreachable-block
 * removal and function-level reachability shrink the binary.
 */
#include "vela.h"

static int ins_has_effect(IrIns *i) {
    switch (i->op) {
        case IR_STORE_LOCAL: case IR_STORE_MEM: case IR_STORE_GLOBAL:
        case IR_CALL: case IR_CALL_IND: case IR_SYSCALL:
        case IR_JMP: case IR_BR: case IR_RET: case IR_RETV:
        case IR_TRAP: case IR_SAVE_REGS: case IR_RESTORE_REGS:
            return 1;
        case IR_DIV: case IR_MOD:
            return 1;   /* may trap on divide by zero */
        default:
            return 0;
    }
}

/* ---- per-block constant folding + copy propagation ---- */

typedef struct { int known; int64_t val; } CVal;

static int fold_block(FnInst *f, IrBlock *b) {
    int changed = 0;
    int nv = f->nvregs > 0 ? f->nvregs : 1;
    CVal *c = NEWN(CVal, nv);
    memset(c, 0, sizeof(CVal) * (size_t)nv);

    for (int i = 0; i < b->ins.len; i++) {
        IrIns *in = VEC_AT(&b->ins, IrIns, i);
        int64_t a = 0, bb = 0;
        int ka = in->a >= 0 && in->a < nv && c[in->a].known;
        int kb = in->b >= 0 && in->b < nv && c[in->b].known;
        if (ka) a = c[in->a].val;
        if (kb) bb = c[in->b].val;

        switch (in->op) {
            case IR_CONST:
                if (in->dst >= 0) { c[in->dst].known = 1; c[in->dst].val = in->imm; }
                break;
            case IR_ADD: case IR_SUB: case IR_MUL: case IR_AND: case IR_OR:
            case IR_XOR: case IR_SHL: case IR_SHR:
            case IR_EQ: case IR_NE: case IR_LT: case IR_LE: case IR_GT: case IR_GE: {
                if (!ka || !kb) break;
                int64_t r = 0;
                switch (in->op) {
                    case IR_ADD: r = (int64_t)((uint64_t)a + (uint64_t)bb); break;
                    case IR_SUB: r = (int64_t)((uint64_t)a - (uint64_t)bb); break;
                    case IR_MUL: r = (int64_t)((uint64_t)a * (uint64_t)bb); break;
                    case IR_AND: r = a & bb; break;
                    case IR_OR:  r = a | bb; break;
                    case IR_XOR: r = a ^ bb; break;
                    case IR_SHL: r = (bb >= 0 && bb < 64) ? (int64_t)((uint64_t)a << bb) : 0; break;
                    case IR_SHR: r = (bb >= 0 && bb < 64) ? (a >> bb) : (a < 0 ? -1 : 0); break;
                    case IR_EQ:  r = a == bb; break;
                    case IR_NE:  r = a != bb; break;
                    case IR_LT:  r = a < bb; break;
                    case IR_LE:  r = a <= bb; break;
                    case IR_GT:  r = a > bb; break;
                    case IR_GE:  r = a >= bb; break;
                    default: break;
                }
                in->op = IR_CONST;
                in->imm = r;
                in->a = in->b = -1;
                if (in->dst >= 0) { c[in->dst].known = 1; c[in->dst].val = r; }
                changed = 1;
                break;
            }
            case IR_DIV: case IR_MOD: {
                if (!ka || !kb || bb == 0) break;
                if (a == INT64_MIN && bb == -1) break;
                int64_t r = in->op == IR_DIV ? a / bb : a % bb;
                in->op = IR_CONST; in->imm = r; in->a = in->b = -1;
                if (in->dst >= 0) { c[in->dst].known = 1; c[in->dst].val = r; }
                changed = 1;
                break;
            }
            case IR_NEG:
                if (ka) { in->op = IR_CONST; in->imm = -a; in->a = -1;
                          if (in->dst >= 0) { c[in->dst].known = 1; c[in->dst].val = -a; }
                          changed = 1; }
                break;
            case IR_NOT:
                if (ka) { in->op = IR_CONST; in->imm = ~a; in->a = -1;
                          if (in->dst >= 0) { c[in->dst].known = 1; c[in->dst].val = ~a; }
                          changed = 1; }
                break;
            case IR_BR:
                if (ka) {
                    in->op = IR_JMP;
                    in->target = a ? in->target : in->target2;
                    in->a = -1;
                    changed = 1;
                }
                break;
            default:
                break;
        }
    }
    return changed;
}

/* ---- dead code elimination inside a block ---- */

static int dce_block(FnInst *f, IrBlock *b) {
    int nv = f->nvregs > 0 ? f->nvregs : 1;
    char *used = (char *)arena_alloc(&g_arena, (size_t)nv);
    memset(used, 0, (size_t)nv);
    for (int i = 0; i < b->ins.len; i++) {
        IrIns *in = VEC_AT(&b->ins, IrIns, i);
        if (in->a >= 0 && in->a < nv) used[in->a] = 1;
        if (in->b >= 0 && in->b < nv) used[in->b] = 1;
        for (int k = 0; k < in->args.len; k++) {
            int v = (int)(intptr_t)in->args.data[k];
            if (v >= 0 && v < nv) used[v] = 1;
        }
    }
    int w = 0, changed = 0;
    for (int i = 0; i < b->ins.len; i++) {
        IrIns *in = VEC_AT(&b->ins, IrIns, i);
        if (!ins_has_effect(in) && in->dst >= 0 && in->dst < nv && !used[in->dst]) {
            changed = 1;
            continue;
        }
        b->ins.data[w++] = in;
    }
    b->ins.len = w;
    return changed;
}

/* ---- unreachable block removal ---- */

static void prune_blocks(FnInst *f) {
    int n = f->blocks.len;
    if (!n) return;
    char *reach = (char *)arena_alloc(&g_arena, (size_t)n);
    memset(reach, 0, (size_t)n);
    Vec stack; memset(&stack, 0, sizeof stack);
    vec_push(&stack, (void *)(intptr_t)0);
    reach[0] = 1;
    while (stack.len) {
        int id = (int)(intptr_t)stack.data[--stack.len];
        IrBlock *b = VEC_AT(&f->blocks, IrBlock, id);
        int terminated = 0;
        for (int i = 0; i < b->ins.len; i++) {
            IrIns *in = VEC_AT(&b->ins, IrIns, i);
            if (in->op == IR_JMP) {
                terminated = 1;
                if (in->target < n && !reach[in->target]) { reach[in->target] = 1; vec_push(&stack, (void *)(intptr_t)in->target); }
            } else if (in->op == IR_BR) {
                terminated = 1;
                if (in->target < n && !reach[in->target]) { reach[in->target] = 1; vec_push(&stack, (void *)(intptr_t)in->target); }
                if (in->target2 < n && !reach[in->target2]) { reach[in->target2] = 1; vec_push(&stack, (void *)(intptr_t)in->target2); }
            } else if (in->op == IR_RET || in->op == IR_RETV) {
                terminated = 1;
            }
            if (terminated) { b->ins.len = i + 1; break; }
        }
        if (!terminated && id + 1 < n && !reach[id + 1]) {
            reach[id + 1] = 1;
            vec_push(&stack, (void *)(intptr_t)(id + 1));
        }
    }
    /* Blocks are kept in place (jump targets are indices) but unreachable ones
       are emptied so they cost a single byte each. */
    for (int i = 0; i < n; i++) {
        IrBlock *b = VEC_AT(&f->blocks, IrBlock, i);
        b->reached = reach[i];
        if (!reach[i]) {
            b->ins.len = 0;
            IrIns *t = ir_emit(f, i, IR_TRAP);
            (void)t;
        }
    }
}

/* ---- whole-program function reachability ---- */

int rodata_fixup_fn_targets(int **out);

static void mark_fn(Unit *u, int idx, char *seen) {
    if (idx < 0 || idx >= u->fns.len || seen[idx]) return;
    seen[idx] = 1;
    FnInst *f = VEC_AT(&u->fns, FnInst, idx);
    for (int i = 0; i < f->blocks.len; i++) {
        IrBlock *b = VEC_AT(&f->blocks, IrBlock, i);
        for (int j = 0; j < b->ins.len; j++) {
            IrIns *in = VEC_AT(&b->ins, IrIns, j);
            if (in->op == IR_CALL || in->op == IR_FN_ADDR) mark_fn(u, in->target, seen);
        }
    }
}

static void reachability(Unit *u) {
    int n = u->fns.len;
    if (!n) return;
    char *seen = (char *)arena_alloc(&g_arena, (size_t)n);
    memset(seen, 0, (size_t)n);
    if (u->entry) mark_fn(u, u->entry->index, seen);
    if (u->init_fn) mark_fn(u, u->init_fn->index, seen);
    for (int i = 0; i < u->fns.len; i++) {
        FnInst *f = VEC_AT(&u->fns, FnInst, i);
        if (f->name == intern("core.rt_init") || f->name == intern("core.divzero") ||
            f->name == intern("core.finish_result") || f->name == intern("$testmain"))
            mark_fn(u, i, seen);
    }
    for (int i = 0; i < u->tests.len; i++)
        mark_fn(u, VEC_AT(&u->tests, FnInst, i)->index, seen);
    /* static closures embedded in rodata keep their targets alive */
    int *fns = NULL;
    int nf = rodata_fixup_fn_targets(&fns);
    for (int i = 0; i < nf; i++) mark_fn(u, fns[i], seen);
    for (int i = 0; i < n; i++) VEC_AT(&u->fns, FnInst, i)->reached = seen[i];
}

/* ------------------------------------------------------------------ */
/* verifier                                                             */
/*                                                                      */
/* The IR has one invariant that the whole backend depends on: within a */
/* block, every operand must be defined by an earlier instruction of    */
/* that same block. Checking it here turns a class of silent            */
/* miscompilations into a loud compiler bug.                            */
/* ------------------------------------------------------------------ */

int ir_verify(Unit *u, FILE *out) {
    int bad = 0;
    for (int i = 0; i < u->fns.len; i++) {
        FnInst *f = VEC_AT(&u->fns, FnInst, i);
        int nv = f->nvregs > 0 ? f->nvregs : 1;
        char *def = (char *)arena_alloc(&g_arena, (size_t)nv);
        for (int j = 0; j < f->blocks.len; j++) {
            IrBlock *b = VEC_AT(&f->blocks, IrBlock, j);
            memset(def, 0, (size_t)nv);
            for (int k = 0; k < b->ins.len; k++) {
                IrIns *in = VEC_AT(&b->ins, IrIns, k);
                int ops[3] = { in->a, in->b, -1 };
                for (int q = 0; q < 2; q++) {
                    int v = ops[q];
                    if (v < 0) continue;
                    if (v >= nv || !def[v]) {
                        fprintf(out, "ir: %s b%d i%d: operand %%%d used before definition\n",
                                f->name, j, k, v);
                        bad++;
                    }
                }
                for (int q = 0; q < in->args.len; q++) {
                    int v = (int)(intptr_t)in->args.data[q];
                    if (v < 0) continue;
                    if (v >= nv || !def[v]) {
                        fprintf(out, "ir: %s b%d i%d: call operand %%%d used before definition\n",
                                f->name, j, k, v);
                        bad++;
                    }
                }
                if (in->dst >= 0 && in->dst < nv) {
                    if (def[in->dst]) {
                        fprintf(out, "ir: %s b%d i%d: %%%d defined twice in a block\n",
                                f->name, j, k, in->dst);
                        bad++;
                    }
                    def[in->dst] = 1;
                }
                if (in->op == IR_JMP && (in->target < 0 || in->target >= f->blocks.len)) {
                    fprintf(out, "ir: %s b%d: jump to invalid block %d\n", f->name, j, in->target);
                    bad++;
                }
                if (in->op == IR_BR &&
                    (in->target < 0 || in->target >= f->blocks.len ||
                     in->target2 < 0 || in->target2 >= f->blocks.len)) {
                    fprintf(out, "ir: %s b%d: branch to invalid block\n", f->name, j);
                    bad++;
                }
            }
        }
    }
    return bad;
}

void ir_optimize(Unit *u) {
    for (int i = 0; i < u->fns.len; i++) {
        FnInst *f = VEC_AT(&u->fns, FnInst, i);
        for (int pass = 0; pass < 3; pass++) {
            int changed = 0;
            for (int j = 0; j < f->blocks.len; j++) {
                IrBlock *b = VEC_AT(&f->blocks, IrBlock, j);
                changed |= fold_block(f, b);
                changed |= dce_block(f, b);
            }
            if (!changed) break;
        }
        prune_blocks(f);
    }
    reachability(u);
}

/* ------------------------------------------------------------------ */
/* dumping                                                              */
/* ------------------------------------------------------------------ */

static const char *opname(IrOp o) {
    switch (o) {
        case IR_CONST: return "const"; case IR_CONSTF: return "constf";
        case IR_GLOBAL_ADDR: return "gaddr"; case IR_RODATA_ADDR: return "roaddr";
        case IR_FN_ADDR: return "fnaddr"; case IR_LOAD_LOCAL: return "ld.local";
        case IR_LOAD_MEM: return "ld"; case IR_MOV: return "mov";
        case IR_ADD: return "add"; case IR_SUB: return "sub"; case IR_MUL: return "mul";
        case IR_DIV: return "div"; case IR_MOD: return "mod"; case IR_AND: return "and";
        case IR_OR: return "or"; case IR_XOR: return "xor"; case IR_SHL: return "shl";
        case IR_SHR: return "shr"; case IR_NEG: return "neg"; case IR_NOT: return "not";
        case IR_FADD: return "fadd"; case IR_FSUB: return "fsub"; case IR_FMUL: return "fmul";
        case IR_FDIV: return "fdiv"; case IR_FNEG: return "fneg";
        case IR_EQ: return "eq"; case IR_NE: return "ne"; case IR_LT: return "lt";
        case IR_LE: return "le"; case IR_GT: return "gt"; case IR_GE: return "ge";
        case IR_FEQ: return "feq"; case IR_FNE: return "fne"; case IR_FLT: return "flt";
        case IR_FLE: return "fle"; case IR_FGT: return "fgt"; case IR_FGE: return "fge";
        case IR_I2F: return "i2f"; case IR_F2I: return "f2i";
        case IR_CALL: return "call"; case IR_CALL_IND: return "call.ind";
        case IR_SYSCALL: return "syscall"; case IR_SAVE_REGS: return "save.regs";
        case IR_STACK_TOP: return "rtslot"; case IR_STORE_LOCAL: return "st.local";
        case IR_STORE_MEM: return "st"; case IR_STORE_GLOBAL: return "st.global";
        case IR_RESTORE_REGS: return "restore.regs"; case IR_TRAP: return "trap";
        case IR_JMP: return "jmp"; case IR_BR: return "br";
        case IR_RET: return "ret"; case IR_RETV: return "ret";
        default: return "?";
    }
}

void ir_dump(Unit *u, FILE *out) {
    for (int i = 0; i < u->fns.len; i++) {
        FnInst *f = VEC_AT(&u->fns, FnInst, i);
        if (!f->blocks.len) continue;
        if (!f->reached) continue;
        fprintf(out, "fn %s  (params=%d slots=%d vregs=%d)\n",
                f->name, f->nparams, f->nslots, f->nvregs);
        for (int j = 0; j < f->blocks.len; j++) {
            IrBlock *b = VEC_AT(&f->blocks, IrBlock, j);
            fprintf(out, "  b%d:%s\n", j, b->label ? b->label : "");
            for (int k = 0; k < b->ins.len; k++) {
                IrIns *in = VEC_AT(&b->ins, IrIns, k);
                fprintf(out, "    ");
                if (in->dst >= 0) fprintf(out, "%%%d = ", in->dst);
                fprintf(out, "%s", opname(in->op));
                if (in->op == IR_CONST) fprintf(out, " %lld", (long long)in->imm);
                else if (in->op == IR_CONSTF) fprintf(out, " %g", in->fimm);
                else if (in->op == IR_CALL) fprintf(out, " %s", in->dbg ? in->dbg : "?");
                else if (in->op == IR_JMP) fprintf(out, " b%d", in->target);
                else if (in->op == IR_BR) fprintf(out, " %%%d ? b%d : b%d", in->a, in->target, in->target2);
                else {
                    if (in->a >= 0) fprintf(out, " %%%d", in->a);
                    if (in->b >= 0) fprintf(out, ", %%%d", in->b);
                    if (in->op == IR_LOAD_MEM || in->op == IR_STORE_MEM)
                        fprintf(out, " [+%lld:%d]", (long long)in->imm, in->size);
                    if (in->op == IR_LOAD_LOCAL || in->op == IR_STORE_LOCAL)
                        fprintf(out, " #%d", in->target);
                }
                for (int q = 0; q < in->args.len; q++)
                    fprintf(out, " %%%d", (int)(intptr_t)in->args.data[q]);
                fprintf(out, "\n");
            }
        }
        fprintf(out, "\n");
    }
}
