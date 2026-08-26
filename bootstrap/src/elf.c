/* elf.c — the ELF64 writer, shared by every backend.
 *
 * Two program headers and nothing else: one read-execute segment holding the
 * code and the read-only data, and one zero-filled read-write segment for the
 * globals and the runtime state area. No sections, no symbol table, no dynamic
 * linking, no interpreter. The kernel maps two ranges and jumps to the entry.
 */
#include "vela.h"
#include <sys/stat.h>

Target g_target = TARGET_X86_64;

const char *target_name(Target t) {
    return t == TARGET_ARM64 ? "arm64" : "x86_64";
}

int target_from_name(const char *s, Target *out) {
    if (!s) return 0;
    if (!strcmp(s, "x86_64") || !strcmp(s, "x86-64") || !strcmp(s, "amd64") ||
        !strcmp(s, "x64")) { *out = TARGET_X86_64; return 1; }
    if (!strcmp(s, "arm64") || !strcmp(s, "aarch64")) { *out = TARGET_ARM64; return 1; }
    return 0;
}

int elf_write(const ElfImage *img, const char *path) {
    Buf out;
    memset(&out, 0, sizeof out);

    size_t seg1_end = img->ro_off + g_rodata.data.len;
    uint64_t data_fileoff = 0x1000 + ((seg1_end + 0xFFF) & ~(uint64_t)0xFFF);

    /* e_ident */
    buf_u8(&out, 0x7f); buf_str(&out, "ELF");
    buf_u8(&out, 2);            /* ELFCLASS64 */
    buf_u8(&out, 1);            /* ELFDATA2LSB */
    buf_u8(&out, 1);            /* EV_CURRENT */
    buf_u8(&out, 0);            /* ELFOSABI_SYSV */
    for (int i = 0; i < 8; i++) buf_u8(&out, 0);

    buf_u16(&out, 2);           /* ET_EXEC */
    buf_u16(&out, img->machine);
    buf_u32(&out, 1);           /* EV_CURRENT */
    buf_u64(&out, img->entry);
    buf_u64(&out, 64);          /* e_phoff */
    buf_u64(&out, 0);           /* e_shoff */
    buf_u32(&out, 0);           /* e_flags */
    buf_u16(&out, 64);          /* e_ehsize */
    buf_u16(&out, 56);          /* e_phentsize */
    buf_u16(&out, 2);           /* e_phnum */
    buf_u16(&out, 64);          /* e_shentsize */
    buf_u16(&out, 0);           /* e_shnum */
    buf_u16(&out, 0);           /* e_shstrndx */

    /* PT_LOAD: text + rodata, read + execute */
    buf_u32(&out, 1); buf_u32(&out, 5);
    buf_u64(&out, 0x1000);
    buf_u64(&out, img->text_vaddr);
    buf_u64(&out, img->text_vaddr);
    buf_u64(&out, seg1_end);
    buf_u64(&out, seg1_end);
    buf_u64(&out, 0x1000);

    /* PT_LOAD: globals and runtime state, read + write, zero filled */
    buf_u32(&out, 1); buf_u32(&out, 6);
    buf_u64(&out, data_fileoff);
    buf_u64(&out, img->data_vaddr);
    buf_u64(&out, img->data_vaddr);
    buf_u64(&out, 0);
    buf_u64(&out, img->rw_size + 4096);
    buf_u64(&out, 0x1000);

    while (out.len < 0x1000) buf_u8(&out, 0);
    buf_put(&out, img->text->data, img->text->len);
    while (out.len < 0x1000 + img->ro_off) buf_u8(&out, 0);
    if (g_rodata.data.len) buf_put(&out, g_rodata.data.data, g_rodata.data.len);

    FILE *fp = fopen(path, "wb");
    if (!fp) { fatal("cannot write `%s`", path); return 0; }
    fwrite(out.data, 1, out.len, fp);
    fclose(fp);
    chmod(path, 0755);
    buf_free(&out);
    return 1;
}
