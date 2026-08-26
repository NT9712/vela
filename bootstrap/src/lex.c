/* lex.c — Vela lexer.
 *
 * Produces a token stream with newline tokens (Vela is newline-terminated).
 * Handles nested block comments, doc comments, all numeric bases, character
 * literals, and string interpolation (which recursively lexes sub-expressions).
 */
#include "vela.h"
#include <stdarg.h>
#include <errno.h>

const char *tok_names[T_MAX] = {
    [T_EOF] = "end of file", [T_NEWLINE] = "newline",
    [T_IDENT] = "identifier", [T_INT] = "integer", [T_FLOAT] = "float",
    [T_STR] = "string", [T_CHAR] = "character", [T_INTERP_STR] = "string",
    [T_AND] = "and", [T_AS] = "as", [T_BREAK] = "break", [T_CONST] = "const",
    [T_CONTINUE] = "continue", [T_ELSE] = "else", [T_ENUM] = "enum",
    [T_FALSE] = "false", [T_FN] = "fn", [T_FOR] = "for", [T_IF] = "if",
    [T_IN] = "in", [T_LET] = "let", [T_MATCH] = "match", [T_MUT] = "mut",
    [T_NIL] = "nil", [T_NOT] = "not", [T_OR] = "or", [T_PUB] = "pub",
    [T_RETURN] = "return", [T_STRUCT] = "struct", [T_TEST] = "test",
    [T_TRUE] = "true", [T_TYPE] = "type", [T_USE] = "use", [T_SELF] = "self",
    [T_WHILE] = "while",
    [T_LPAREN] = "(", [T_RPAREN] = ")", [T_LBRACE] = "{", [T_RBRACE] = "}",
    [T_LBRACKET] = "[", [T_RBRACKET] = "]", [T_COMMA] = ",", [T_DOT] = ".",
    [T_DOTDOT] = "..", [T_DOTDOTEQ] = "..=", [T_COLON] = ":", [T_SEMI] = ";",
    [T_ARROW] = "->", [T_FATARROW] = "=>", [T_PLUS] = "+", [T_MINUS] = "-",
    [T_STAR] = "*", [T_SLASH] = "/", [T_PERCENT] = "%", [T_PLUSEQ] = "+=",
    [T_MINUSEQ] = "-=", [T_STAREQ] = "*=", [T_SLASHEQ] = "/=",
    [T_PERCENTEQ] = "%=", [T_EQ] = "=", [T_EQEQ] = "==", [T_BANGEQ] = "!=",
    [T_LT] = "<", [T_LE] = "<=", [T_GT] = ">", [T_GE] = ">=", [T_AMP] = "&",
    [T_PIPE] = "|", [T_CARET] = "^", [T_TILDE] = "~", [T_SHL] = "<<",
    [T_SHR] = ">>", [T_QUESTION] = "?", [T_QQ] = "??", [T_BANG] = "!",
    [T_AT] = "@", [T_UNDERSCORE] = "_", [T_DOC] = "doc comment",
};

typedef struct Lexer Lexer;
static void lex_one_token(Lexer *L);

struct Lexer {
    SrcFile *f;
    const char *p;
    const char *start;
    const char *end;
    int line, linestart;
    TokList *out;
    int ok;
};

static struct { const char *kw; TokKind k; } keywords[] = {
    {"and", T_AND}, {"as", T_AS}, {"break", T_BREAK}, {"const", T_CONST},
    {"continue", T_CONTINUE}, {"else", T_ELSE}, {"enum", T_ENUM},
    {"false", T_FALSE}, {"fn", T_FN}, {"for", T_FOR}, {"if", T_IF},
    {"in", T_IN}, {"let", T_LET}, {"match", T_MATCH}, {"mut", T_MUT},
    {"nil", T_NIL}, {"not", T_NOT}, {"or", T_OR}, {"pub", T_PUB},
    {"return", T_RETURN}, {"struct", T_STRUCT}, {"test", T_TEST},
    {"true", T_TRUE}, {"type", T_TYPE}, {"use", T_USE}, {"self", T_SELF},
    {"while", T_WHILE},
    {NULL, T_EOF}
};

static void tl_push(TokList *tl, Tok t) {
    if (tl->n == tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 256;
        tl->toks = (Tok *)realloc(tl->toks, sizeof(Tok) * (size_t)tl->cap);
        if (!tl->toks) fatal("out of memory");
    }
    tl->toks[tl->n++] = t;
}

static Span mkspan(Lexer *L, const char *from) {
    Span s;
    s.file = L->f->id;
    s.off = (int)(from - L->start);
    s.len = (int)(L->p - from);
    s.line = L->line;
    s.col = (int)(from - L->start) - L->linestart + 1;
    return s;
}

static void lerr(Lexer *L, const char *from, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char msg[512];
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    Span sp = mkspan(L, from);
    if (sp.len < 1) sp.len = 1;
    diag_add(DIAG_ERROR, sp, "%s", msg);
    L->ok = 0;
}

static int is_ident_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_ident(int c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Skip whitespace/comments. Returns 1 if a newline was crossed. */
static int skip_trivia(Lexer *L) {
    int nl = 0;
    for (;;) {
        char c = *L->p;
        if (c == ' ' || c == '\t' || c == '\r') { L->p++; continue; }
        if (c == '\\' && L->p[1] == '\n') {   /* explicit line continuation */
            L->p += 2; L->line++; L->linestart = (int)(L->p - L->start); continue;
        }
        if (c == '\n') { return nl; }
        if (c == '/' && L->p[1] == '/') {
            if (L->p[2] == '/') return nl;   /* doc comment: handled as token */
            while (*L->p && *L->p != '\n') L->p++;
            continue;
        }
        if (c == '/' && L->p[1] == '*') {
            const char *from = L->p;
            int depth = 0;
            while (L->p < L->end) {
                if (L->p[0] == '/' && L->p[1] == '*') { depth++; L->p += 2; }
                else if (L->p[0] == '*' && L->p[1] == '/') {
                    depth--; L->p += 2;
                    if (depth == 0) break;
                } else {
                    if (*L->p == '\n') { L->line++; L->linestart = (int)(L->p - L->start) + 1; nl = 1; }
                    L->p++;
                }
            }
            if (depth != 0) lerr(L, from, "unterminated block comment");
            continue;
        }
        break;
    }
    return nl;
}

/* Unescape one escape sequence at L->p (points after the backslash). */
static int read_escape(Lexer *L, const char *from, int *out) {
    char c = *L->p++;
    switch (c) {
        case 'n': *out = '\n'; return 1;
        case 'r': *out = '\r'; return 1;
        case 't': *out = '\t'; return 1;
        case '0': *out = 0; return 1;
        case '\\': *out = '\\'; return 1;
        case '"': *out = '"'; return 1;
        case '\'': *out = '\''; return 1;
        case '{': *out = '{'; return 1;
        case '}': *out = '}'; return 1;
        case 'x': {
            int h1 = hexval(L->p[0]), h2 = hexval(L->p[1]);
            if (h1 < 0 || h2 < 0) { lerr(L, from, "`\\x` needs two hex digits"); return 0; }
            L->p += 2; *out = h1 * 16 + h2; return 1;
        }
        default:
            L->p--;
            lerr(L, from, "unknown escape sequence `\\%c`", c);
            L->p++;
            return 0;
    }
}

static void lex_number(Lexer *L, const char *from) {
    Tok t; memset(&t, 0, sizeof t);
    int base = 10;
    if (*L->p == '0' && (L->p[1] == 'x' || L->p[1] == 'X')) { base = 16; L->p += 2; }
    else if (*L->p == '0' && (L->p[1] == 'b' || L->p[1] == 'B')) { base = 2; L->p += 2; }
    else if (*L->p == '0' && (L->p[1] == 'o' || L->p[1] == 'O')) { base = 8; L->p += 2; }

    if (base != 10) {
        uint64_t v = 0; int any = 0, overflow = 0;
        while (L->p < L->end) {
            char c = *L->p;
            if (c == '_') { L->p++; continue; }
            int d = hexval(c);
            if (d < 0 || d >= base) break;
            if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) overflow = 1;
            v = v * (uint64_t)base + (uint64_t)d;
            L->p++; any = 1;
        }
        if (!any) { lerr(L, from, "expected digits after base prefix"); }
        if (overflow) lerr(L, from, "integer literal is too large for `Int` (64-bit)");
        if (is_ident_start(*L->p)) lerr(L, from, "invalid suffix on integer literal");
        t.kind = T_INT; t.ival = (int64_t)v;
        t.span = mkspan(L, from);
        tl_push(L->out, t);
        return;
    }

    char nbuf[128]; int ni = 0;
    int isf = 0;
    while (L->p < L->end && (is_digit(*L->p) || *L->p == '_')) {
        if (*L->p != '_' && ni < 120) nbuf[ni++] = *L->p;
        L->p++;
    }
    if (*L->p == '.' && is_digit(L->p[1])) {
        isf = 1; nbuf[ni++] = '.'; L->p++;
        while (L->p < L->end && (is_digit(*L->p) || *L->p == '_')) {
            if (*L->p != '_' && ni < 120) nbuf[ni++] = *L->p;
            L->p++;
        }
    }
    if ((*L->p == 'e' || *L->p == 'E') &&
        (is_digit(L->p[1]) || ((L->p[1] == '+' || L->p[1] == '-') && is_digit(L->p[2])))) {
        isf = 1; nbuf[ni++] = 'e'; L->p++;
        if (*L->p == '+' || *L->p == '-') nbuf[ni++] = *L->p++;
        while (L->p < L->end && is_digit(*L->p)) { if (ni < 120) nbuf[ni++] = *L->p; L->p++; }
    }
    nbuf[ni] = 0;
    if (is_ident_start(*L->p)) {
        const char *sfx = L->p;
        while (is_ident(*L->p)) L->p++;
        lerr(L, from, "invalid suffix `%.*s` on number literal",
             (int)(L->p - sfx), sfx);
    }
    if (isf) {
        t.kind = T_FLOAT; t.fval = strtod(nbuf, NULL);
    } else {
        errno = 0;
        char *endp;
        unsigned long long v = strtoull(nbuf, &endp, 10);
        if (v > (unsigned long long)INT64_MAX)
            lerr(L, from, "integer literal `%s` is too large for `Int` (max 9223372036854775807)", nbuf);
        t.kind = T_INT; t.ival = (int64_t)v;
    }
    t.span = mkspan(L, from);
    tl_push(L->out, t);
}

/* Lex an interpolation expression: from '{' to matching '}'. */
static StrPiece *lex_interp_expr(Lexer *L, const char *from) {
    L->p++;   /* consume '{' */
    const char *estart = L->p;
    int depth = 1;
    while (L->p < L->end) {
        char c = *L->p;
        if (c == '"') {  /* nested string */
            L->p++;
            while (L->p < L->end && *L->p != '"') {
                if (*L->p == '\\') L->p++;
                L->p++;
            }
            if (L->p < L->end) L->p++;
            continue;
        }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) break; }
        else if (c == '\n') { lerr(L, from, "unterminated `{` in string interpolation (write `{{` for a literal brace)"); return NULL; }
        L->p++;
    }
    if (L->p >= L->end) { lerr(L, from, "unterminated `{` in string interpolation"); return NULL; }
    size_t elen = (size_t)(L->p - estart);
    if (elen == 0) {
        lerr(L, from, "empty `{}` in string; write `{{` for a literal brace");
        L->p++;
        return NULL;
    }
    L->p++;   /* consume '}' */

    /* Recursively lex the expression text as its own token list. */
    SrcFile sub = *L->f;
    Lexer L2;
    memset(&L2, 0, sizeof L2);
    TokList tl2; memset(&tl2, 0, sizeof tl2);
    L2.f = L->f;
    L2.start = L->start;
    L2.end = estart + elen;
    L2.p = estart;
    L2.line = L->line;
    L2.linestart = L->linestart;
    L2.out = &tl2;
    L2.ok = 1;
    (void)sub;
    while (L2.p < L2.end) {
        skip_trivia(&L2);
        if (L2.p >= L2.end) break;
        if (*L2.p == '\n') { L2.p++; continue; }
        /* reuse the main scanner for the sub-expression */
        lex_one_token(&L2);
    }
    if (!L2.ok) L->ok = 0;
    Tok eof; memset(&eof, 0, sizeof eof);
    eof.kind = T_EOF; eof.span = mkspan(L, from);
    tl_push(&tl2, eof);

    StrPiece *sp = NEW(StrPiece);
    sp->is_expr = 1;
    sp->toks = tl2.toks;
    sp->ntoks = tl2.n;
    return sp;
}

static void lex_string(Lexer *L, const char *from) {
    L->p++;  /* opening quote */
    Buf lit; memset(&lit, 0, sizeof lit);
    StrPiece *head = NULL, *tail = NULL;
    int has_interp = 0;

    for (;;) {
        if (L->p >= L->end) { lerr(L, from, "unterminated string literal"); break; }
        char c = *L->p;
        if (c == '"') { L->p++; break; }
        if (c == '\n') { lerr(L, from, "unterminated string literal (newline in string)"); break; }
        if (c == '\\') {
            if (L->p[1] == 'u' && L->p[2] == '{') {
                const char *esc = L->p;
                L->p += 3;
                uint32_t cp = 0;
                int digits = 0;
                while (L->p < L->end && *L->p != '}') {
                    int h = hexval(*L->p);
                    if (h < 0) break;
                    cp = cp * 16 + (uint32_t)h;
                    digits++;
                    L->p++;
                }
                if (*L->p != '}' || digits == 0 || digits > 6 || cp > 0x10FFFF) {
                    lerr(L, esc, "`\\u{...}` needs 1 to 6 hex digits naming a Unicode code point");
                    if (*L->p == '}') L->p++;
                    continue;
                }
                L->p++;
                if (cp < 0x80) buf_u8(&lit, (uint8_t)cp);
                else if (cp < 0x800) {
                    buf_u8(&lit, (uint8_t)(0xC0 | (cp >> 6)));
                    buf_u8(&lit, (uint8_t)(0x80 | (cp & 0x3F)));
                } else if (cp < 0x10000) {
                    buf_u8(&lit, (uint8_t)(0xE0 | (cp >> 12)));
                    buf_u8(&lit, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
                    buf_u8(&lit, (uint8_t)(0x80 | (cp & 0x3F)));
                } else {
                    buf_u8(&lit, (uint8_t)(0xF0 | (cp >> 18)));
                    buf_u8(&lit, (uint8_t)(0x80 | ((cp >> 12) & 0x3F)));
                    buf_u8(&lit, (uint8_t)(0x80 | ((cp >> 6) & 0x3F)));
                    buf_u8(&lit, (uint8_t)(0x80 | (cp & 0x3F)));
                }
                continue;
            }
            L->p++;
            int ch;
            if (read_escape(L, from, &ch)) buf_u8(&lit, (uint8_t)ch);
            continue;
        }
        if (c == '{') {
            if (L->p[1] == '{') { buf_u8(&lit, '{'); L->p += 2; continue; }
            has_interp = 1;
            if (lit.len > 0) {
                StrPiece *t = NEW(StrPiece);
                t->is_expr = 0;
                t->text = intern_n((char *)lit.data, lit.len);
                if (tail) tail->next = t; else head = t;
                tail = t;
                lit.len = 0;
            }
            StrPiece *e = lex_interp_expr(L, from);
            if (!e) break;
            if (tail) tail->next = e; else head = e;
            tail = e;
            continue;
        }
        if (c == '}' && L->p[1] == '}') { buf_u8(&lit, '}'); L->p += 2; continue; }
        buf_u8(&lit, (uint8_t)c);
        L->p++;
    }

    Tok t; memset(&t, 0, sizeof t);
    if (has_interp) {
        if (lit.len > 0) {
            StrPiece *tp = NEW(StrPiece);
            tp->is_expr = 0;
            tp->text = intern_n((char *)lit.data, lit.len);
            if (tail) tail->next = tp; else head = tp;
            tail = tp;
        }
        t.kind = T_INTERP_STR;
        t.pieces = head;
    } else {
        t.kind = T_STR;
        t.text = intern_n(lit.data ? (char *)lit.data : "", lit.len);
        t.ival = (int64_t)lit.len;
    }
    buf_free(&lit);
    t.span = mkspan(L, from);
    tl_push(L->out, t);
}

static void lex_char(Lexer *L, const char *from) {
    L->p++;
    int v = 0;
    if (L->p >= L->end || *L->p == '\n') { lerr(L, from, "unterminated character literal"); return; }
    if (*L->p == '\\') { L->p++; if (!read_escape(L, from, &v)) return; }
    else v = (unsigned char)*L->p++;
    if (*L->p != '\'') {
        lerr(L, from, "character literal must contain exactly one character");
        while (L->p < L->end && *L->p != '\'' && *L->p != '\n') L->p++;
        if (*L->p == '\'') L->p++;
        return;
    }
    L->p++;
    Tok t; memset(&t, 0, sizeof t);
    t.kind = T_CHAR; t.ival = v & 0xff;
    t.span = mkspan(L, from);
    tl_push(L->out, t);
}

static void lex_one_token(Lexer *L) {
    const char *from = L->p;
    char c = *L->p;
    Tok t; memset(&t, 0, sizeof t);

    if (c == '/' && L->p[1] == '/' && L->p[2] == '/') {
        L->p += 3;
        if (*L->p == ' ') L->p++;
        const char *ds = L->p;
        while (L->p < L->end && *L->p != '\n') L->p++;
        t.kind = T_DOC;
        t.text = intern_n(ds, (size_t)(L->p - ds));
        t.span = mkspan(L, from);
        tl_push(L->out, t);
        return;
    }

    if (is_ident_start(c)) {
        while (L->p < L->end && is_ident(*L->p)) L->p++;
        size_t n = (size_t)(L->p - from);
        const char *id = intern_n(from, n);
        t.kind = T_IDENT; t.text = id;
        for (int i = 0; keywords[i].kw; i++) {
            if (strlen(keywords[i].kw) == n && memcmp(keywords[i].kw, from, n) == 0) {
                t.kind = keywords[i].k; break;
            }
        }
        if (n == 1 && from[0] == '_') t.kind = T_UNDERSCORE;
        t.span = mkspan(L, from);
        tl_push(L->out, t);
        return;
    }
    if (is_digit(c)) { lex_number(L, from); return; }
    if (c == '"') { lex_string(L, from); return; }
    if (c == '\'') { lex_char(L, from); return; }

    L->p++;
    TokKind k = T_EOF;
    char n1 = *L->p;
    switch (c) {
        case '(': k = T_LPAREN; break;
        case ')': k = T_RPAREN; break;
        case '{': k = T_LBRACE; break;
        case '}': k = T_RBRACE; break;
        case '[': k = T_LBRACKET; break;
        case ']': k = T_RBRACKET; break;
        case ',': k = T_COMMA; break;
        case ';': k = T_SEMI; break;
        case ':': k = T_COLON; break;
        case '@': k = T_AT; break;
        case '~': k = T_TILDE; break;
        case '^': k = T_CARET; break;
        case '&': k = T_AMP; break;
        case '|': k = T_PIPE; break;
        case '.':
            if (n1 == '.' && L->p[1] == '=') { L->p += 2; k = T_DOTDOTEQ; }
            else if (n1 == '.') { L->p++; k = T_DOTDOT; }
            else k = T_DOT;
            break;
        case '+': if (n1 == '=') { L->p++; k = T_PLUSEQ; } else k = T_PLUS; break;
        case '-': if (n1 == '=') { L->p++; k = T_MINUSEQ; }
                  else if (n1 == '>') { L->p++; k = T_ARROW; } else k = T_MINUS; break;
        case '*': if (n1 == '=') { L->p++; k = T_STAREQ; } else k = T_STAR; break;
        case '/': if (n1 == '=') { L->p++; k = T_SLASHEQ; } else k = T_SLASH; break;
        case '%': if (n1 == '=') { L->p++; k = T_PERCENTEQ; } else k = T_PERCENT; break;
        case '=': if (n1 == '=') { L->p++; k = T_EQEQ; }
                  else if (n1 == '>') { L->p++; k = T_FATARROW; } else k = T_EQ; break;
        case '!': if (n1 == '=') { L->p++; k = T_BANGEQ; } else k = T_BANG; break;
        case '<': if (n1 == '=') { L->p++; k = T_LE; }
                  else if (n1 == '<') { L->p++; k = T_SHL; } else k = T_LT; break;
        case '>': if (n1 == '=') { L->p++; k = T_GE; }
                  else if (n1 == '>') { L->p++; k = T_SHR; } else k = T_GT; break;
        case '?': if (n1 == '?') { L->p++; k = T_QQ; } else k = T_QUESTION; break;
        default:
            lerr(L, from, "unexpected character `%c` (0x%02x)", (c >= 32 && c < 127) ? c : '?', (unsigned char)c);
            return;
    }
    t.kind = k;
    t.span = mkspan(L, from);
    tl_push(L->out, t);
}

int lex_file(SrcFile *f, TokList *out) {
    Lexer L;
    memset(&L, 0, sizeof L);
    L.f = f;
    L.start = f->text;
    L.p = f->text;
    L.end = f->text + f->len;
    L.line = 1;
    L.linestart = 0;
    L.out = out;
    L.ok = 1;

    while (L.p < L.end) {
        skip_trivia(&L);
        if (L.p >= L.end) break;
        if (*L.p == '\n') {
            const char *from = L.p;
            L.p++;
            L.line++;
            L.linestart = (int)(L.p - L.start);
            Tok t; memset(&t, 0, sizeof t);
            t.kind = T_NEWLINE;
            t.span = mkspan(&L, from);
            t.span.len = 1;
            /* Collapse runs of newlines into one token; the number of extra
               blank lines is kept in ival so the formatter can preserve
               paragraph breaks. */
            int blanks = 0;
            for (;;) {
                skip_trivia(&L);
                if (L.p < L.end && *L.p == '\n') {
                    L.p++; L.line++; L.linestart = (int)(L.p - L.start); blanks++;
                } else break;
            }
            t.ival = blanks;
            tl_push(out, t);
            continue;
        }
        lex_one_token(&L);
    }
    Tok t; memset(&t, 0, sizeof t);
    t.kind = T_NEWLINE; t.span = mkspan(&L, L.p);
    tl_push(out, t);
    t.kind = T_EOF; t.span = mkspan(&L, L.p);
    tl_push(out, t);
    return L.ok;
}
