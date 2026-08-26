/* main.c — velac, the Vela bootstrap compiler driver.
 *
 * Usage: velac [options] <file.vela>
 *
 * velac performs whole-program compilation: it loads the entry module and
 * everything it transitively imports (including the standard library, which is
 * itself written in Vela), type-checks and monomorphises the lot, lowers it to
 * Vela IR, optimises, and emits a static x86-64 ELF64 executable.
 */
#define _POSIX_C_SOURCE 200809L
#include "vela.h"
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <stdarg.h>

static const char *opt_out = NULL;
static int opt_emit_ir = 0, opt_emit_tokens = 0, opt_emit_ast = 0;
static int opt_check_only = 0, opt_time = 0, opt_quiet = 0, opt_werror = 0;
static const char *opt_root = NULL;

/* ------------------------------------------------------------------ */
/* module loading                                                       */
/* ------------------------------------------------------------------ */

static char velaroot[1024];

static int file_exists(const char *p) {
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static void dirname_of(const char *path, char *out, size_t n) {
    const char *slash = strrchr(path, '/');
    if (!slash) { snprintf(out, n, "."); return; }
    size_t len = (size_t)(slash - path);
    if (len >= n) len = n - 1;
    memcpy(out, path, len);
    out[len] = 0;
}

/* Normalise a path in place: collapse `a/./b` and `a/b/../c`. */
static void normalize(char *p) {
    char out[1024];
    int o = 0;
    int seg_start[64], nseg = 0;
    int i = 0;
    if (p[0] == '/') { out[o++] = '/'; i = 1; }
    for (; p[i];) {
        if (p[i] == '/') { if (o && out[o-1] != '/') out[o++] = '/'; i++; continue; }
        int j = i;
        while (p[j] && p[j] != '/') j++;
        int len = j - i;
        if (len == 1 && p[i] == '.') { i = j; continue; }
        if (len == 2 && p[i] == '.' && p[i+1] == '.') {
            if (nseg > 0) { o = seg_start[--nseg]; if (o > 1 && out[o-1] == '/') o--; }
            else if (out[0] != '/') { if (o && out[o-1] != '/') out[o++] = '/'; memcpy(out + o, "..", 2); o += 2; }
            i = j;
            continue;
        }
        if (o && out[o-1] != '/') out[o++] = '/';
        if (nseg < 64) seg_start[nseg++] = o;
        if (o + len < 1000) { memcpy(out + o, p + i, (size_t)len); o += len; }
        i = j;
    }
    out[o] = 0;
    strcpy(p, out[0] ? out : ".");
}

Module *load_module(const char *modpath, const char *fromfile, Span sp) {
    if (!modpath || !*modpath) return NULL;
    /* already loaded? */
    for (int i = 0; i < g_unit.modules.len; i++) {
        Module *m = VEC_AT(&g_unit.modules, Module, i);
        if (m->modpath == intern(modpath)) return m;
    }

    char path[1024];
    path[0] = 0;
    const char *display = modpath;

    if (modpath[0] == '.') {
        char dir[1024];
        dirname_of(fromfile ? fromfile : ".", dir, sizeof dir);
        snprintf(path, sizeof path, "%s/%s.vela", dir, modpath);
        normalize(path);
    } else if (strncmp(modpath, "std/", 4) == 0 || strcmp(modpath, "core") == 0 ||
               strcmp(modpath, "prelude") == 0) {
        if (strcmp(modpath, "core") == 0)
            snprintf(path, sizeof path, "%s/lib/core/core.vela", velaroot);
        else if (strcmp(modpath, "prelude") == 0)
            snprintf(path, sizeof path, "%s/lib/core/prelude.vela", velaroot);
        else
            snprintf(path, sizeof path, "%s/lib/%s.vela", velaroot, modpath);
    } else {
        /* package: deps/<pkg>/src/<rest>.vela, searched from each root */
        const char *slash = strchr(modpath, '/');
        char pkg[256], rest[512];
        if (slash) {
            size_t n = (size_t)(slash - modpath);
            if (n >= sizeof pkg) n = sizeof pkg - 1;
            memcpy(pkg, modpath, n); pkg[n] = 0;
            snprintf(rest, sizeof rest, "%s", slash + 1);
        } else {
            snprintf(pkg, sizeof pkg, "%s", modpath);
            snprintf(rest, sizeof rest, "%s", modpath);
        }
        for (int i = 0; i < g_unit.searchpaths.len && !path[0]; i++) {
            const char *root = (const char *)g_unit.searchpaths.data[i];
            char cand[1024];
            snprintf(cand, sizeof cand, "%s/%s/src/%s.vela", root, pkg, rest);
            if (file_exists(cand)) { snprintf(path, sizeof path, "%s", cand); break; }
            snprintf(cand, sizeof cand, "%s/%s/src/main.vela", root, pkg);
            if (!slash && file_exists(cand)) { snprintf(path, sizeof path, "%s", cand); break; }
        }
        if (!path[0]) {
            /* also allow a bare sibling file for single-file projects */
            char dir[1024];
            dirname_of(fromfile ? fromfile : ".", dir, sizeof dir);
            snprintf(path, sizeof path, "%s/%s.vela", dir, modpath);
            normalize(path);
        }
    }

    if (!file_exists(path)) {
        Diag *d = diag_add(DIAG_ERROR, sp, "cannot find module `%s`", modpath);
        diag_note(d, NOSPAN, "looked for `%s`", path);
        if (strncmp(modpath, "std/", 4) == 0)
            diag_note(d, NOSPAN, "the standard library lives in `%s/lib/std`; set VELA_ROOT if it moved", velaroot);
        else if (modpath[0] != '.')
            diag_note(d, NOSPAN, "help: add it to `vela.toml` under `[deps]`, or use `./%s` for a local file", modpath);
        return NULL;
    }

    SrcFile *sf = src_load(path, path);
    if (!sf) {
        diag_add(DIAG_ERROR, sp, "cannot read `%s`", path);
        return NULL;
    }
    Module *m = NEW(Module);
    m->modpath = intern(modpath);
    m->file = intern(path);
    m->src = sf;
    const char *base = strrchr(modpath, '/');
    m->name = intern(base ? base + 1 : modpath);
    vec_push(&g_unit.modules, m);
    (void)display;

    TokList tl;
    memset(&tl, 0, sizeof tl);
    lex_file(sf, &tl);
    parse_module(&tl, sf, m);
    free(tl.toks);

    /* eagerly load imports so cycles are reported with a useful message */
    for (int i = 0; i < m->decls.len; i++) {
        Decl *d = VEC_AT(&m->decls, Decl, i);
        if (d->kind != D_USE) continue;
        load_module(d->path, m->file, d->span);
    }
    return m;
}

/* ------------------------------------------------------------------ */
/* driver                                                               */
/* ------------------------------------------------------------------ */

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void usage(FILE *o) {
    fprintf(o,
"velac - the Vela bootstrap compiler\n"
"\n"
"usage: velac [options] <file.vela>\n"
"\n"
"options:\n"
"  -o <path>        write the executable to <path> (default: a.out)\n"
"  --check          type-check only; do not emit an executable\n"
"  --test           compile `test` blocks into a test binary\n"
"  --emit-ir        print the optimised Vela IR to stdout\n"
"  --emit-tokens    print the token stream of the entry module\n"
"  --root <dir>     location of the Vela installation (lib/, tools/)\n"
"  --dep <dir>      add a dependency search root (repeatable)\n"
"  --no-color       disable coloured diagnostics\n"
"  --werror         treat warnings as errors\n"
"  --time           print per-phase timings\n"
"  -q, --quiet      suppress warnings\n"
"  -h, --help       show this message\n"
"  --version        show the version\n");
}

static void discover_root(const char *argv0) {
    const char *env = getenv("VELA_ROOT");
    if (env && *env) { snprintf(velaroot, sizeof velaroot, "%s", env); return; }
    if (opt_root) { snprintf(velaroot, sizeof velaroot, "%s", opt_root); return; }
    /* <dir of argv0>/.. */
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n > 0) {
        exe[n] = 0;
        char dir[1024];
        dirname_of(exe, dir, sizeof dir);
        snprintf(velaroot, sizeof velaroot, "%s/..", dir);
        normalize(velaroot);
        char probe[1200];
        snprintf(probe, sizeof probe, "%s/lib/core/core.vela", velaroot);
        if (file_exists(probe)) return;
    }
    char dir[1024];
    dirname_of(argv0, dir, sizeof dir);
    snprintf(velaroot, sizeof velaroot, "%s/..", dir);
    normalize(velaroot);
}

int main(int argc, char **argv) {
    const char *input = NULL;
    if (!isatty(2)) diag_set_color(0);

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-o") == 0 && i + 1 < argc) opt_out = argv[++i];
        else if (strcmp(a, "--check") == 0) opt_check_only = 1;
        else if (strcmp(a, "--test") == 0) g_unit.build_tests = 1;
        else if (strcmp(a, "--emit-ir") == 0) opt_emit_ir = 1;
        else if (strcmp(a, "--emit-tokens") == 0) opt_emit_tokens = 1;
        else if (strcmp(a, "--emit-ast") == 0) opt_emit_ast = 1;
        else if (strcmp(a, "--root") == 0 && i + 1 < argc) opt_root = argv[++i];
        else if (strcmp(a, "--dep") == 0 && i + 1 < argc)
            vec_push(&g_unit.searchpaths, (void *)argv[++i]);
        else if (strcmp(a, "--no-color") == 0) diag_set_color(0);
        else if (strcmp(a, "--color") == 0) diag_set_color(1);
        else if (strcmp(a, "--werror") == 0) opt_werror = 1;
        else if (strcmp(a, "--time") == 0) opt_time = 1;
        else if (strcmp(a, "-q") == 0 || strcmp(a, "--quiet") == 0) opt_quiet = 1;
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(stdout); return 0; }
        else if (strcmp(a, "--version") == 0) { printf("velac 1.0.0 (bootstrap)\n"); return 0; }
        else if (a[0] == '-' && a[1]) {
            fprintf(stderr, "velac: unknown option `%s`\n", a);
            fprintf(stderr, "try `velac --help`\n");
            return 2;
        } else {
            if (input) { fprintf(stderr, "velac: only one input file may be given\n"); return 2; }
            input = a;
        }
    }
    if (!input) { usage(stderr); return 2; }
    if (!file_exists(input)) { fprintf(stderr, "velac: cannot open `%s`\n", input); return 2; }

    discover_root(argv[0]);
    {
        char probe[1200];
        snprintf(probe, sizeof probe, "%s/lib/core/core.vela", velaroot);
        if (!file_exists(probe)) {
            fprintf(stderr,
                "velac: cannot find the Vela standard library.\n"
                "       looked in `%s/lib`.\n"
                "       set VELA_ROOT or pass --root <install dir>.\n", velaroot);
            return 2;
        }
    }
    g_unit.stdroot = intern(velaroot);
    /* default dependency root: ./deps relative to the entry file */
    {
        char dir[1024], deps[1100];
        dirname_of(input, dir, sizeof dir);
        snprintf(deps, sizeof deps, "%s/deps", dir);
        vec_push(&g_unit.searchpaths, arena_strdup(&g_arena, deps));
    }

    double t0 = now_ms(), t_parse, t_sema, t_ir, t_code;

    if (opt_emit_tokens) {
        SrcFile *sf = src_load(input, input);
        TokList tl; memset(&tl, 0, sizeof tl);
        lex_file(sf, &tl);
        for (int i = 0; i < tl.n; i++) {
            Tok *t = &tl.toks[i];
            printf("%3d:%-3d %-14s", t->span.line, t->span.col,
                   tok_names[t->kind] ? tok_names[t->kind] : "?");
            if (t->kind == T_IDENT || t->kind == T_STR || t->kind == T_DOC) printf(" %s", t->text);
            if (t->kind == T_INT || t->kind == T_CHAR) printf(" %lld", (long long)t->ival);
            if (t->kind == T_FLOAT) printf(" %g", t->fval);
            printf("\n");
        }
        diag_flush(stderr);
        return diag_error_count() ? 1 : 0;
    }

    /* the core module and prelude are always present */
    load_module("core", NULL, NOSPAN);
    load_module("prelude", NULL, NOSPAN);

    /* load the entry file directly by path */
    Module *root = NULL;
    {
        char apath[1024];
        snprintf(apath, sizeof apath, "%s", input);
        normalize(apath);
        SrcFile *sf = src_load(apath, apath);
        if (!sf) { fprintf(stderr, "velac: cannot read `%s`\n", input); return 2; }
        root = NEW(Module);
        root->modpath = intern(apath);
        root->file = intern(apath);
        root->src = sf;
        const char *base = strrchr(apath, '/');
        base = base ? base + 1 : apath;
        char nm[256];
        snprintf(nm, sizeof nm, "%s", base);
        char *dot = strrchr(nm, '.');
        if (dot) *dot = 0;
        root->name = intern(nm);
        vec_push(&g_unit.modules, root);
        TokList tl; memset(&tl, 0, sizeof tl);
        lex_file(sf, &tl);
        parse_module(&tl, sf, root);
        free(tl.toks);
        for (int i = 0; i < root->decls.len; i++) {
            Decl *d = VEC_AT(&root->decls, Decl, i);
            if (d->kind == D_USE) load_module(d->path, root->file, d->span);
        }
    }
    g_unit.root = root;
    t_parse = now_ms();

    if (diag_error_count()) { diag_flush(stderr); return 1; }

    sema_run(&g_unit);
    t_sema = now_ms();

    /* locate main */
    if (!g_unit.build_tests) {
        for (int i = 0; i < root->decls.len; i++) {
            Decl *d = VEC_AT(&root->decls, Decl, i);
            if (d->kind == D_FN && d->name == intern("main") && !d->recv) {
                if (d->params.len)
                    diag_add(DIAG_ERROR, d->span, "`main` takes no parameters (use `os.args()` instead)");
                if (d->insts.len) g_unit.entry = VEC_AT(&d->insts, FnInst, 0);
            }
        }
        if (!g_unit.entry && !opt_check_only) {
            Diag *d = diag_add(DIAG_ERROR, NOSPAN, "`%s` has no `main` function", input);
            diag_note(d, NOSPAN, "help: add\n       fn main() {\n           println(\"hello\")\n       }");
        }
    }

    if (opt_quiet) { /* warnings suppressed at flush time below */ }
    int errs = diag_error_count();
    int warns = diag_warn_count();
    diag_flush(stderr);
    if (errs) {
        fprintf(stderr, "%d error%s\n", errs, errs == 1 ? "" : "s");
        return 1;
    }
    if (opt_werror && warns) {
        fprintf(stderr, "%d warning%s treated as errors (--werror)\n", warns, warns == 1 ? "" : "s");
        return 1;
    }
    if (opt_check_only) {
        if (!opt_quiet && warns) fprintf(stderr, "%d warning%s\n", warns, warns == 1 ? "" : "s");
        return 0;
    }

    irgen_run(&g_unit);
    /* the test harness entry point */
    if (g_unit.build_tests) {
        extern FnInst *build_test_main(Unit *u);
        g_unit.entry = build_test_main(&g_unit);
    }
    {
        extern int ir_verify(Unit *u, FILE *out);
        int bad = opt_emit_ir ? 0 : ir_verify(&g_unit, stderr);
        if (bad) {
            fprintf(stderr, "velac: internal error: %d IR invariant violation%s\n"
                            "       this is a compiler bug; please report it\n",
                    bad, bad == 1 ? "" : "s");
            return 70;
        }
    }
    ir_optimize(&g_unit);
    t_ir = now_ms();

    if (opt_emit_ir) { ir_dump(&g_unit, stdout); return 0; }

    const char *out = opt_out ? opt_out : "a.out";
    codegen_run(&g_unit, out);
    t_code = now_ms();

    if (opt_time) {
        fprintf(stderr, "parse  %6.1f ms\nsema   %6.1f ms\nir     %6.1f ms\ncodegen%6.1f ms\ntotal  %6.1f ms\n",
                t_parse - t0, t_sema - t_parse, t_ir - t_sema, t_code - t_ir, t_code - t0);
        fprintf(stderr, "arena  %6.1f MB\n", g_arena.total / 1048576.0);
    }
    if (!opt_quiet && warns) fprintf(stderr, "%d warning%s\n", warns, warns == 1 ? "" : "s");
    return 0;
}
