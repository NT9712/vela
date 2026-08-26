/* pe.c — the PE32+ writer and the Windows import table.
 *
 * Windows has no stable system-call interface: the numbers change between
 * builds, and Microsoft's supported boundary is the DLLs. So the Windows
 * target does not use `@syscall` at all. Instead the compiler knows a fixed
 * table of imports, `sys_windows.vela` names them by index, and the
 * `@winapi` intrinsic becomes an indirect call through the import address
 * table using the Microsoft x64 calling convention.
 */
#include "vela.h"

/* The order here is the ABI between the compiler and lib/core/sys_windows.vela.
   Append only; never reorder. */
const WinImport win_imports[] = {
    /*  0 */ { "kernel32.dll", "ExitProcess" },
    /*  1 */ { "kernel32.dll", "GetStdHandle" },
    /*  2 */ { "kernel32.dll", "WriteFile" },
    /*  3 */ { "kernel32.dll", "ReadFile" },
    /*  4 */ { "kernel32.dll", "VirtualAlloc" },
    /*  5 */ { "kernel32.dll", "CreateFileA" },
    /*  6 */ { "kernel32.dll", "CloseHandle" },
    /*  7 */ { "kernel32.dll", "DeleteFileA" },
    /*  8 */ { "kernel32.dll", "CreateDirectoryA" },
    /*  9 */ { "kernel32.dll", "RemoveDirectoryA" },
    /* 10 */ { "kernel32.dll", "MoveFileA" },
    /* 11 */ { "kernel32.dll", "GetFileAttributesA" },
    /* 12 */ { "kernel32.dll", "GetFileSizeEx" },
    /* 13 */ { "kernel32.dll", "FindFirstFileA" },
    /* 14 */ { "kernel32.dll", "FindNextFileA" },
    /* 15 */ { "kernel32.dll", "FindClose" },
    /* 16 */ { "kernel32.dll", "GetCommandLineA" },
    /* 17 */ { "kernel32.dll", "GetEnvironmentStringsA" },
    /* 18 */ { "kernel32.dll", "GetCurrentDirectoryA" },
    /* 19 */ { "kernel32.dll", "SetCurrentDirectoryA" },
    /* 20 */ { "kernel32.dll", "GetCurrentProcessId" },
    /* 21 */ { "kernel32.dll", "Sleep" },
    /* 22 */ { "kernel32.dll", "QueryPerformanceCounter" },
    /* 23 */ { "kernel32.dll", "QueryPerformanceFrequency" },
    /* 24 */ { "kernel32.dll", "GetSystemTimeAsFileTime" },
    /* 25 */ { "kernel32.dll", "CreateProcessA" },
    /* 26 */ { "kernel32.dll", "WaitForSingleObject" },
    /* 27 */ { "kernel32.dll", "GetExitCodeProcess" },
    /* 28 */ { "kernel32.dll", "CreatePipe" },
    /* 29 */ { "kernel32.dll", "SetHandleInformation" },
    /* 30 */ { "kernel32.dll", "TerminateProcess" },
    /* 31 */ { "kernel32.dll", "OpenProcess" },
    /* 32 */ { "kernel32.dll", "GetConsoleMode" },
    /* 33 */ { "kernel32.dll", "SetFilePointerEx" },
    /* 34 */ { "kernel32.dll", "GetModuleFileNameA" },
    /* 35 */ { "ws2_32.dll",   "WSAStartup" },
    /* 36 */ { "ws2_32.dll",   "socket" },
    /* 37 */ { "ws2_32.dll",   "connect" },
    /* 38 */ { "ws2_32.dll",   "bind" },
    /* 39 */ { "ws2_32.dll",   "listen" },
    /* 40 */ { "ws2_32.dll",   "accept" },
    /* 41 */ { "ws2_32.dll",   "send" },
    /* 42 */ { "ws2_32.dll",   "recv" },
    /* 43 */ { "ws2_32.dll",   "closesocket" },
    /* 44 */ { "ws2_32.dll",   "setsockopt" },
    /* 45 */ { "ws2_32.dll",   "shutdown" },
};

int win_import_count(void) { return (int)(sizeof win_imports / sizeof *win_imports); }

static size_t align_up(size_t v, size_t a);

#define SECT_ALIGN 0x1000
#define FILE_ALIGN 0x200
#define IMAGE_BASE 0x140000000ULL

static size_t align_up(size_t v, size_t a) { return (v + a - 1) & ~(a - 1); }

/* The .idata layout, computed once and used by both the writer and the
   relocation pass. Getting these two out of step is exactly the kind of bug
   that produces a jump into the middle of the import name table. */
typedef struct {
    const char *dlls[8];
    int   ndll;
    size_t off_desc, off_ilt, off_iat, off_names;
    int   slot[64];        /* IAT offset within the blob, per import index */
} IdataLayout;

static void idata_layout(IdataLayout *L) {
    int n = win_import_count();
    memset(L, 0, sizeof *L);
    for (int i = 0; i < n; i++) {
        int seen = 0;
        for (int j = 0; j < L->ndll; j++)
            if (!strcmp(L->dlls[j], win_imports[i].dll)) seen = 1;
        if (!seen && L->ndll < 8) L->dlls[L->ndll++] = win_imports[i].dll;
    }
    size_t desc_sz = (size_t)(L->ndll + 1) * 20;
    size_t thunks = 0;
    for (int d = 0; d < L->ndll; d++) {
        int cnt = 0;
        for (int i = 0; i < n; i++) if (!strcmp(win_imports[i].dll, L->dlls[d])) cnt++;
        thunks += (size_t)(cnt + 1) * 8;
    }
    L->off_desc  = 0;
    L->off_ilt   = desc_sz;
    L->off_iat   = L->off_ilt + thunks;
    L->off_names = L->off_iat + thunks;

    size_t cur = L->off_iat;
    for (int d = 0; d < L->ndll; d++) {
        for (int i = 0; i < n; i++) {
            if (strcmp(win_imports[i].dll, L->dlls[d])) continue;
            if (i < 64) L->slot[i] = (int)cur;
            cur += 8;
        }
        cur += 8;                      /* the null terminator */
    }
}

/* The IAT entry for import `i`, as an offset within the .idata blob. */
uint32_t pe_iat_slot(int i) {
    IdataLayout L;
    idata_layout(&L);
    if (i < 0 || i >= win_import_count() || i >= 64) return (uint32_t)L.off_iat;
    return (uint32_t)L.slot[i];
}

/* The exact size of the .idata blob. The estimate and the reality have to
   agree, or the globals and the runtime state area land at the wrong address. */
size_t pe_idata_size(void) {
    Buf tmp; memset(&tmp, 0, sizeof tmp);
    extern uint32_t build_idata_impl(Buf *out, uint32_t rva);
    build_idata_impl(&tmp, 0);
    size_t n = tmp.len;
    buf_free(&tmp);
    return n;
}

uint32_t build_idata_impl(Buf *out, uint32_t rva);

static uint32_t build_idata(Buf *out, uint32_t rva) {
    return build_idata_impl(out, rva);
}

uint32_t build_idata_impl(Buf *out, uint32_t rva) {
    int n = win_import_count();
    IdataLayout L;
    idata_layout(&L);

    Buf names; memset(&names, 0, sizeof names);
    uint32_t *name_rva = NEWN(uint32_t, n);
    for (int i = 0; i < n; i++) {
        if (names.len & 1) buf_u8(&names, 0);
        name_rva[i] = (uint32_t)(rva + L.off_names + names.len);
        buf_u16(&names, 0);                     /* hint */
        buf_str(&names, win_imports[i].fn);
        buf_u8(&names, 0);
    }
    uint32_t dll_rva[8];
    for (int d = 0; d < L.ndll; d++) {
        if (names.len & 1) buf_u8(&names, 0);
        dll_rva[d] = (uint32_t)(rva + L.off_names + names.len);
        buf_str(&names, L.dlls[d]);
        buf_u8(&names, 0);
    }

    size_t total = align_up(L.off_names + names.len, 16);
    uint8_t *blob = (uint8_t *)arena_alloc(&g_arena, total);
    memset(blob, 0, total);

    size_t ilt_cur = L.off_ilt, iat_cur = L.off_iat;
    for (int d = 0; d < L.ndll; d++) {
        uint8_t *desc = blob + L.off_desc + (size_t)d * 20;
        uint32_t v;
        v = (uint32_t)(rva + ilt_cur); memcpy(desc + 0, &v, 4);    /* OriginalFirstThunk */
        v = 0;                          memcpy(desc + 4, &v, 4);    /* TimeDateStamp */
        v = 0;                          memcpy(desc + 8, &v, 4);    /* ForwarderChain */
        v = dll_rva[d];                 memcpy(desc + 12, &v, 4);   /* Name */
        v = (uint32_t)(rva + iat_cur); memcpy(desc + 16, &v, 4);   /* FirstThunk */
        for (int i = 0; i < n; i++) {
            if (strcmp(win_imports[i].dll, L.dlls[d])) continue;
            uint64_t e = name_rva[i];
            memcpy(blob + ilt_cur, &e, 8); ilt_cur += 8;
            memcpy(blob + iat_cur, &e, 8); iat_cur += 8;
        }
        ilt_cur += 8;
        iat_cur += 8;
    }
    memcpy(blob + L.off_names, names.data, names.len);
    buf_free(&names);

    buf_put(out, blob, total);
    return (uint32_t)L.off_iat;
}

static void put_section(Buf *o, const char *name, uint32_t vsize, uint32_t vaddr,
                        uint32_t rawsize, uint32_t rawptr, uint32_t chars) {
    char nm[8];
    memset(nm, 0, 8);
    memcpy(nm, name, strlen(name) < 8 ? strlen(name) : 8);
    buf_put(o, nm, 8);
    buf_u32(o, vsize); buf_u32(o, vaddr);
    buf_u32(o, rawsize); buf_u32(o, rawptr);
    buf_u32(o, 0); buf_u32(o, 0);
    buf_u16(o, 0); buf_u16(o, 0);
    buf_u32(o, chars);
}

int pe_write(Buf *text, size_t ro_off, size_t rw_size, uint32_t entry_rva,
             uint32_t idata_rva, const char *path) {
    Buf idata; memset(&idata, 0, sizeof idata);
    uint32_t iat_off = build_idata(&idata, idata_rva);

    size_t text_v = align_up(ro_off + g_rodata.data.len, SECT_ALIGN);
    size_t idata_v = align_up(idata.len, SECT_ALIGN);
    size_t data_v = align_up(rw_size + 4096, SECT_ALIGN);

    uint32_t text_rva = SECT_ALIGN;
    uint32_t idata_rva_real = (uint32_t)(text_rva + text_v);
    uint32_t data_rva = (uint32_t)(idata_rva_real + idata_v);

    size_t hdr_sz = align_up(0x200, FILE_ALIGN);
    uint32_t text_raw = (uint32_t)hdr_sz;
    uint32_t text_rawsz = (uint32_t)align_up(ro_off + g_rodata.data.len, FILE_ALIGN);
    uint32_t idata_raw = text_raw + text_rawsz;
    uint32_t idata_rawsz = (uint32_t)align_up(idata.len, FILE_ALIGN);

    Buf o; memset(&o, 0, sizeof o);
    /* DOS header and stub */
    buf_u8(&o, 'M'); buf_u8(&o, 'Z');
    for (int i = 2; i < 0x3c; i++) buf_u8(&o, 0);
    buf_u32(&o, 0x40);
    buf_str(&o, "PE"); buf_u8(&o, 0); buf_u8(&o, 0);

    /* COFF header */
    buf_u16(&o, 0x8664);            /* x86-64 */
    buf_u16(&o, 3);                 /* sections */
    buf_u32(&o, 0);                 /* timestamp: zero, for reproducible builds */
    buf_u32(&o, 0); buf_u32(&o, 0);
    buf_u16(&o, 240);               /* optional header size */
    buf_u16(&o, 0x0022);            /* EXECUTABLE_IMAGE | LARGE_ADDRESS_AWARE */

    /* optional header, PE32+ */
    buf_u16(&o, 0x20b);
    buf_u8(&o, 14); buf_u8(&o, 0);
    buf_u32(&o, (uint32_t)text_v);
    buf_u32(&o, (uint32_t)data_v);
    buf_u32(&o, 0);
    buf_u32(&o, entry_rva);
    buf_u32(&o, text_rva);
    buf_u64(&o, IMAGE_BASE);
    buf_u32(&o, SECT_ALIGN);
    buf_u32(&o, FILE_ALIGN);
    buf_u16(&o, 6); buf_u16(&o, 0);     /* OS version */
    buf_u16(&o, 0); buf_u16(&o, 0);     /* image version */
    buf_u16(&o, 6); buf_u16(&o, 0);     /* subsystem version */
    buf_u32(&o, 0);
    buf_u32(&o, (uint32_t)(data_rva + data_v));   /* size of image */
    buf_u32(&o, (uint32_t)hdr_sz);
    buf_u32(&o, 0);                     /* checksum */
    buf_u16(&o, 3);                     /* CONSOLE */
    buf_u16(&o, 0);                     /* dll characteristics */
    buf_u64(&o, 0x100000); buf_u64(&o, 0x10000);   /* stack reserve/commit */
    buf_u64(&o, 0x100000); buf_u64(&o, 0x10000);   /* heap  reserve/commit */
    buf_u32(&o, 0);
    buf_u32(&o, 16);                    /* directory count */
    for (int i = 0; i < 16; i++) {
        if (i == 1) { buf_u32(&o, idata_rva_real); buf_u32(&o, (uint32_t)idata.len); }
        else if (i == 12) { buf_u32(&o, idata_rva_real + iat_off); buf_u32(&o, 8); }
        else { buf_u32(&o, 0); buf_u32(&o, 0); }
    }

    put_section(&o, ".text",  (uint32_t)text_v,  text_rva,        text_rawsz,  text_raw,  0x60000020);
    put_section(&o, ".idata", (uint32_t)idata_v, idata_rva_real,  idata_rawsz, idata_raw, 0xC0000040);
    put_section(&o, ".data",  (uint32_t)data_v,  data_rva,        0,           0,         0xC0000080);

    while (o.len < hdr_sz) buf_u8(&o, 0);
    buf_put(&o, text->data, text->len);
    while (o.len < text_raw + ro_off) buf_u8(&o, 0);
    if (g_rodata.data.len) buf_put(&o, g_rodata.data.data, g_rodata.data.len);
    while (o.len < text_raw + text_rawsz) buf_u8(&o, 0);
    buf_put(&o, idata.data, idata.len);
    while (o.len < idata_raw + idata_rawsz) buf_u8(&o, 0);

    FILE *fp = fopen(path, "wb");
    if (!fp) { fatal("cannot write `%s`", path); return 0; }
    fwrite(o.data, 1, o.len, fp);
    fclose(fp);
    buf_free(&o);
    buf_free(&idata);
    return 1;
}
