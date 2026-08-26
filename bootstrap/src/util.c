/* util.c — arena, containers, source files, diagnostics. */
#include "vela.h"
#include <stdarg.h>
#include <errno.h>

Arena g_arena;

struct ArenaBlock {
    ArenaBlock *next;
    size_t used, cap;
    char data[];
};

#define ARENA_BLOCK (1u << 20)

void *arena_alloc(Arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    if (!a->head || a->head->used + n > a->head->cap) {
        size_t cap = n > ARENA_BLOCK ? n : ARENA_BLOCK;
        ArenaBlock *b = (ArenaBlock *)calloc(1, sizeof(ArenaBlock) + cap);
        if (!b) fatal("out of memory");
        b->cap = cap; b->used = 0; b->next = a->head; a->head = b;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    a->total += n;
    return p;
}

void arena_free(Arena *a) {
    ArenaBlock *b = a->head;
    while (b) { ArenaBlock *n = b->next; free(b); b = n; }
    a->head = NULL; a->total = 0;
}

char *arena_strdup(Arena *a, const char *s) { return arena_strndup(a, s, strlen(s)); }

char *arena_strndup(Arena *a, const char *s, size_t n) {
    char *p = (char *)arena_alloc(a, n + 1);
    memcpy(p, s, n); p[n] = 0;
    return p;
}

void vec_push(Vec *v, void *p) {
    if (v->len == v->cap) {
        int nc = v->cap ? v->cap * 2 : 8;
        void **nd = (void **)malloc(sizeof(void *) * (size_t)nc);
        if (!nd) fatal("out of memory");
        if (v->data) { memcpy(nd, v->data, sizeof(void *) * (size_t)v->len); free(v->data); }
        v->data = nd; v->cap = nc;
    }
    v->data[v->len++] = p;
}

void vec_insert(Vec *v, int idx, void *p) {
    vec_push(v, NULL);
    for (int i = v->len - 1; i > idx; i--) v->data[i] = v->data[i - 1];
    v->data[idx] = p;
}

void buf_put(Buf *b, const void *p, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 256;
        while (nc < b->len + n) nc *= 2;
        uint8_t *nd = (uint8_t *)realloc(b->data, nc);
        if (!nd) fatal("out of memory");
        b->data = nd; b->cap = nc;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
void buf_u8(Buf *b, uint8_t v)   { buf_put(b, &v, 1); }
void buf_u16(Buf *b, uint16_t v) { buf_put(b, &v, 2); }
void buf_u32(Buf *b, uint32_t v) { buf_put(b, &v, 4); }
void buf_u64(Buf *b, uint64_t v) { buf_put(b, &v, 8); }
void buf_str(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }
void buf_zero(Buf *b, size_t n) { while (n--) buf_u8(b, 0); }
void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[4096];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n < sizeof tmp) { buf_put(b, tmp, (size_t)n); return; }
    char *big = (char *)malloc((size_t)n + 1);
    va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
    buf_put(b, big, (size_t)n);
    free(big);
}

/* ---------------- interning ---------------- */

#define INTERN_BUCKETS 4096
typedef struct IStr { struct IStr *next; size_t len; char s[]; } IStr;
static IStr *itab[INTERN_BUCKETS];

static uint32_t hash_bytes(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

const char *intern_n(const char *s, size_t n) {
    uint32_t h = hash_bytes(s, n) % INTERN_BUCKETS;
    for (IStr *p = itab[h]; p; p = p->next)
        if (p->len == n && memcmp(p->s, s, n) == 0) return p->s;
    IStr *p = (IStr *)arena_alloc(&g_arena, sizeof(IStr) + n + 1);
    p->len = n; memcpy(p->s, s, n); p->s[n] = 0;
    p->next = itab[h]; itab[h] = p;
    return p->s;
}
const char *intern(const char *s) { return intern_n(s, strlen(s)); }

/* ---------------- source files ---------------- */

static Vec g_files;

SrcFile *src_get(int id) {
    if (id < 0 || id >= g_files.len) return NULL;
    return VEC_AT(&g_files, SrcFile, id);
}

SrcFile *src_load(const char *path, const char *display) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    char *txt = (char *)arena_alloc(&g_arena, (size_t)sz + 2);
    size_t rd = fread(txt, 1, (size_t)sz, f);
    fclose(f);
    txt[rd] = '\n'; txt[rd + 1] = 0;
    SrcFile *sf = NEW(SrcFile);
    sf->path = intern(path);
    sf->display = intern(display ? display : path);
    sf->text = txt;
    sf->len = rd;
    sf->id = g_files.len;
    vec_push(&g_files, sf);
    return sf;
}

/* ---------------- diagnostics ---------------- */

static Vec g_diags;
static int g_nerr, g_nwarn;
static int g_color = 1;
static int g_maxerr = 25;

void diag_set_color(int on) { g_color = on; }
void diag_set_max_errors(int n) { g_maxerr = n; }
void diag_reset(void) { g_diags.len = 0; g_nerr = g_nwarn = 0; }
int diag_error_count(void) { return g_nerr; }
int diag_warn_count(void) { return g_nwarn; }

static char *vfmt(const char *fmt, va_list ap) {
    va_list ap2; va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    char *s = (char *)arena_alloc(&g_arena, (size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, fmt, ap);
    return s;
}

Diag *diag_add(DiagLevel lv, Span sp, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char *msg = vfmt(fmt, ap);
    va_end(ap);
    if (lv == DIAG_ERROR) {
        /* de-duplicate identical errors at the same location */
        for (int i = 0; i < g_diags.len; i++) {
            Diag *d = VEC_AT(&g_diags, Diag, i);
            if (d->level == DIAG_ERROR && d->span.file == sp.file &&
                d->span.off == sp.off && strcmp(d->msg, msg) == 0)
                return d;
        }
        g_nerr++;
    } else if (lv == DIAG_WARN) g_nwarn++;
    Diag *d = NEW(Diag);
    d->level = lv; d->span = sp; d->msg = msg; d->next = NULL;
    vec_push(&g_diags, d);
    return d;
}

void diag_note(Diag *d, Span sp, const char *fmt, ...) {
    if (!d) return;
    va_list ap; va_start(ap, fmt);
    char *msg = vfmt(fmt, ap);
    va_end(ap);
    Diag *n = NEW(Diag);
    n->level = DIAG_NOTE; n->span = sp; n->msg = msg; n->next = NULL;
    while (d->next) d = d->next;
    d->next = n;
}

static const char *C_RED  = "\033[1;31m";
static const char *C_YEL  = "\033[1;33m";
static const char *C_CYN  = "\033[1;36m";
static const char *C_BLU  = "\033[1;34m";
static const char *C_BOLD = "\033[1m";
static const char *C_DIM  = "\033[2m";
static const char *C_OFF  = "\033[0m";

static const char *col(const char *c) { return g_color ? c : ""; }

static void line_bounds(SrcFile *f, int off, int *start, int *end) {
    int s = off;
    if (s > (int)f->len) s = (int)f->len;
    while (s > 0 && f->text[s - 1] != '\n') s--;
    int e = off;
    while (e < (int)f->len && f->text[e] != '\n') e++;
    *start = s; *end = e;
}

static void render_snippet(FILE *out, Span sp, const char *marker_color, char marker) {
    SrcFile *f = src_get(sp.file);
    if (!f) return;
    int ls, le;
    line_bounds(f, sp.off, &ls, &le);
    int lnw = 1, l = sp.line;
    while (l >= 10) { l /= 10; lnw++; }
    if (lnw < 3) lnw = 3;

    fprintf(out, "%s%*s-->%s %s:%d:%d\n", col(C_BLU), lnw, "", col(C_OFF),
            f->display, sp.line, sp.col);
    fprintf(out, "%s%*s |%s\n", col(C_BLU), lnw, "", col(C_OFF));
    fprintf(out, "%s%*d |%s ", col(C_BLU), lnw, sp.line, col(C_OFF));
    for (int i = ls; i < le; i++) fputc(f->text[i] == '\t' ? ' ' : f->text[i], out);
    fputc('\n', out);
    fprintf(out, "%s%*s |%s ", col(C_BLU), lnw, "", col(C_OFF));
    for (int i = ls; i < sp.off && i < le; i++) fputc(' ', out);
    fprintf(out, "%s", col(marker_color));
    int n = sp.len > 0 ? sp.len : 1;
    if (sp.off + n > le + 1) n = le - sp.off + 1;
    if (n < 1) n = 1;
    for (int i = 0; i < n; i++) fputc(marker, out);
    fprintf(out, "%s\n", col(C_OFF));
}

void diag_flush(FILE *out) {
    int shown = 0;
    for (int i = 0; i < g_diags.len; i++) {
        Diag *d = VEC_AT(&g_diags, Diag, i);
        if (d->level == DIAG_ERROR && shown >= g_maxerr) {
            fprintf(out, "%serror%s: too many errors; stopping after %d\n",
                    col(C_RED), col(C_OFF), g_maxerr);
            break;
        }
        if (d->level == DIAG_ERROR) shown++;
        const char *lbl, *c;
        switch (d->level) {
            case DIAG_ERROR: lbl = "error"; c = C_RED; break;
            case DIAG_WARN:  lbl = "warning"; c = C_YEL; break;
            case DIAG_NOTE:  lbl = "note"; c = C_CYN; break;
            default:         lbl = "help"; c = C_CYN; break;
        }
        fprintf(out, "%s%s%s%s: %s%s\n", col(c), lbl, col(C_OFF), col(C_BOLD),
                d->msg, col(C_OFF));
        if (d->span.file >= 0)
            render_snippet(out, d->span, c, d->level == DIAG_ERROR ? '^' : '-');
        for (Diag *n = d->next; n; n = n->next) {
            const char *nl = n->level == DIAG_HELP ? "help" : "note";
            fprintf(out, "%s   = %s%s%s: %s\n", col(C_DIM), nl, col(C_OFF), col(C_DIM), n->msg);
            fprintf(out, "%s", col(C_OFF));
            if (n->span.file >= 0) render_snippet(out, n->span, C_CYN, '-');
        }
        fputc('\n', out);
    }
    g_diags.len = 0;
}

void fatal(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "%serror%s: ", col(C_RED), col(C_OFF));
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}
