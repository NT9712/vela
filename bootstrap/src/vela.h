/* vela.h — shared declarations for the Vela bootstrap compiler.
 *
 * The bootstrap compiler is written in C99 with no dependencies beyond the
 * freestanding-ish subset of libc (stdio/stdlib/string). It reads .vela source
 * and emits a static x86-64 ELF64 executable. No linker, no assembler, no libc
 * in the output.
 */
#ifndef VELA_H
#define VELA_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Arena + containers                                                   */
/* ------------------------------------------------------------------ */

typedef struct ArenaBlock ArenaBlock;
typedef struct { ArenaBlock *head; size_t total; } Arena;

void  *arena_alloc(Arena *a, size_t n);
void   arena_free(Arena *a);
char  *arena_strdup(Arena *a, const char *s);
char  *arena_strndup(Arena *a, const char *s, size_t n);

extern Arena g_arena;
#define NEW(T)      ((T *)arena_alloc(&g_arena, sizeof(T)))
#define NEWN(T, n)  ((T *)arena_alloc(&g_arena, sizeof(T) * (size_t)(n)))

/* growable pointer vector */
typedef struct { void **data; int len; int cap; } Vec;
void  vec_push(Vec *v, void *p);
void  vec_insert(Vec *v, int idx, void *p);
#define VEC_AT(v, T, i) ((T *)((v)->data[i]))

/* growable byte buffer */
typedef struct { uint8_t *data; size_t len; size_t cap; } Buf;
void buf_put(Buf *b, const void *p, size_t n);
void buf_u8(Buf *b, uint8_t v);
void buf_u16(Buf *b, uint16_t v);
void buf_u32(Buf *b, uint32_t v);
void buf_u64(Buf *b, uint64_t v);
void buf_str(Buf *b, const char *s);
void buf_printf(Buf *b, const char *fmt, ...);
void buf_zero(Buf *b, size_t n);
void buf_free(Buf *b);

/* interned strings */
const char *intern(const char *s);
const char *intern_n(const char *s, size_t n);

/* ------------------------------------------------------------------ */
/* Source management + diagnostics                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *path;      /* as written / resolved */
    const char *display;   /* short path for messages */
    char       *text;
    size_t      len;
    int         id;
} SrcFile;

SrcFile *src_load(const char *path, const char *display);
SrcFile *src_get(int id);

typedef struct { int file; int line; int col; int off; int len; } Span;
#define NOSPAN ((Span){ -1, 0, 0, 0, 0 })

typedef enum { DIAG_ERROR, DIAG_WARN, DIAG_NOTE, DIAG_HELP } DiagLevel;

typedef struct Diag {
    DiagLevel   level;
    Span        span;
    char       *msg;
    struct Diag *next;   /* attached notes/helps */
} Diag;

void  diag_reset(void);
Diag *diag_add(DiagLevel lv, Span sp, const char *fmt, ...);
void  diag_note(Diag *d, Span sp, const char *fmt, ...);
int   diag_error_count(void);
int   diag_warn_count(void);
void  diag_flush(FILE *out);
void  diag_set_color(int on);
void  diag_set_max_errors(int n);
/* Fatal: print and exit. */
void  fatal(const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* Tokens                                                               */
/* ------------------------------------------------------------------ */

typedef enum {
    T_EOF, T_NEWLINE,
    T_IDENT, T_INT, T_FLOAT, T_STR, T_CHAR, T_INTERP_STR,
    /* keywords */
    T_AND, T_AS, T_BREAK, T_CONST, T_CONTINUE, T_ELSE, T_ENUM, T_FALSE,
    T_FN, T_FOR, T_IF, T_IN, T_LET, T_MATCH, T_MUT, T_NIL, T_NOT, T_OR,
    T_PUB, T_RETURN, T_STRUCT, T_TEST, T_TRUE, T_TYPE, T_USE, T_SELF, T_WHILE,
    /* punctuation */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_COMMA, T_DOT, T_DOTDOT, T_DOTDOTEQ, T_COLON, T_SEMI, T_ARROW, T_FATARROW,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ, T_PERCENTEQ,
    T_EQ, T_EQEQ, T_BANGEQ, T_LT, T_LE, T_GT, T_GE,
    T_AMP, T_PIPE, T_CARET, T_TILDE, T_SHL, T_SHR,
    T_QUESTION, T_QQ, T_BANG, T_AT, T_UNDERSCORE, T_DOC,
    T_MAX
} TokKind;

extern const char *tok_names[T_MAX];

/* A piece of an interpolated string: either literal text or an expression. */
typedef struct StrPiece {
    int         is_expr;
    const char *text;       /* literal (already unescaped) */
    struct Tok *toks;       /* token stream for the expression */
    int         ntoks;
    struct StrPiece *next;
} StrPiece;

typedef struct Tok {
    TokKind     kind;
    Span        span;
    const char *text;       /* interned identifier / raw text */
    int64_t     ival;
    double      fval;
    StrPiece   *pieces;     /* for T_INTERP_STR */
} Tok;

typedef struct { Tok *toks; int n; int cap; } TokList;

int lex_file(SrcFile *f, TokList *out);

/* ------------------------------------------------------------------ */
/* AST                                                                  */
/* ------------------------------------------------------------------ */

typedef struct Type Type;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Decl Decl;
typedef struct Module Module;
typedef struct Pattern Pattern;

/* --- type expressions (syntax) --- */
typedef enum {
    TE_NAME, TE_OPT, TE_RES, TE_LIST, TE_MAP, TE_FN
} TypeExprKind;

typedef struct TypeExpr {
    TypeExprKind kind;
    Span         span;
    const char  *name;      /* TE_NAME */
    const char  *modname;   /* module qualifier, e.g. `lex` in `lex.Token` */
    Vec          args;      /* TypeExpr* : generic args / fn params */
    struct TypeExpr *sub;   /* TE_OPT/TE_RES/TE_LIST elem, TE_FN ret */
    struct TypeExpr *sub2;  /* TE_MAP value */
} TypeExpr;

/* --- expressions --- */
typedef enum {
    E_INT, E_FLOAT, E_STR, E_INTERP, E_CHAR, E_BOOL, E_NIL,
    E_IDENT, E_PATH,        /* mod.name */
    E_UNARY, E_BINARY, E_ASSIGNOP,
    E_CALL, E_METHOD, E_INDEX, E_FIELD, E_SLICE,
    E_LIST, E_MAP, E_STRUCT, E_LAMBDA, E_MATCH,
    E_TRY, E_ORELSE, E_CAST, E_INTRINSIC, E_RANGE,
    E_STMTEXPR   /* return/break/continue used as an expression tail of ?? */
} ExprKind;

typedef struct { const char *name; Expr *value; Span span; } FieldInit;
typedef struct { Pattern *pat; Expr *guard; Expr *body; Stmt *block; Span span; } MatchArm;
typedef struct { const char *name; TypeExpr *type; Span span; } Param;

struct Expr {
    ExprKind kind;
    Span     span;
    Type    *type;          /* filled by sema */

    int64_t     ival;
    double      fval;
    const char *sval;
    StrPiece   *pieces;

    const char *name;       /* ident / field / method / intrinsic */
    const char *mod;        /* module qualifier for E_PATH */
    int         op;         /* TokKind of operator */

    Expr  *a, *b, *c;
    Vec    list;            /* args / elements / fields / arms */
    Vec    targs;           /* TypeExpr* explicit type args */
    TypeExpr *texpr;        /* cast target / lambda ret */
    Vec    params;          /* Param* for lambda */
    Stmt  *body;            /* lambda block */
    Stmt  *stmt;            /* E_STMTEXPR */

    /* sema results */
    void   *sym;            /* resolved symbol */
    int     idx;            /* field index / variant tag / op class */
    void   *target;         /* resolved FnInst* for calls and lambdas */
    int     inclusive;      /* range */
    int     is_static;      /* call to non-closure */
    int     builtin;        /* BI_* when this is a builtin call */
    void   *extra;          /* FnInst* of a user `to_str`, when there is one */
    void   *cmp_type;       /* Type* compared by `==` on aggregates */
};

/* operand classes recorded on E_BINARY / E_INDEX / E_UNARY by sema */
enum { OPC_INT = 0, OPC_FLOAT, OPC_STR, OPC_ANY, OPC_BOOL, OPC_LIST, OPC_BYTE, OPC_PTR };
enum { IDX_LIST = 0, IDX_MAP, IDX_STR };

/* builtin functions known to the compiler */
enum {
    BI_NONE = 0, BI_STR, BI_INT, BI_FLOAT, BI_BYTE, BI_BOOL, BI_LEN,
    BI_PRINT, BI_PRINTLN, BI_PANIC, BI_ASSERT, BI_ASSERT_EQ, BI_ASSERT_NE,
    BI_OK, BI_ERR, BI_ERR_CODE, BI_VOIDVAL, BI_MAX
};

/* --- patterns --- */
typedef enum {
    P_WILD, P_BIND, P_INT, P_STR, P_BOOL, P_CHAR, P_FLOAT, P_NIL,
    P_ENUM, P_STRUCT, P_OR, P_OK, P_ERR, P_SOME
} PatKind;

struct Pattern {
    PatKind  kind;
    Span     span;
    const char *name;       /* binding name / variant / type */
    const char *tyname;
    const char *modname;    /* module qualifier for `mod.Type.Variant` */
    Vec      subs;          /* Pattern* */
    Vec      fields;        /* const char* names parallel to subs (P_STRUCT) */
    int64_t  ival;
    double   fval;
    const char *sval;
    /* sema */
    Type    *type;
    int      tag;
    int      slot;
    void    *sym;
};

/* --- statements --- */
typedef enum {
    S_LET, S_ASSIGN, S_EXPR, S_RETURN, S_IF, S_IFLET, S_WHILE, S_FOR,
    S_BLOCK, S_BREAK, S_CONTINUE, S_MATCH
} StmtKind;

struct Stmt {
    StmtKind kind;
    Span     span;
    const char *name;       /* let / iflet / for var */
    const char *name2;      /* for key,value */
    int      is_mut;
    TypeExpr *texpr;
    Expr    *a, *b;         /* init / cond / value */
    Stmt    *then_s, *else_s;
    Vec      list;          /* block stmts / match arms */
    int      op;            /* assign op */
    /* sema */
    void    *sym;
    void    *sym2;
    Type    *type;
};

/* --- declarations --- */
typedef enum { D_USE, D_CONST, D_STRUCT, D_ENUM, D_ALIAS, D_FN, D_TEST } DeclKind;

typedef struct { const char *name; TypeExpr *type; Span span; const char *doc; } StructField;
typedef struct { const char *name; Vec types; Span span; int tag; const char *doc; } EnumVariant;

struct Decl {
    DeclKind kind;
    Span     span;
    int      is_pub;
    const char *name;
    const char *doc;
    Module  *mod;

    /* use */
    const char *path;
    const char *alias;
    Module     *target_mod;

    /* const */
    TypeExpr *texpr;
    Expr     *value;

    /* struct/enum */
    Vec       generics;     /* const char* */
    Vec       fields;       /* StructField* */
    Vec       variants;     /* EnumVariant* */

    /* fn */
    const char *recv;       /* method receiver type name, or NULL */
    Vec       params;       /* Param* */
    TypeExpr *ret;
    Stmt     *body;
    int       has_self;

    /* sema */
    Type     *type;
    void     *sym;
    Vec       insts;        /* FnInst* */
    /* compile-time constant value (D_CONST only) */
    int       cfold;        /* 0 none, 1 int/bool/byte, 2 float, 3 string */
    int64_t   cfold_i;
    double    cfold_f;
    const char *cfold_s;
    int       cfold_len;
};

struct Module {
    const char *name;       /* binding name */
    const char *modpath;    /* canonical, e.g. "std/io" */
    const char *file;
    SrcFile    *src;
    Vec         decls;
    void       *scope;      /* Scope* */
    int         resolved;
    int         loading;
};

int parse_module(TokList *tl, SrcFile *f, Module *m);

/* ------------------------------------------------------------------ */
/* Types (semantic)                                                     */
/* ------------------------------------------------------------------ */

typedef enum {
    TY_VOID, TY_INT, TY_FLOAT, TY_BOOL, TY_BYTE, TY_STR,
    TY_LIST, TY_MAP, TY_OPT, TY_RES, TY_FN, TY_RANGE,
    TY_STRUCT, TY_ENUM, TY_GENERIC, TY_ANY, TY_ERRTYPE
} TypeKind;

struct Type {
    TypeKind kind;
    Type    *elem;          /* list elem / opt inner / res inner / map key */
    Type    *val;           /* map value */
    Vec      params;        /* Type* fn params */
    Type    *ret;
    Decl    *decl;          /* struct/enum decl */
    Vec      targs;         /* Type* generic args for struct/enum instance */
    const char *name;       /* generic param name / display */
    Vec      fields;        /* Type* resolved field types (struct) */
    Vec      variants;      /* Vec* of Vec of Type* (enum payloads) */
    int      instid;        /* monomorphised instance id */
    int      size;
    int      is_prim;       /* enum with no payloads */
    Type    *canon;
};

extern Type *ty_void, *ty_int, *ty_float, *ty_bool, *ty_byte, *ty_str,
            *ty_range, *ty_any, *ty_error;

Type *ty_list(Type *e);
Type *ty_map(Type *k, Type *v);
Type *ty_opt(Type *e);
Type *ty_res(Type *e);
Type *ty_fn(Vec params, Type *ret);
int   ty_eq(Type *a, Type *b);
int   ty_is_ref(Type *t);
const char *ty_str_of(Type *t);

/* ------------------------------------------------------------------ */
/* Symbols / scopes                                                     */
/* ------------------------------------------------------------------ */

typedef enum { SYM_LOCAL, SYM_PARAM, SYM_FN, SYM_CONST, SYM_TYPE, SYM_MOD, SYM_CAPTURE } SymKind;

typedef struct Sym {
    SymKind     kind;
    const char *name;
    Type       *type;
    Span        span;
    int         is_mut;
    int         used;
    int         slot;       /* local slot index */
    int         boxed;      /* captured by a closure -> heap cell */
    Decl       *decl;
    Module     *mod;
    struct Sym *next;
} Sym;

typedef struct Scope {
    struct Scope *parent;
    Sym         **buckets;
    int           nbuckets;
    int           count;
} Scope;

Scope *scope_new(Scope *parent);
Sym   *scope_put(Scope *s, Sym *sym);
Sym   *scope_get(Scope *s, const char *name);
Sym   *scope_get_local(Scope *s, const char *name);

/* ------------------------------------------------------------------ */
/* Compilation unit                                                     */
/* ------------------------------------------------------------------ */

typedef struct FnInst FnInst;

typedef struct {
    Vec      modules;       /* Module* */
    Vec      fns;           /* FnInst*  (monomorphised) */
    Vec      globals;       /* Sym* consts */
    Module  *root;
    FnInst  *entry;
    FnInst  *init_fn;
    Vec      tests;         /* FnInst* */
    int      build_tests;
    const char *stdroot;
    Vec      searchpaths;   /* char* extra roots for packages */
} Unit;

extern Unit g_unit;

Module *load_module(const char *modpath, const char *fromfile, Span sp);

/* type descriptors (emitted into rodata by codegen, consumed by the runtime) */
int  typedesc_for(Type *t);      /* returns a rodata offset */
void typedesc_reset(void);

/* struct/enum helpers shared between sema, irgen and codegen */
Type *struct_field_type(Type *st, int i);
int   struct_field_index(Type *st, const char *name);
int   enum_variant_index(Type *et, const char *name);
Type *enum_payload_type(Type *et, int variant, int i);
int   enum_payload_count(Type *et, int variant);
FnInst *find_to_str(Type *t);

/* ------------------------------------------------------------------ */
/* IR                                                                   */
/* ------------------------------------------------------------------ */

typedef enum {
    /* value producing */
    IR_CONST, IR_CONSTF, IR_GLOBAL_ADDR, IR_RODATA_ADDR, IR_FN_ADDR,
    IR_LOAD_LOCAL, IR_LOAD_MEM, IR_MOV,
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
    IR_AND, IR_OR, IR_XOR, IR_SHL, IR_SHR, IR_SAR_HACK, IR_NEG, IR_NOT,
    IR_FADD, IR_FSUB, IR_FMUL, IR_FDIV, IR_FNEG, IR_FSQRT,
    IR_EQ, IR_NE, IR_LT, IR_LE, IR_GT, IR_GE,
    IR_FEQ, IR_FNE, IR_FLT, IR_FLE, IR_FGT, IR_FGE,
    IR_I2F, IR_F2I, IR_CALL, IR_CALL_IND, IR_SYSCALL,
    IR_SAVE_REGS, IR_STACK_TOP, IR_LIST_GET, IR_STR_IDX, IR_LIST_SET, IR_WINCALL,
    /* effects */
    IR_STORE_LOCAL, IR_STORE_MEM, IR_STORE_GLOBAL, IR_RESTORE_REGS, IR_TRAP,
    /* terminators */
    IR_JMP, IR_BR, IR_RET, IR_RETV,
    IR_OP_MAX
} IrOp;

typedef struct IrIns {
    IrOp     op;
    int      dst;           /* vreg or -1 */
    int      a, b;          /* vreg operands */
    int64_t  imm;
    double   fimm;
    int      size;          /* memory access width in bytes */
    int      is_float;      /* dst is a float vreg */
    int      target;        /* block id / global idx / fn idx */
    int      target2;       /* second block for BR */
    Vec      args;          /* int-boxed vregs for calls */
    Span     span;
    const char *dbg;
} IrIns;

typedef struct IrBlock {
    int   id;
    Vec   ins;              /* IrIns* */
    int   reached;
    const char *label;
} IrBlock;

struct FnInst {
    const char *name;       /* mangled */
    Decl       *decl;
    Module     *mod;
    Vec         blocks;     /* IrBlock* */
    int         nvregs;
    int         nslots;     /* stack slots (8 bytes each) */
    Vec         param_types;/* Type* */
    Type       *ret;
    Vec         targs;      /* Type* */
    int         index;
    int         is_closure_body;
    int         nparams;
    int32_t     code_off;   /* filled by codegen */
    int         used;
    int         is_test;
    const char *test_name;
    int        *vreg_float; /* per-vreg float flag */
    int         vreg_cap;
    Span        span;
    Vec         captures;   /* Sym* captured by this closure body */
    Vec         param_syms; /* Sym* for parameters (to build boxes) */
    FnInst     *enclosing;
    int         is_lambda;
    int         reached;
    const char *doc;
    Span        inst_site;   /* where a generic instance was requested */
};

/* rodata blob */
typedef struct { Buf data; Vec fixups; } RoData;
extern RoData g_rodata;

/* ------------------------------------------------------------------ */
/* targets                                                              */
/* ------------------------------------------------------------------ */

typedef enum { TARGET_X86_64 = 0, TARGET_ARM64, TARGET_WINDOWS_X64,
               TARGET_MACOS_X64, TARGET_MACOS_ARM64 } Target;

/* An operating system interface the backend must provide. */
typedef enum { OS_LINUX = 0, OS_WINDOWS, OS_MACOS } TargetOS;

/* Where a macOS syscall's second return word is parked, as an offset into the
   runtime state area. Kept in step with `RT_SYSAUX` in lib/core/core.vela. */
#define RT_SYSAUX 392
TargetOS target_os(Target t);
extern Target g_target;
const char *target_name(Target t);
int target_from_name(const char *s, Target *out);

/* Shared ELF64 writer; the backends hand it a text blob and the layout. */
typedef struct {
    Buf     *text;
    uint64_t text_vaddr;
    size_t   ro_off;         /* offset of rodata within the text segment */
    uint64_t ro_vaddr;
    uint64_t data_vaddr;
    size_t   rw_size;
    uint64_t entry;
    uint16_t machine;        /* EM_X86_64 = 62, EM_AARCH64 = 183 */
} ElfImage;

int elf_write(const ElfImage *img, const char *path);

/* Windows imports the compiler knows how to emit. `sys_windows.vela` refers to
   these by index through the `@winapi` intrinsic. */
typedef struct { const char *dll; const char *fn; } WinImport;
extern const WinImport win_imports[];
int win_import_count(void);

/* Shared Mach-O writer, parameterised the same way as the ELF one. */
typedef struct {
    Buf     *text;
    uint64_t text_vaddr;
    size_t   ro_off;
    uint64_t data_vaddr;
    size_t   rw_size;
    uint64_t entry;
    int      arm;            /* 1 = arm64, 0 = x86-64 */
    uint64_t page;           /* segment alignment: 16 KiB on arm64 */
    const char *ident;       /* code-signing identifier */
} MachImage;

int macho_write(const MachImage *img, const char *path);
const char *mach_ident(const char *path);
int pe_write(Buf *text, size_t ro_off, size_t rw_size, uint32_t entry_rva,
             uint32_t iat_rva, const char *path);
size_t pe_idata_size(void);

int sema_run(Unit *u);
int irgen_run(Unit *u);
void ir_optimize(Unit *u);
int codegen_run(Unit *u, const char *outpath);
void ir_dump(Unit *u, FILE *out);

/* helpers shared with irgen */
int  ir_new_block(FnInst *f, const char *label);
IrIns *ir_emit(FnInst *f, int blk, IrOp op);

#endif /* VELA_H */
