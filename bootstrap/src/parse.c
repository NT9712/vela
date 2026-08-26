/* parse.c — recursive-descent parser for Vela.
 *
 * Newline-sensitive: a statement ends at a newline unless the expression is
 * obviously incomplete (trailing binary operator / open bracket). Produces a
 * full AST with spans on every node, and recovers from errors at statement and
 * declaration boundaries so one bad line does not cascade.
 */
#include "vela.h"
#include <stdarg.h>

typedef struct {
    Tok    *t;
    int     n;
    int     i;
    SrcFile *f;
    Module *mod;
    int     ok;
    int     panic;
    int     depth;      /* recursion guard */
    const char *pending_doc;
} P;

#define MAX_DEPTH 220

static Expr *parse_expr(P *p);
static Stmt *parse_stmt(P *p);
static Stmt *parse_block(P *p);
static TypeExpr *parse_type(P *p);
static Pattern *parse_pattern(P *p);
static Expr *parse_unary(P *p);

/* ---------------- token helpers ---------------- */

static Tok *cur(P *p) { return &p->t[p->i]; }
static TokKind K(P *p) { return p->t[p->i].kind; }
static TokKind K2(P *p) { return p->i + 1 < p->n ? p->t[p->i + 1].kind : T_EOF; }
static Tok *adv(P *p) { Tok *t = &p->t[p->i]; if (p->i < p->n - 1) p->i++; return t; }
static int at(P *p, TokKind k) { return K(p) == k; }

static int accept(P *p, TokKind k) {
    if (K(p) == k) { adv(p); return 1; }
    return 0;
}

static void perr(P *p, Span sp, const char *fmt, ...) {
    if (p->panic) return;
    va_list ap; va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    diag_add(DIAG_ERROR, sp, "%s", msg);
    p->ok = 0;
    p->panic = 1;
}

static Diag *perr_d(P *p, Span sp, const char *fmt, ...) {
    if (p->panic) return NULL;
    va_list ap; va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    Diag *d = diag_add(DIAG_ERROR, sp, "%s", msg);
    p->ok = 0;
    p->panic = 1;
    return d;
}

static const char *tokdesc(Tok *t) {
    static char buf[128];
    switch (t->kind) {
        case T_IDENT: snprintf(buf, sizeof buf, "`%s`", t->text); return buf;
        case T_INT:   snprintf(buf, sizeof buf, "integer `%lld`", (long long)t->ival); return buf;
        case T_FLOAT: snprintf(buf, sizeof buf, "float literal"); return buf;
        case T_STR: case T_INTERP_STR: return "string literal";
        case T_NEWLINE: return "end of line";
        case T_EOF: return "end of file";
        default:
            snprintf(buf, sizeof buf, "`%s`", tok_names[t->kind] ? tok_names[t->kind] : "?");
            return buf;
    }
}

static Tok *expect(P *p, TokKind k, const char *ctx) {
    if (K(p) == k) return adv(p);
    perr(p, cur(p)->span, "expected `%s`%s%s, found %s",
         tok_names[k] ? tok_names[k] : "token",
         ctx ? " " : "", ctx ? ctx : "", tokdesc(cur(p)));
    return NULL;
}

/* Skip newline tokens (used where newlines are insignificant). */
static void skip_nl(P *p) { while (K(p) == T_NEWLINE) adv(p); }

/* End of statement: newline, `}`, EOF, or `;` */
static void end_stmt(P *p) {
    if (K(p) == T_NEWLINE) { adv(p); return; }
    if (K(p) == T_SEMI) { adv(p); return; }   /* `;` separates statements on one line */
    if (K(p) == T_RBRACE || K(p) == T_EOF) return;
    perr(p, cur(p)->span, "unexpected %s after statement", tokdesc(cur(p)));
}

/* Error recovery: skip to the next plausible statement start. */
static void sync_stmt(P *p) {
    p->panic = 0;
    int depth = 0;
    while (K(p) != T_EOF) {
        if (K(p) == T_LBRACE) depth++;
        if (K(p) == T_RBRACE) { if (depth == 0) return; depth--; }
        if (K(p) == T_NEWLINE && depth == 0) { adv(p); return; }
        adv(p);
    }
}

static void sync_decl(P *p) {
    p->panic = 0;
    int depth = 0;
    while (K(p) != T_EOF) {
        if (depth == 0 && (K(p) == T_FN || K(p) == T_STRUCT || K(p) == T_ENUM ||
                           K(p) == T_CONST || K(p) == T_USE || K(p) == T_TYPE ||
                           K(p) == T_PUB || K(p) == T_TEST))
            return;
        if (K(p) == T_LBRACE) depth++;
        else if (K(p) == T_RBRACE) { if (depth > 0) depth--; }
        adv(p);
    }
}

static Expr *mkexpr(P *p, ExprKind k, Span sp) {
    Expr *e = NEW(Expr);
    e->kind = k; e->span = sp;
    return e;
}
static Stmt *mkstmt(P *p, StmtKind k, Span sp) {
    Stmt *s = NEW(Stmt);
    s->kind = k; s->span = sp;
    return s;
}
static Span join(Span a, Span b) {
    if (a.file < 0) return b;
    if (b.file != a.file) return a;
    Span s = a;
    if (b.off + b.len > a.off) s.len = b.off + b.len - a.off;
    return s;
}

/* ---------------- types ---------------- */

static TypeExpr *mktype(TypeExprKind k, Span sp) {
    TypeExpr *t = NEW(TypeExpr);
    t->kind = k; t->span = sp;
    return t;
}

static TypeExpr *parse_type(P *p) {
    Span sp = cur(p)->span;
    if (p->depth++ > MAX_DEPTH) { perr(p, sp, "type is nested too deeply"); p->depth--; return mktype(TE_NAME, sp); }
    TypeExpr *r = NULL;
    switch (K(p)) {
        case T_QUESTION: {
            adv(p);
            r = mktype(TE_OPT, sp);
            r->sub = parse_type(p);
            r->span = join(sp, r->sub->span);
            break;
        }
        case T_BANG: {
            adv(p);
            r = mktype(TE_RES, sp);
            r->sub = parse_type(p);
            r->span = join(sp, r->sub->span);
            break;
        }
        case T_LBRACKET: {
            adv(p);
            r = mktype(TE_LIST, sp);
            r->sub = parse_type(p);
            Tok *e = expect(p, T_RBRACKET, "to close list type");
            if (e) r->span = join(sp, e->span);
            break;
        }
        case T_LBRACE: {
            adv(p);
            r = mktype(TE_MAP, sp);
            r->sub = parse_type(p);
            expect(p, T_COLON, "between map key and value types");
            r->sub2 = parse_type(p);
            Tok *e = expect(p, T_RBRACE, "to close map type");
            if (e) r->span = join(sp, e->span);
            break;
        }
        case T_FN: {
            adv(p);
            r = mktype(TE_FN, sp);
            expect(p, T_LPAREN, "in function type");
            if (!at(p, T_RPAREN)) {
                do {
                    skip_nl(p);
                    if (at(p, T_RPAREN)) break;
                    vec_push(&r->args, parse_type(p));
                    skip_nl(p);
                } while (accept(p, T_COMMA));
            }
            Tok *e = expect(p, T_RPAREN, "to close function type");
            if (accept(p, T_ARROW)) r->sub = parse_type(p);
            if (e) r->span = join(sp, r->sub ? r->sub->span : e->span);
            break;
        }
        case T_IDENT: {
            Tok *id = adv(p);
            r = mktype(TE_NAME, sp);
            r->name = id->text;
            r->span = id->span;
            if (at(p, T_DOT)) {
                adv(p);
                Tok *t2 = expect(p, T_IDENT, "as a type name after `.`");
                if (t2) { r->modname = id->text; r->name = t2->text; r->span = join(sp, t2->span); }
            }
            if (at(p, T_LBRACKET)) {
                adv(p);
                do {
                    skip_nl(p);
                    if (at(p, T_RBRACKET)) break;
                    vec_push(&r->args, parse_type(p));
                    skip_nl(p);
                } while (accept(p, T_COMMA));
                Tok *e = expect(p, T_RBRACKET, "to close type arguments");
                if (e) r->span = join(sp, e->span);
            }
            break;
        }
        default:
            perr(p, sp, "expected a type, found %s", tokdesc(cur(p)));
            r = mktype(TE_NAME, sp);
            r->name = intern("Int");
            break;
    }
    p->depth--;
    return r;
}

/* ---------------- patterns ---------------- */

static Pattern *mkpat(PatKind k, Span sp) {
    Pattern *pt = NEW(Pattern);
    pt->kind = k; pt->span = sp;
    return pt;
}

static Pattern *parse_pattern_atom(P *p) {
    Span sp = cur(p)->span;
    switch (K(p)) {
        case T_UNDERSCORE: adv(p); return mkpat(P_WILD, sp);
        case T_NIL:        adv(p); return mkpat(P_NIL, sp);
        case T_TRUE:  { adv(p); Pattern *q = mkpat(P_BOOL, sp); q->ival = 1; return q; }
        case T_FALSE: { adv(p); Pattern *q = mkpat(P_BOOL, sp); q->ival = 0; return q; }
        case T_MINUS: {
            adv(p);
            if (at(p, T_INT)) { Tok *t = adv(p); Pattern *q = mkpat(P_INT, join(sp, t->span)); q->ival = -t->ival; return q; }
            if (at(p, T_FLOAT)) { Tok *t = adv(p); Pattern *q = mkpat(P_FLOAT, join(sp, t->span)); q->fval = -t->fval; return q; }
            perr(p, sp, "expected a number after `-` in pattern");
            return mkpat(P_WILD, sp);
        }
        case T_INT:   { Tok *t = adv(p); Pattern *q = mkpat(P_INT, sp);   q->ival = t->ival; return q; }
        case T_FLOAT: { Tok *t = adv(p); Pattern *q = mkpat(P_FLOAT, sp); q->fval = t->fval; return q; }
        case T_CHAR:  { Tok *t = adv(p); Pattern *q = mkpat(P_CHAR, sp);  q->ival = t->ival; return q; }
        case T_STR:   { Tok *t = adv(p); Pattern *q = mkpat(P_STR, sp);   q->sval = t->text; q->ival = t->ival; return q; }
        case T_IDENT: {
            Tok *id = adv(p);
            /* ok(x) / err(e) sugar for !T */
            if (at(p, T_LPAREN) && (id->text == intern("ok") || id->text == intern("err") ||
                                    id->text == intern("some"))) {
                PatKind pk = id->text == intern("ok") ? P_OK :
                             id->text == intern("err") ? P_ERR : P_SOME;
                adv(p);
                Pattern *q = mkpat(pk, sp);
                if (!at(p, T_RPAREN)) vec_push(&q->subs, parse_pattern(p));
                Tok *e = expect(p, T_RPAREN, "to close pattern");
                if (e) q->span = join(sp, e->span);
                return q;
            }
            if (at(p, T_DOT)) {
                adv(p);
                Tok *v = expect(p, T_IDENT, "as enum variant name");
                Pattern *q = mkpat(P_ENUM, sp);
                q->tyname = id->text;
                q->name = v ? v->text : intern("?");
                if (at(p, T_DOT)) {          /* mod.Type.Variant */
                    adv(p);
                    Tok *v2 = expect(p, T_IDENT, "as enum variant name");
                    q->modname = id->text;
                    q->tyname = v ? v->text : intern("?");
                    q->name = v2 ? v2->text : intern("?");
                }
                if (accept(p, T_LPAREN)) {
                    if (!at(p, T_RPAREN)) {
                        do { skip_nl(p); if (at(p,T_RPAREN)) break;
                             vec_push(&q->subs, parse_pattern(p)); skip_nl(p);
                        } while (accept(p, T_COMMA));
                    }
                    Tok *e = expect(p, T_RPAREN, "to close variant pattern");
                    if (e) q->span = join(sp, e->span);
                } else if (v) q->span = join(sp, v->span);
                return q;
            }
            if (at(p, T_LBRACE)) {
                adv(p);
                Pattern *q = mkpat(P_STRUCT, sp);
                q->tyname = id->text;
                skip_nl(p);
                while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
                    Tok *fn = expect(p, T_IDENT, "as field name");
                    if (!fn) break;
                    Pattern *sub;
                    if (accept(p, T_COLON)) sub = parse_pattern(p);
                    else { sub = mkpat(P_BIND, fn->span); sub->name = fn->text; }
                    vec_push(&q->fields, (void *)(fn->text));
                    vec_push(&q->subs, sub);
                    skip_nl(p);
                    if (!accept(p, T_COMMA)) break;
                    skip_nl(p);
                }
                Tok *e = expect(p, T_RBRACE, "to close struct pattern");
                if (e) q->span = join(sp, e->span);
                return q;
            }
            Pattern *q = mkpat(P_BIND, sp);
            q->name = id->text;
            return q;
        }
        default:
            perr(p, sp, "expected a pattern, found %s", tokdesc(cur(p)));
            return mkpat(P_WILD, sp);
    }
}

static Pattern *parse_pattern(P *p) {
    Pattern *a = parse_pattern_atom(p);
    if (at(p, T_PIPE)) {
        Pattern *o = mkpat(P_OR, a->span);
        vec_push(&o->subs, a);
        while (accept(p, T_PIPE)) {
            Pattern *b = parse_pattern_atom(p);
            vec_push(&o->subs, b);
            o->span = join(o->span, b->span);
        }
        return o;
    }
    return a;
}

/* ---------------- expressions ---------------- */

static int prec_of(TokKind k) {
    switch (k) {
        case T_OR: return 1;
        case T_AND: return 2;
        case T_EQEQ: case T_BANGEQ: case T_LT: case T_LE: case T_GT: case T_GE: return 3;
        case T_DOTDOT: case T_DOTDOTEQ: return 4;
        case T_PIPE: case T_CARET: return 5;
        case T_AMP: return 6;
        case T_SHL: case T_SHR: return 7;
        case T_PLUS: case T_MINUS: return 8;
        case T_STAR: case T_SLASH: case T_PERCENT: return 9;
        default: return 0;
    }
}

static Expr *parse_lambda_short(P *p) {
    /* |a, b| expr    or   || expr */
    Span sp = cur(p)->span;
    Expr *e = mkexpr(p, E_LAMBDA, sp);
    adv(p);  /* '|' */
    if (!at(p, T_PIPE)) {
        do {
            Tok *id = expect(p, T_IDENT, "as lambda parameter");
            if (!id) break;
            Param *pa = NEW(Param);
            pa->name = id->text; pa->span = id->span;
            if (accept(p, T_COLON)) pa->type = parse_type(p);
            vec_push(&e->params, pa);
        } while (accept(p, T_COMMA));
    }
    expect(p, T_PIPE, "to close lambda parameters");
    if (accept(p, T_ARROW)) e->texpr = parse_type(p);
    if (at(p, T_LBRACE)) e->body = parse_block(p);
    else {
        Expr *body = parse_expr(p);
        Stmt *r = mkstmt(p, S_RETURN, body->span);
        r->a = body;
        Stmt *blk = mkstmt(p, S_BLOCK, body->span);
        vec_push(&blk->list, r);
        e->body = blk;
        e->span = join(sp, body->span);
    }
    return e;
}

static Expr *parse_lambda_fn(P *p) {
    Span sp = cur(p)->span;
    Expr *e = mkexpr(p, E_LAMBDA, sp);
    adv(p);  /* 'fn' */
    expect(p, T_LPAREN, "in anonymous function");
    if (!at(p, T_RPAREN)) {
        do {
            skip_nl(p);
            Tok *id = expect(p, T_IDENT, "as parameter name");
            if (!id) break;
            Param *pa = NEW(Param);
            pa->name = id->text; pa->span = id->span;
            expect(p, T_COLON, "after parameter name");
            pa->type = parse_type(p);
            vec_push(&e->params, pa);
            skip_nl(p);
        } while (accept(p, T_COMMA));
    }
    expect(p, T_RPAREN, "to close parameters");
    if (accept(p, T_ARROW)) e->texpr = parse_type(p);
    e->body = parse_block(p);
    return e;
}

static Expr *parse_match(P *p);

/* Does a `{` here start a struct literal (vs a block)? Only in a context where
   a struct literal is allowed, which is tracked by p_no_struct. */
static int p_no_struct = 0;

static Expr *parse_primary(P *p) {
    Span sp = cur(p)->span;
    switch (K(p)) {
        case T_INT:   { Tok *t = adv(p); Expr *e = mkexpr(p, E_INT, sp); e->ival = t->ival; return e; }
        case T_FLOAT: { Tok *t = adv(p); Expr *e = mkexpr(p, E_FLOAT, sp); e->fval = t->fval; return e; }
        case T_CHAR:  { Tok *t = adv(p); Expr *e = mkexpr(p, E_CHAR, sp); e->ival = t->ival; return e; }
        case T_STR:   { Tok *t = adv(p); Expr *e = mkexpr(p, E_STR, sp); e->sval = t->text; e->ival = t->ival; return e; }
        case T_TRUE:  { adv(p); Expr *e = mkexpr(p, E_BOOL, sp); e->ival = 1; return e; }
        case T_FALSE: { adv(p); Expr *e = mkexpr(p, E_BOOL, sp); e->ival = 0; return e; }
        case T_NIL:   { adv(p); return mkexpr(p, E_NIL, sp); }
        case T_SELF:  { adv(p); Expr *e = mkexpr(p, E_IDENT, sp); e->name = intern("self"); return e; }
        case T_INTERP_STR: {
            Tok *t = adv(p);
            Expr *e = mkexpr(p, E_INTERP, sp);
            for (StrPiece *piece = t->pieces; piece; piece = piece->next) {
                if (!piece->is_expr) {
                    Expr *lit = mkexpr(p, E_STR, sp);
                    lit->sval = piece->text;
                    lit->ival = (int64_t)strlen(piece->text);
                    vec_push(&e->list, lit);
                } else {
                    P sub;
                    memset(&sub, 0, sizeof sub);
                    sub.t = piece->toks; sub.n = piece->ntoks; sub.i = 0;
                    sub.f = p->f; sub.mod = p->mod; sub.ok = 1; sub.depth = p->depth;
                    Expr *ie = parse_expr(&sub);
                    if (!sub.ok) p->ok = 0;
                    if (sub.ok && sub.t[sub.i].kind != T_EOF)
                        diag_add(DIAG_ERROR, sub.t[sub.i].span,
                                 "unexpected %s in string interpolation", tokdesc(&sub.t[sub.i])),
                        p->ok = 0;
                    vec_push(&e->list, ie);
                }
            }
            return e;
        }
        case T_LPAREN: {
            adv(p);
            skip_nl(p);
            int save = p_no_struct; p_no_struct = 0;
            Expr *e = parse_expr(p);
            p_no_struct = save;
            skip_nl(p);
            Tok *r = expect(p, T_RPAREN, "to close parenthesised expression");
            if (r) e->span = join(sp, r->span);
            return e;
        }
        case T_LBRACKET: {
            adv(p);
            Expr *e = mkexpr(p, E_LIST, sp);
            int save = p_no_struct; p_no_struct = 0;
            skip_nl(p);
            while (!at(p, T_RBRACKET) && !at(p, T_EOF)) {
                vec_push(&e->list, parse_expr(p));
                skip_nl(p);
                if (!accept(p, T_COMMA)) break;
                skip_nl(p);
            }
            p_no_struct = save;
            Tok *r = expect(p, T_RBRACKET, "to close list literal");
            if (r) e->span = join(sp, r->span);
            return e;
        }
        case T_LBRACE: {
            /* map literal: { k: v, ... } or {:} */
            adv(p);
            Expr *e = mkexpr(p, E_MAP, sp);
            int save = p_no_struct; p_no_struct = 0;
            skip_nl(p);
            if (accept(p, T_COLON)) { /* {:} empty */ }
            else {
                while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
                    Expr *k = parse_expr(p);
                    expect(p, T_COLON, "between map key and value");
                    Expr *v = parse_expr(p);
                    vec_push(&e->list, k);
                    vec_push(&e->list, v);
                    skip_nl(p);
                    if (!accept(p, T_COMMA)) break;
                    skip_nl(p);
                }
            }
            p_no_struct = save;
            Tok *r = expect(p, T_RBRACE, "to close map literal");
            if (r) e->span = join(sp, r->span);
            return e;
        }
        case T_PIPE: return parse_lambda_short(p);
        case T_OR: {
            /* `||` lexes as T_OR when there are no params: `|| expr` */
            Span s2 = cur(p)->span;
            adv(p);
            Expr *e = mkexpr(p, E_LAMBDA, s2);
            if (accept(p, T_ARROW)) e->texpr = parse_type(p);
            if (at(p, T_LBRACE)) e->body = parse_block(p);
            else {
                Expr *body = parse_expr(p);
                Stmt *r = mkstmt(p, S_RETURN, body->span);
                r->a = body;
                Stmt *blk = mkstmt(p, S_BLOCK, body->span);
                vec_push(&blk->list, r);
                e->body = blk;
            }
            return e;
        }
        case T_FN: return parse_lambda_fn(p);
        case T_MATCH: return parse_match(p);
        case T_AT: {
            adv(p);
            Tok *id = expect(p, T_IDENT, "as intrinsic name");
            Expr *e = mkexpr(p, E_INTRINSIC, sp);
            e->name = id ? id->text : intern("?");
            if (at(p, T_LBRACKET)) {
                adv(p);
                do { skip_nl(p); if (at(p,T_RBRACKET)) break;
                     vec_push(&e->targs, parse_type(p)); skip_nl(p);
                } while (accept(p, T_COMMA));
                expect(p, T_RBRACKET, "to close intrinsic type arguments");
            }
            expect(p, T_LPAREN, "after intrinsic name");
            int save = p_no_struct; p_no_struct = 0;
            if (!at(p, T_RPAREN)) {
                do { skip_nl(p); if (at(p,T_RPAREN)) break;
                     vec_push(&e->list, parse_expr(p)); skip_nl(p);
                } while (accept(p, T_COMMA));
            }
            p_no_struct = save;
            Tok *r = expect(p, T_RPAREN, "to close intrinsic call");
            if (r) e->span = join(sp, r->span);
            return e;
        }
        case T_RETURN: case T_BREAK: case T_CONTINUE: {
            Expr *e = mkexpr(p, E_STMTEXPR, sp);
            e->stmt = parse_stmt(p);
            return e;
        }
        case T_IDENT: {
            Tok *id = adv(p);
            /* Type[Args]{...} or Type{...} struct literal, or generic call */
            if (at(p, T_LBRACE) && !p_no_struct) {
                Expr *e = mkexpr(p, E_STRUCT, sp);
                e->name = id->text;
                adv(p);
                int save = p_no_struct; p_no_struct = 0;
                skip_nl(p);
                while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
                    Tok *fn = expect(p, T_IDENT, "as field name");
                    if (!fn) break;
                    FieldInit *fi = NEW(FieldInit);
                    fi->name = fn->text; fi->span = fn->span;
                    if (accept(p, T_COLON)) fi->value = parse_expr(p);
                    else {
                        Expr *ref = mkexpr(p, E_IDENT, fn->span);
                        ref->name = fn->text;
                        fi->value = ref;
                    }
                    vec_push(&e->list, fi);
                    skip_nl(p);
                    if (!accept(p, T_COMMA)) break;
                    skip_nl(p);
                }
                p_no_struct = save;
                Tok *r = expect(p, T_RBRACE, "to close struct literal");
                if (r) e->span = join(sp, r->span);
                return e;
            }
            Expr *e = mkexpr(p, E_IDENT, sp);
            e->name = id->text;
            return e;
        }
        default:
            perr(p, sp, "expected an expression, found %s", tokdesc(cur(p)));
            return mkexpr(p, E_INT, sp);
    }
}

/* Look ahead from a `[` to decide: type-argument list or index? Heuristic:
   `ident [ T ] (` where the contents parse as types and are followed by `(`
   or `{`. We scan for the matching `]` and check the next token. */
static int looks_like_targs(P *p) {
    int i = p->i, depth = 0, bdepth = 0;
    while (i < p->n) {
        TokKind k = p->t[i].kind;
        if (k == T_LBRACKET) depth++;
        else if (k == T_RBRACKET) { depth--; if (depth == 0) break; }
        else if (k == T_LBRACE) bdepth++;
        else if (k == T_RBRACE) { if (bdepth == 0) return 0; bdepth--; }
        else if (k == T_NEWLINE || k == T_EOF || k == T_SEMI) return 0;
        i++;
    }
    if (i >= p->n - 1) return 0;
    TokKind after = p->t[i + 1].kind;
    /* `xs[i] { ... }` is an index followed by a block whenever struct literals
       are not allowed here (if/while/for/match headers). */
    if (after != T_LPAREN && !(after == T_LBRACE && !p_no_struct)) return 0;
    /* contents must look like a type list */
    for (int j = p->i + 1; j < i; j++) {
        switch (p->t[j].kind) {
            case T_IDENT: case T_QUESTION: case T_BANG: case T_LBRACKET:
            case T_RBRACKET: case T_LBRACE: case T_RBRACE: case T_COLON:
            case T_COMMA: case T_FN: case T_ARROW: case T_LPAREN: case T_RPAREN:
                break;
            default: return 0;
        }
    }
    return 1;
}

static Expr *parse_postfix(P *p) {
    Expr *e = parse_primary(p);
    for (;;) {
        Span sp = cur(p)->span;
        if (at(p, T_DOT)) {
            adv(p);
            if (at(p, T_INT)) {   /* tuple-ish access is not supported; give a good error */
                perr(p, cur(p)->span, "numeric field access is not supported; use named fields");
                adv(p);
                continue;
            }
            Tok *id = expect(p, T_IDENT, "after `.`");
            if (!id) break;
            if (at(p, T_LPAREN) || (at(p, T_LBRACKET) && looks_like_targs(p))) {
                Expr *m = mkexpr(p, E_METHOD, e->span);
                m->a = e;
                m->name = id->text;
                if (at(p, T_LBRACKET)) {
                    adv(p);
                    do { skip_nl(p); if (at(p,T_RBRACKET)) break;
                         vec_push(&m->targs, parse_type(p)); skip_nl(p);
                    } while (accept(p, T_COMMA));
                    expect(p, T_RBRACKET, "to close type arguments");
                }
                expect(p, T_LPAREN, "to start arguments");
                int save = p_no_struct; p_no_struct = 0;
                skip_nl(p);
                while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
                    vec_push(&m->list, parse_expr(p));
                    skip_nl(p);
                    if (!accept(p, T_COMMA)) break;
                    skip_nl(p);
                }
                p_no_struct = save;
                Tok *r = expect(p, T_RPAREN, "to close arguments");
                if (r) m->span = join(e->span, r->span);
                e = m;
            } else {
                Expr *f = mkexpr(p, E_FIELD, join(e->span, id->span));
                f->a = e;
                f->name = id->text;
                e = f;
            }
            continue;
        }
        if (at(p, T_LPAREN)) {
            adv(p);
            Expr *c = mkexpr(p, E_CALL, e->span);
            c->a = e;
            int save = p_no_struct; p_no_struct = 0;
            skip_nl(p);
            while (!at(p, T_RPAREN) && !at(p, T_EOF)) {
                vec_push(&c->list, parse_expr(p));
                skip_nl(p);
                if (!accept(p, T_COMMA)) break;
                skip_nl(p);
            }
            p_no_struct = save;
            Tok *r = expect(p, T_RPAREN, "to close call arguments");
            if (r) c->span = join(e->span, r->span);
            e = c;
            continue;
        }
        if (at(p, T_LBRACKET)) {
            if (looks_like_targs(p) && e->kind == E_IDENT) {
                adv(p);
                do { skip_nl(p); if (at(p,T_RBRACKET)) break;
                     vec_push(&e->targs, parse_type(p)); skip_nl(p);
                } while (accept(p, T_COMMA));
                expect(p, T_RBRACKET, "to close type arguments");
                if (at(p, T_LBRACE) && !p_no_struct) {
                    /* generic struct literal */
                    Expr *st = mkexpr(p, E_STRUCT, e->span);
                    st->name = e->name;
                    st->targs = e->targs;
                    adv(p);
                    int save = p_no_struct; p_no_struct = 0;
                    skip_nl(p);
                    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
                        Tok *fn = expect(p, T_IDENT, "as field name");
                        if (!fn) break;
                        FieldInit *fi = NEW(FieldInit);
                        fi->name = fn->text; fi->span = fn->span;
                        if (accept(p, T_COLON)) fi->value = parse_expr(p);
                        else { Expr *ref = mkexpr(p, E_IDENT, fn->span); ref->name = fn->text; fi->value = ref; }
                        vec_push(&st->list, fi);
                        skip_nl(p);
                        if (!accept(p, T_COMMA)) break;
                        skip_nl(p);
                    }
                    p_no_struct = save;
                    Tok *r = expect(p, T_RBRACE, "to close struct literal");
                    if (r) st->span = join(e->span, r->span);
                    e = st;
                }
                continue;
            }
            adv(p);
            int save = p_no_struct; p_no_struct = 0;
            skip_nl(p);
            Expr *idx = NULL;
            Expr *r2 = NULL;
            int inclusive = 0, is_slice = 0;
            if (at(p, T_DOTDOT) || at(p, T_DOTDOTEQ)) {
                is_slice = 1;
                inclusive = at(p, T_DOTDOTEQ);
                adv(p);
                if (!at(p, T_RBRACKET)) r2 = parse_expr(p);
            } else {
                idx = parse_expr(p);
            }
            p_no_struct = save;
            Tok *rb = expect(p, T_RBRACKET, "to close index");
            if (idx && idx->kind == E_RANGE) {
                Expr *s = mkexpr(p, E_SLICE, join(e->span, rb ? rb->span : sp));
                s->a = e; s->b = idx->a; s->c = idx->b; s->inclusive = idx->inclusive;
                e = s;
            } else if (is_slice) {
                Expr *s = mkexpr(p, E_SLICE, join(e->span, rb ? rb->span : sp));
                s->a = e; s->b = NULL; s->c = r2; s->inclusive = inclusive;
                e = s;
            } else {
                Expr *ix = mkexpr(p, E_INDEX, join(e->span, rb ? rb->span : sp));
                ix->a = e; ix->b = idx;
                e = ix;
            }
            continue;
        }
        if (at(p, T_QUESTION)) {
            adv(p);
            Expr *t = mkexpr(p, E_TRY, join(e->span, sp));
            t->a = e;
            e = t;
            continue;
        }
        if (at(p, T_AS)) {
            adv(p);
            Expr *c = mkexpr(p, E_CAST, e->span);
            c->a = e;
            c->texpr = parse_type(p);
            c->span = join(e->span, c->texpr->span);
            e = c;
            continue;
        }
        break;
    }
    return e;
}

static Expr *parse_unary(P *p) {
    Span sp = cur(p)->span;
    if (at(p, T_MINUS) || at(p, T_NOT) || at(p, T_TILDE)) {
        TokKind op = K(p);
        adv(p);
        Expr *sub = parse_unary(p);
        /* fold negative literals */
        if (op == T_MINUS && sub->kind == E_INT) { sub->ival = -sub->ival; sub->span = join(sp, sub->span); return sub; }
        if (op == T_MINUS && sub->kind == E_FLOAT) { sub->fval = -sub->fval; sub->span = join(sp, sub->span); return sub; }
        Expr *e = mkexpr(p, E_UNARY, join(sp, sub->span));
        e->op = op; e->a = sub;
        return e;
    }
    return parse_postfix(p);
}

static Expr *parse_binary(P *p, int min_prec) {
    if (p->depth++ > MAX_DEPTH) {
        perr(p, cur(p)->span, "expression is nested too deeply (limit %d)", MAX_DEPTH);
        p->depth--;
        return mkexpr(p, E_INT, cur(p)->span);
    }
    Expr *lhs = parse_unary(p);
    for (;;) {
        TokKind k = K(p);
        int pr = prec_of(k);
        if (pr == 0 || pr < min_prec) break;
        Span opsp = cur(p)->span;
        adv(p);
        skip_nl(p);
        if (k == T_DOTDOT || k == T_DOTDOTEQ) {
            Expr *rhs2 = NULL;
            if (!at(p, T_RBRACKET) && !at(p, T_RPAREN) && !at(p, T_LBRACE) &&
                !at(p, T_NEWLINE) && !at(p, T_EOF) && !at(p, T_COMMA))
                rhs2 = parse_binary(p, pr + 1);
            Expr *e = mkexpr(p, E_RANGE, join(lhs->span, rhs2 ? rhs2->span : opsp));
            e->a = lhs; e->b = rhs2; e->inclusive = (k == T_DOTDOTEQ);
            lhs = e;
            continue;
        }
        Expr *rhs = parse_binary(p, pr + 1);
        if (pr == 3 && prec_of(K(p)) == 3) {
            perr(p, cur(p)->span, "comparison operators cannot be chained; use `and` to combine them");
        }
        Expr *e = mkexpr(p, E_BINARY, join(lhs->span, rhs->span));
        e->op = k; e->a = lhs; e->b = rhs;
        lhs = e;
    }
    p->depth--;
    return lhs;
}

static Expr *parse_expr(P *p) {
    Expr *e = parse_binary(p, 1);
    while (at(p, T_QQ)) {
        adv(p);
        skip_nl(p);
        Expr *rhs;
        if (at(p, T_RETURN) || at(p, T_BREAK) || at(p, T_CONTINUE)) {
            rhs = mkexpr(p, E_STMTEXPR, cur(p)->span);
            rhs->stmt = parse_stmt(p);
        } else rhs = parse_binary(p, 1);
        Expr *o = mkexpr(p, E_ORELSE, join(e->span, rhs->span));
        o->a = e; o->b = rhs;
        e = o;
    }
    return e;
}

static Expr *parse_match(P *p) {
    Span sp = cur(p)->span;
    adv(p);   /* match */
    Expr *e = mkexpr(p, E_MATCH, sp);
    int save = p_no_struct; p_no_struct = 1;
    e->a = parse_expr(p);
    p_no_struct = save;
    expect(p, T_LBRACE, "to open match arms");
    skip_nl(p);
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        MatchArm *arm = NEW(MatchArm);
        arm->span = cur(p)->span;
        arm->pat = parse_pattern(p);
        if (accept(p, T_IF)) {
            int s2 = p_no_struct; p_no_struct = 1;
            arm->guard = parse_expr(p);
            p_no_struct = s2;
        }
        expect(p, T_FATARROW, "after match pattern");
        if (at(p, T_LBRACE)) arm->block = parse_block(p);
        else arm->body = parse_expr(p);
        vec_push(&e->list, arm);
        accept(p, T_COMMA);
        skip_nl(p);
        if (p->panic) { sync_stmt(p); skip_nl(p); }
    }
    Tok *r = expect(p, T_RBRACE, "to close match");
    if (r) e->span = join(sp, r->span);
    return e;
}

/* ---------------- statements ---------------- */

static Stmt *parse_block(P *p) {
    Span sp = cur(p)->span;
    Stmt *b = mkstmt(p, S_BLOCK, sp);
    if (!expect(p, T_LBRACE, "to open block")) return b;
    skip_nl(p);
    while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
        Stmt *s = parse_stmt(p);
        if (s) vec_push(&b->list, s);
        if (p->panic) { sync_stmt(p); }
        skip_nl(p);
    }
    Tok *r = expect(p, T_RBRACE, "to close block");
    if (r) b->span = join(sp, r->span);
    return b;
}

static int is_lvalue(Expr *e) {
    return e->kind == E_IDENT || e->kind == E_FIELD || e->kind == E_INDEX;
}

static Stmt *parse_stmt(P *p) {
    Span sp = cur(p)->span;
    if (p->depth++ > MAX_DEPTH) {
        perr(p, sp, "statement is nested too deeply (limit %d)", MAX_DEPTH);
        p->depth--;
        return mkstmt(p, S_BLOCK, sp);
    }
    Stmt *r = NULL;
    switch (K(p)) {
        case T_LET: {
            adv(p);
            r = mkstmt(p, S_LET, sp);
            r->is_mut = accept(p, T_MUT);
            Tok *id = expect(p, T_IDENT, "as variable name");
            r->name = id ? id->text : intern("_");
            if (accept(p, T_COLON)) r->texpr = parse_type(p);
            if (!expect(p, T_EQ, "in `let` (every binding must be initialised)")) break;
            skip_nl(p);
            r->a = parse_expr(p);
            r->span = join(sp, r->a->span);
            end_stmt(p);
            break;
        }
        case T_RETURN: {
            adv(p);
            r = mkstmt(p, S_RETURN, sp);
            if (!at(p, T_NEWLINE) && !at(p, T_RBRACE) && !at(p, T_EOF) &&
                !at(p, T_COMMA) && !at(p, T_RPAREN)) {
                r->a = parse_expr(p);
                r->span = join(sp, r->a->span);
            }
            break;
        }
        case T_BREAK:    adv(p); r = mkstmt(p, S_BREAK, sp); break;
        case T_CONTINUE: adv(p); r = mkstmt(p, S_CONTINUE, sp); break;
        case T_IF: {
            adv(p);
            if (at(p, T_LET)) {
                adv(p);
                r = mkstmt(p, S_IFLET, sp);
                Tok *id = expect(p, T_IDENT, "as binding name");
                r->name = id ? id->text : intern("_");
                expect(p, T_EQ, "in `if let`");
                int save = p_no_struct; p_no_struct = 1;
                r->a = parse_expr(p);
                p_no_struct = save;
            } else {
                r = mkstmt(p, S_IF, sp);
                int save = p_no_struct; p_no_struct = 1;
                r->a = parse_expr(p);
                p_no_struct = save;
            }
            r->then_s = parse_block(p);
            {
                int save = p->i;
                while (K(p) == T_NEWLINE) adv(p);
                if (!at(p, T_ELSE)) p->i = save;
            }
            if (at(p, T_ELSE)) {
                adv(p);
                if (at(p, T_IF)) r->else_s = parse_stmt(p);
                else r->else_s = parse_block(p);
            }
            r->span = join(sp, r->then_s->span);
            break;
        }
        case T_WHILE: {
            adv(p);
            r = mkstmt(p, S_WHILE, sp);
            int save = p_no_struct; p_no_struct = 1;
            r->a = parse_expr(p);
            p_no_struct = save;
            r->then_s = parse_block(p);
            r->span = join(sp, r->then_s->span);
            break;
        }
        case T_FOR: {
            adv(p);
            r = mkstmt(p, S_FOR, sp);
            Tok *id = expect(p, T_IDENT, "as loop variable");
            r->name = id ? id->text : intern("_");
            if (accept(p, T_COMMA)) {
                Tok *id2 = expect(p, T_IDENT, "as second loop variable");
                r->name2 = id2 ? id2->text : intern("_");
            }
            expect(p, T_IN, "in `for` loop");
            int save = p_no_struct; p_no_struct = 1;
            r->a = parse_expr(p);
            p_no_struct = save;
            r->then_s = parse_block(p);
            r->span = join(sp, r->then_s->span);
            break;
        }
        case T_LBRACE:
            r = parse_block(p);
            break;
        case T_NEWLINE:
            adv(p);
            p->depth--;
            return NULL;
        case T_SEMI:
            adv(p);
            p->depth--;
            return NULL;
        default: {
            Expr *e = parse_expr(p);
            TokKind k = K(p);
            if (k == T_EQ || k == T_PLUSEQ || k == T_MINUSEQ || k == T_STAREQ ||
                k == T_SLASHEQ || k == T_PERCENTEQ) {
                Span osp = cur(p)->span;
                adv(p);
                skip_nl(p);
                r = mkstmt(p, S_ASSIGN, sp);
                r->op = k;
                r->a = e;
                r->b = parse_expr(p);
                r->span = join(sp, r->b->span);
                if (!is_lvalue(e)) {
                    Diag *d = perr_d(p, e->span, "cannot assign to this expression");
                    diag_note(d, osp, "the left side of `=` must be a variable, field, or index");
                }
                end_stmt(p);
            } else {
                r = mkstmt(p, S_EXPR, e->span);
                r->a = e;
                end_stmt(p);
            }
            break;
        }
    }
    p->depth--;
    return r;
}

/* ---------------- declarations ---------------- */

static void parse_generics(P *p, Vec *out) {
    if (!at(p, T_LBRACKET)) return;
    adv(p);
    do {
        skip_nl(p);
        if (at(p, T_RBRACKET)) break;
        Tok *id = expect(p, T_IDENT, "as generic parameter");
        if (!id) break;
        vec_push(out, (void *)id->text);
        skip_nl(p);
    } while (accept(p, T_COMMA));
    expect(p, T_RBRACKET, "to close generic parameters");
}

static Decl *parse_decl(P *p, int is_pub, const char *doc) {
    Span sp = cur(p)->span;
    Decl *d = NEW(Decl);
    d->is_pub = is_pub;
    d->span = sp;
    d->doc = doc;
    d->mod = p->mod;

    switch (K(p)) {
        case T_USE: {
            adv(p);
            d->kind = D_USE;
            Buf path; memset(&path, 0, sizeof path);
            if (at(p, T_DOT) || at(p, T_DOTDOT)) {
                buf_str(&path, at(p, T_DOTDOT) ? ".." : ".");
                adv(p);
                if (!at(p, T_SLASH)) { perr(p, cur(p)->span, "expected `/` after `.` in module path"); break; }
            }
            const char *last = NULL;
            for (;;) {
                if (at(p, T_SLASH)) { adv(p); buf_u8(&path, '/'); continue; }
                if (at(p, T_IDENT)) {
                    Tok *id = adv(p);
                    buf_str(&path, id->text);
                    last = id->text;
                    if (at(p, T_SLASH)) continue;
                    break;
                }
                perr(p, cur(p)->span, "expected a module path segment, found %s", tokdesc(cur(p)));
                break;
            }
            buf_u8(&path, 0);
            d->path = intern((char *)path.data);
            buf_free(&path);
            if (accept(p, T_AS)) {
                Tok *a = expect(p, T_IDENT, "as import alias");
                d->alias = a ? a->text : last;
            } else d->alias = last;
            d->name = d->alias;
            d->span = join(sp, p->t[p->i - 1].span);
            end_stmt(p);
            break;
        }
        case T_CONST: {
            adv(p);
            d->kind = D_CONST;
            Tok *id = expect(p, T_IDENT, "as constant name");
            d->name = id ? id->text : intern("?");
            if (accept(p, T_COLON)) d->texpr = parse_type(p);
            expect(p, T_EQ, "in constant declaration");
            skip_nl(p);
            d->value = parse_expr(p);
            d->span = join(sp, d->value->span);
            end_stmt(p);
            break;
        }
        case T_TYPE: {
            adv(p);
            d->kind = D_ALIAS;
            Tok *id = expect(p, T_IDENT, "as type name");
            d->name = id ? id->text : intern("?");
            expect(p, T_EQ, "in type alias");
            d->texpr = parse_type(p);
            d->span = join(sp, d->texpr->span);
            end_stmt(p);
            break;
        }
        case T_STRUCT: {
            adv(p);
            d->kind = D_STRUCT;
            Tok *id = expect(p, T_IDENT, "as struct name");
            d->name = id ? id->text : intern("?");
            parse_generics(p, &d->generics);
            expect(p, T_LBRACE, "to open struct body");
            skip_nl(p);
            while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
                const char *fdoc = NULL;
                while (at(p, T_DOC)) { fdoc = adv(p)->text; skip_nl(p); }
                if (at(p, T_RBRACE)) break;
                Tok *fn = expect(p, T_IDENT, "as field name");
                if (!fn) break;
                StructField *f = NEW(StructField);
                f->name = fn->text; f->span = fn->span; f->doc = fdoc;
                expect(p, T_COLON, "after field name");
                f->type = parse_type(p);
                vec_push(&d->fields, f);
                skip_nl(p);
                accept(p, T_COMMA);
                skip_nl(p);
                if (p->panic) break;
            }
            Tok *r = expect(p, T_RBRACE, "to close struct body");
            if (r) d->span = join(sp, r->span);
            break;
        }
        case T_ENUM: {
            adv(p);
            d->kind = D_ENUM;
            Tok *id = expect(p, T_IDENT, "as enum name");
            d->name = id ? id->text : intern("?");
            parse_generics(p, &d->generics);
            expect(p, T_LBRACE, "to open enum body");
            skip_nl(p);
            int tag = 0;
            while (!at(p, T_RBRACE) && !at(p, T_EOF)) {
                const char *vdoc = NULL;
                while (at(p, T_DOC)) { vdoc = adv(p)->text; skip_nl(p); }
                if (at(p, T_RBRACE)) break;
                Tok *vn = expect(p, T_IDENT, "as variant name");
                if (!vn) break;
                EnumVariant *v = NEW(EnumVariant);
                v->name = vn->text; v->span = vn->span; v->tag = tag++; v->doc = vdoc;
                if (accept(p, T_LPAREN)) {
                    if (!at(p, T_RPAREN)) {
                        do { skip_nl(p); if (at(p,T_RPAREN)) break;
                             vec_push(&v->types, parse_type(p)); skip_nl(p);
                        } while (accept(p, T_COMMA));
                    }
                    expect(p, T_RPAREN, "to close variant payload");
                }
                vec_push(&d->variants, v);
                skip_nl(p);
                accept(p, T_COMMA);
                skip_nl(p);
                if (p->panic) break;
            }
            Tok *r = expect(p, T_RBRACE, "to close enum body");
            if (r) d->span = join(sp, r->span);
            break;
        }
        case T_TEST: {
            adv(p);
            d->kind = D_TEST;
            Tok *nm = expect(p, T_STR, "as test name");
            d->name = nm ? nm->text : intern("test");
            d->body = parse_block(p);
            d->span = join(sp, d->body->span);
            break;
        }
        case T_FN: {
            adv(p);
            d->kind = D_FN;
            Tok *id = expect(p, T_IDENT, "as function name");
            if (!id) break;
            if (at(p, T_LBRACKET) && K2(p) == T_IDENT) {
                /* Could be `fn List[T].push(...)` — a method on a generic type. */
                int save = p->i;
                Vec tmp; memset(&tmp, 0, sizeof tmp);
                parse_generics(p, &tmp);
                if (at(p, T_DOT)) {
                    adv(p);
                    d->recv = id->text;
                    d->generics = tmp;
                    Tok *m = expect(p, T_IDENT, "as method name");
                    d->name = m ? m->text : intern("?");
                } else {
                    p->i = save;
                    d->name = id->text;
                    parse_generics(p, &d->generics);
                }
            } else if (at(p, T_DOT)) {
                adv(p);
                d->recv = id->text;
                Tok *m = expect(p, T_IDENT, "as method name");
                d->name = m ? m->text : intern("?");
                parse_generics(p, &d->generics);
            } else {
                d->name = id->text;
                parse_generics(p, &d->generics);
            }
            expect(p, T_LPAREN, "to open parameter list");
            skip_nl(p);
            if (!at(p, T_RPAREN)) {
                int first = 1;
                do {
                    skip_nl(p);
                    if (at(p, T_RPAREN)) break;
                    if (first && at(p, T_SELF)) {
                        adv(p);
                        d->has_self = 1;
                        if (!d->recv)
                            perr(p, p->t[p->i - 1].span,
                                 "`self` is only allowed in a method (`fn Type.name(self, ...)`)");
                        first = 0;
                        skip_nl(p);
                        continue;
                    }
                    first = 0;
                    Tok *pn = expect(p, T_IDENT, "as parameter name");
                    if (!pn) break;
                    Param *pa = NEW(Param);
                    pa->name = pn->text; pa->span = pn->span;
                    if (!expect(p, T_COLON, "after parameter name (parameters need explicit types)")) break;
                    pa->type = parse_type(p);
                    vec_push(&d->params, pa);
                    skip_nl(p);
                } while (accept(p, T_COMMA));
            }
            expect(p, T_RPAREN, "to close parameter list");
            if (accept(p, T_ARROW)) d->ret = parse_type(p);
            if (p->panic) break;
            d->body = parse_block(p);
            d->span = join(sp, d->body ? d->body->span : sp);
            break;
        }
        default:
            perr(p, sp, "expected a declaration (`fn`, `struct`, `enum`, `const`, `type`, `use`, `test`), found %s",
                 tokdesc(cur(p)));
            return NULL;
    }
    return d;
}

int parse_module(TokList *tl, SrcFile *f, Module *m) {
    P p;
    memset(&p, 0, sizeof p);
    p.t = tl->toks; p.n = tl->n; p.i = 0; p.f = f; p.mod = m; p.ok = 1;
    p_no_struct = 0;

    skip_nl(&p);
    while (K(&p) != T_EOF) {
        const char *doc = NULL;
        Buf db; memset(&db, 0, sizeof db);
        while (at(&p, T_DOC)) {
            if (db.len) buf_u8(&db, '\n');
            buf_str(&db, adv(&p)->text);
            skip_nl(&p);
        }
        if (db.len) { buf_u8(&db, 0); doc = intern((char *)db.data); buf_free(&db); }
        if (K(&p) == T_EOF) break;
        int is_pub = accept(&p, T_PUB);
        Decl *d = parse_decl(&p, is_pub, doc);
        if (d) vec_push(&m->decls, d);
        if (p.panic) sync_decl(&p);
        skip_nl(&p);
    }
    return p.ok;
}
