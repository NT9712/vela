/* macho.c — the Mach-O writer, shared by the two macOS targets.
 *
 * The shape mirrors elf.c: one read-execute segment holding the code and the
 * read-only data, one zero-filled read-write segment for the globals and the
 * runtime state area. There is no dynamic linker, no libSystem, and no symbol
 * table; the kernel maps the segments and jumps to the thread state's entry.
 *
 * Two things differ from ELF and both are forced by the platform:
 *
 *   __PAGEZERO      macOS wants an unmapped 4 GB guard at address zero, so the
 *                   image is based at 0x1_0000_0000 rather than 0x40_0000.
 *
 *   code signature  arm64 macOS refuses to execute an unsigned binary. Not
 *                   "warns" — the kernel kills it. So every image carries an
 *                   ad-hoc signature: a CodeDirectory holding a SHA-256 hash of
 *                   every 4 KiB page. Ad-hoc means it names no identity and
 *                   needs no key, which is exactly what `codesign -s -` writes.
 *                   Doing it here keeps the toolchain dependency-free.
 *
 * Everything in the signature is big-endian; everything else is little-endian.
 */
#include "vela.h"
#include <sys/stat.h>

/* ------------------------------------------------------------------ */
/* SHA-256                                                              */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t h[8]; uint64_t n; uint8_t buf[64]; size_t used; } Sha256;

static const uint32_t sha_k[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2,
};

static uint32_t ror32(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

static void sha_block(Sha256 *s, const uint8_t *p) {
    uint32_t w[64], a, b, c, d, e, f, g, h;
    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) |
               ((uint32_t)p[i*4+2] << 8) | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ror32(w[i-15], 7) ^ ror32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = ror32(w[i-2], 17) ^ ror32(w[i-2], 19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3];
    e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + sha_k[i] + w[i];
        uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d;
    s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

static void sha_init(Sha256 *s) {
    static const uint32_t iv[8] = { 0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                                    0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19 };
    memcpy(s->h, iv, sizeof iv);
    s->n = 0; s->used = 0;
}

static void sha_update(Sha256 *s, const uint8_t *p, size_t n) {
    s->n += n;
    while (n) {
        size_t take = 64 - s->used;
        if (take > n) take = n;
        memcpy(s->buf + s->used, p, take);
        s->used += take; p += take; n -= take;
        if (s->used == 64) { sha_block(s, s->buf); s->used = 0; }
    }
}

static void sha_final(Sha256 *s, uint8_t out[32]) {
    uint64_t bits = s->n * 8;
    uint8_t pad = 0x80;
    sha_update(s, &pad, 1);
    uint8_t z = 0;
    while (s->used != 56) sha_update(s, &z, 1);
    uint8_t len[8];
    for (int i = 0; i < 8; i++) len[i] = (uint8_t)(bits >> (56 - i * 8));
    s->n += 8;                      /* keep the counter honest, unused after */
    memcpy(s->buf + 56, len, 8);
    sha_block(s, s->buf);
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(s->h[i] >> 24);
        out[i*4+1] = (uint8_t)(s->h[i] >> 16);
        out[i*4+2] = (uint8_t)(s->h[i] >> 8);
        out[i*4+3] = (uint8_t)s->h[i];
    }
}

/* ------------------------------------------------------------------ */
/* little helpers                                                       */
/* ------------------------------------------------------------------ */

static void be32(Buf *b, uint32_t v) {
    buf_u8(b, (uint8_t)(v >> 24)); buf_u8(b, (uint8_t)(v >> 16));
    buf_u8(b, (uint8_t)(v >> 8));  buf_u8(b, (uint8_t)v);
}
static void be64(Buf *b, uint64_t v) { be32(b, (uint32_t)(v >> 32)); be32(b, (uint32_t)v); }

/* A 16-byte segment or section name, NUL-padded. Names may be exactly 16 bytes
   with no terminator, so the length is measured before copying rather than
   walking off the end of the literal. */
static void put_name(Buf *b, const char *s) {
    size_t n = strlen(s);
    if (n > 16) n = 16;
    for (size_t i = 0; i < n; i++) buf_u8(b, (uint8_t)s[i]);
    for (size_t i = n; i < 16; i++) buf_u8(b, 0);
}

static uint64_t up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

#define LC_SEGMENT_64     0x19
#define LC_UNIXTHREAD     0x05
#define LC_CODE_SIGNATURE 0x1d
#define LC_DYLD_INFO_ONLY     0x22
#define LC_FUNCTION_STARTS    0x26
#define LC_DATA_IN_CODE       0x29
#define LC_SOURCE_VERSION     0x2A
#define LC_BUILD_VERSION 0x32

/* One LC_SEGMENT_64 with at most one section. */
static void put_segment(Buf *o, const char *seg, uint64_t vmaddr, uint64_t vmsize,
                        uint64_t fileoff, uint64_t filesize, uint32_t maxprot,
                        uint32_t initprot, const char *sect, uint64_t saddr,
                        uint64_t ssize, uint64_t soff, uint32_t salign, uint32_t sflags) {
    uint32_t nsects = sect ? 1 : 0;
    buf_u32(o, LC_SEGMENT_64);
    buf_u32(o, 72 + 80 * nsects);
    put_name(o, seg);
    buf_u64(o, vmaddr); buf_u64(o, vmsize);
    buf_u64(o, fileoff); buf_u64(o, filesize);
    buf_u32(o, maxprot); buf_u32(o, initprot);
    buf_u32(o, nsects);  buf_u32(o, 0);
    if (sect) {
        put_name(o, sect); put_name(o, seg);
        buf_u64(o, saddr); buf_u64(o, ssize);
        buf_u32(o, (uint32_t)soff); buf_u32(o, salign);
        buf_u32(o, 0); buf_u32(o, 0);            /* reloff, nreloc */
        buf_u32(o, sflags);
        buf_u32(o, 0); buf_u32(o, 0); buf_u32(o, 0);
    }
}

int macho_write(const MachImage *img, const char *path) {
    const char *ident = img->ident ? img->ident : "vela";
    uint64_t page = img->page;
    uint64_t base = img->text_vaddr - page;

    size_t seg1_end = img->ro_off + g_rodata.data.len;
    uint64_t text_fsize = up(page + seg1_end, page);
    uint64_t data_vmsize = up(img->rw_size + 4096, page);
    uint64_t link_vaddr = up(img->data_vaddr + data_vmsize, page);

    /* The signature sits at the end of the file and covers everything before
       it, so its offset is simply the size of the rest of the image. */
    uint64_t sig_off = text_fsize;
    uint32_t nslots = (uint32_t)((sig_off + 4095) / 4096);
    size_t ident_len = strlen(ident) + 1;
    uint32_t cd_len = (uint32_t)(88 + ident_len + (size_t)nslots * 32);
    uint32_t sig_len = (uint32_t)up(20 + cd_len, 16);

    /* LC_BUILD_VERSION is mandatory on arm64 macOS (AMFI checks for it). */
    /* System linker includes these optional commands; kernel may expect them. */
    int ncmds = 11;
    /* LC_UNIXTHREAD: static binary model */
    uint32_t unixthread_sz = img->arm ? 16 + 68 * 4 : 16 + 42 * 4;
    uint32_t sizeofcmds = 72          /* __PAGEZERO */
                        + 72 + 80     /* __TEXT + __text */
                        + 72 + 80     /* __DATA + __bss */
                        + 72          /* __LINKEDIT */
                        + unixthread_sz
                        + 16          /* LC_CODE_SIGNATURE */
                        + 32          /* LC_BUILD_VERSION */
                        + 56          /* LC_DYLD_INFO_ONLY */
                        + 16          /* LC_FUNCTION_STARTS */
                        + 16          /* LC_DATA_IN_CODE */
                        + 16;         /* LC_SOURCE_VERSION */

    Buf out;
    memset(&out, 0, sizeof out);

    buf_u32(&out, 0xFEEDFACF);
    buf_u32(&out, img->arm ? 0x0100000C : 0x01000007);   /* cputype */
    buf_u32(&out, img->arm ? 0x00000000 : 0x00000003);   /* cpusubtype: ALL */
    buf_u32(&out, 2);                                     /* MH_EXECUTE */
    buf_u32(&out, (uint32_t)ncmds);
    buf_u32(&out, sizeofcmds);
    /* MH_PIE (0x200000) is required on arm64 macOS by AMFI policy.
       MH_NOUNDEFS (1) is also set. */
    uint32_t hdr_flags = 1 | (img->arm ? 0x200000 : 0);
    buf_u32(&out, hdr_flags);
    buf_u32(&out, 0);

    put_segment(&out, "__PAGEZERO", 0, base, 0, 0, 0, 0, NULL, 0, 0, 0, 0, 0);
    put_segment(&out, "__TEXT", base, text_fsize, 0, text_fsize, 5, 5,
                "__text", img->text_vaddr, seg1_end, page, 4,
                0x80000400);            /* PURE_INSTRUCTIONS | SOME_INSTRUCTIONS */
    put_segment(&out, "__DATA", img->data_vaddr, data_vmsize, text_fsize, 0, 3, 3,
                "__bss", img->data_vaddr, img->rw_size + 4096, 0, 4,
                0x00000001);            /* S_ZEROFILL */
    put_segment(&out, "__LINKEDIT", link_vaddr, up(sig_len, page),
                sig_off, sig_len, 1, 1, NULL, 0, 0, 0, 0, 0);

    /* LC_UNIXTHREAD: a register dump the kernel restores. No dyld involved. */
    buf_u32(&out, LC_UNIXTHREAD);
    if (img->arm) {
        buf_u32(&out, 16 + 68 * 4);
        buf_u32(&out, 6);               /* ARM_THREAD_STATE64 */
        buf_u32(&out, 68);
        for (int i = 0; i < 68; i++) {
            /* x0-x28, fp, lr, sp, pc, cpsr, pad: pc is the 33rd 64-bit slot */
            if (i == 64) { buf_u32(&out, (uint32_t)img->entry); continue; }
            if (i == 65) { buf_u32(&out, (uint32_t)(img->entry >> 32)); continue; }
            buf_u32(&out, 0);
        }
    } else {
        buf_u32(&out, 16 + 42 * 4);
        buf_u32(&out, 4);               /* x86_THREAD_STATE64 */
        buf_u32(&out, 42);
        for (int i = 0; i < 42; i++) {
            /* rax..r15 then rip: rip is the 17th 64-bit slot */
            if (i == 32) { buf_u32(&out, (uint32_t)img->entry); continue; }
            if (i == 33) { buf_u32(&out, (uint32_t)(img->entry >> 32)); continue; }
            buf_u32(&out, 0);
        }
    }

    buf_u32(&out, LC_CODE_SIGNATURE);
    buf_u32(&out, 16);
    buf_u32(&out, (uint32_t)sig_off);
    buf_u32(&out, sig_len);

    /* LC_BUILD_VERSION: required on arm64 macOS. */
    buf_u32(&out, LC_BUILD_VERSION);
    buf_u32(&out, 32);
    buf_u32(&out, 1);                 /* platform: macOS */
    buf_u32(&out, 0x000E0000);        /* minos: 14.0 */
    buf_u32(&out, 0x000E0000);        /* sdk: 14.0 */
    buf_u32(&out, 1);                 /* one tool entry */
    buf_u32(&out, 3);                 /* type: 3 = ld */
    buf_u32(&out, 0x000E0000);        /* version: 14.0 */

    /* LC_DYLD_INFO_ONLY: empty, but kernel may expect it.
       cmdsize = 8 (header) + 12*4 (data) = 56. */
    buf_u32(&out, LC_DYLD_INFO_ONLY);
    buf_u32(&out, 56);
    for (int i = 0; i < 12; i++) buf_u32(&out, 0);  /* 12 uint32_t fields */

    /* LC_FUNCTION_STARTS: empty. */
    buf_u32(&out, LC_FUNCTION_STARTS);
    buf_u32(&out, 16);
    buf_u32(&out, 0); buf_u32(&out, 0);  /* dataoff, datasize */

    /* LC_DATA_IN_CODE: empty. */
    buf_u32(&out, LC_DATA_IN_CODE);
    buf_u32(&out, 16);
    buf_u32(&out, 0); buf_u32(&out, 0);

    /* LC_SOURCE_VERSION: version 1.0.0. */
    buf_u32(&out, LC_SOURCE_VERSION);
    buf_u32(&out, 16);
    buf_u64(&out, 0x0001000000000000ULL);  /* A.B.C.D.E = 1.0.0.0.0 */

    if (out.len > page) fatal("mach-o load commands overflow the first page");
    while (out.len < page) buf_u8(&out, 0);
    buf_put(&out, img->text->data, img->text->len);
    while (out.len < page + img->ro_off) buf_u8(&out, 0);
    if (g_rodata.data.len) buf_put(&out, g_rodata.data.data, g_rodata.data.len);
    while (out.len < sig_off) buf_u8(&out, 0);

    /* ---- ad-hoc signature over everything written so far ---- */
    Buf cd;
    memset(&cd, 0, sizeof cd);
    uint32_t hash_off = (uint32_t)(88 + ident_len);
    be32(&cd, 0xFADE0C02);              /* CSMAGIC_CODEDIRECTORY */
    be32(&cd, cd_len);
    be32(&cd, 0x20400);                 /* version with execSeg fields */
    be32(&cd, 0x00000002);              /* CS_ADHOC */
    be32(&cd, hash_off);
    be32(&cd, 88);                      /* identOffset */
    be32(&cd, 0);                       /* nSpecialSlots */
    be32(&cd, nslots);
    be32(&cd, (uint32_t)sig_off);       /* codeLimit */
    /* Pack the 4 uint8_t fields into a be32 to avoid buf_u8 alignment issues:
       hashSize=32, hashType=2, platform=0, pageSize=12 -> 0x2002000C */
    be32(&cd, 0x2002000C);
    be32(&cd, 0);                       /* spare2 */
    be32(&cd, 0);                       /* scatterOffset */
    be32(&cd, 0);                       /* teamOffset */
    be32(&cd, 0);                       /* spare3 */
    be64(&cd, 0);                       /* codeLimit64: unused below 4 GiB */
    be64(&cd, 0);                       /* execSegBase */
    be64(&cd, text_fsize);              /* execSegLimit */
    be64(&cd, 1);                       /* execSegFlags: CS_EXECSEG_MAIN_BINARY */
    buf_put(&cd, (const uint8_t *)ident, ident_len);
    for (uint32_t i = 0; i < nslots; i++) {
        size_t off = (size_t)i * 4096;
        size_t n = sig_off - off < 4096 ? sig_off - off : 4096;
        Sha256 s; uint8_t h[32];
        sha_init(&s);
        sha_update(&s, out.data + off, n);
        sha_final(&s, h);
        buf_put(&cd, h, 32);
    }

    Buf sb;
    memset(&sb, 0, sizeof sb);
    be32(&sb, 0xFADE0CC0);              /* CSMAGIC_EMBEDDED_SIGNATURE */
    be32(&sb, (uint32_t)(20 + cd.len));
    be32(&sb, 1);                       /* one blob */
    be32(&sb, 0);                       /* CSSLOT_CODEDIRECTORY */
    be32(&sb, 20);                      /* its offset */
    buf_put(&sb, cd.data, cd.len);
    while (sb.len < sig_len) buf_u8(&sb, 0);
    buf_put(&out, sb.data, sb.len);
    buf_free(&cd); buf_free(&sb);

    FILE *fp = fopen(path, "wb");
    if (!fp) { fatal("cannot write `%s`", path); return 0; }
    fwrite(out.data, 1, out.len, fp);
    fclose(fp);
    chmod(path, 0755);
    buf_free(&out);
    return 1;
}
