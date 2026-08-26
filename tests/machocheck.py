#!/usr/bin/env python3
"""Structural validation of a Mach-O executable.

Nothing here executes the file. It re-parses the image from scratch and checks
that the header, the load commands, the segment map and the ad-hoc code
signature are all self-consistent -- including recomputing every SHA-256 page
hash in the CodeDirectory, which is the part macOS itself checks before it will
run an arm64 binary.
"""
import hashlib
import struct
import sys

MH_MAGIC_64 = 0xFEEDFACF
LC_SEGMENT_64 = 0x19
LC_UNIXTHREAD = 0x05
LC_CODE_SIGNATURE = 0x1D
CPU_X86_64 = 0x01000007
CPU_ARM64 = 0x0100000C


def fail(msg):
    print("machocheck: " + msg, file=sys.stderr)
    sys.exit(1)


def main(path):
    d = open(path, "rb").read()
    magic, cputype, cpusub, filetype, ncmds, sizeofcmds, flags, _ = struct.unpack_from("<IIIIIIII", d, 0)
    if magic != MH_MAGIC_64:
        fail("bad magic 0x%08x, expected 0x%08x" % (magic, MH_MAGIC_64))
    if cputype not in (CPU_X86_64, CPU_ARM64):
        fail("unexpected cputype 0x%08x" % cputype)
    arm = cputype == CPU_ARM64
    if filetype != 2:
        fail("filetype is %d, expected 2 (MH_EXECUTE)" % filetype)

    off = 32
    segs = []
    entry = None
    sig = None
    seen = 0
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", d, off)
        if cmdsize < 8 or off + cmdsize > len(d):
            fail("load command at 0x%x has impossible size %d" % (off, cmdsize))
        if cmd == LC_SEGMENT_64:
            name = d[off + 8:off + 24].rstrip(b"\0").decode()
            vmaddr, vmsize, fileoff, filesize, maxprot, initprot, nsects, _f = \
                struct.unpack_from("<QQQQIIII", d, off + 24)
            if fileoff + filesize > len(d):
                fail("segment %s runs past the end of the file" % name)
            if cmdsize != 72 + 80 * nsects:
                fail("segment %s cmdsize %d does not match %d sections" % (name, cmdsize, nsects))
            segs.append((name, vmaddr, vmsize, fileoff, filesize, initprot))
            so = off + 72
            for i in range(nsects):
                sname = d[so:so + 16].rstrip(b"\0").decode()
                saddr, ssize, soff = struct.unpack_from("<QQI", d, so + 32)
                sflags = struct.unpack_from("<I", d, so + 64)[0]
                zerofill = (sflags & 0xFF) == 1
                if not zerofill and soff + ssize > len(d):
                    fail("section %s,%s runs past the end of the file" % (name, sname))
                if not (vmaddr <= saddr and saddr + ssize <= vmaddr + vmsize):
                    fail("section %s,%s is not inside its segment" % (name, sname))
                so += 80
        elif cmd == LC_UNIXTHREAD:
            flavor, count = struct.unpack_from("<II", d, off + 8)
            want_flavor, want_count, pc_idx = (6, 68, 32) if arm else (4, 42, 16)
            if flavor != want_flavor:
                fail("thread flavor %d, expected %d" % (flavor, want_flavor))
            if count != want_count:
                fail("thread state count %d, expected %d" % (count, want_count))
            entry = struct.unpack_from("<Q", d, off + 16 + pc_idx * 8)[0]
        elif cmd == LC_CODE_SIGNATURE:
            sigoff, sigsize = struct.unpack_from("<II", d, off + 8)
            sig = (sigoff, sigsize)
        seen += 1
        off += cmdsize
    if off - 32 != sizeofcmds:
        fail("sizeofcmds is %d but the commands span %d bytes" % (sizeofcmds, off - 32))

    names = [s[0] for s in segs]
    for required in ("__PAGEZERO", "__TEXT", "__DATA"):
        if required not in names:
            fail("missing %s segment" % required)
    pz = segs[names.index("__PAGEZERO")]
    if pz[1] != 0 or pz[5] != 0:
        fail("__PAGEZERO must be unmapped at address zero")
    text = segs[names.index("__TEXT")]
    if text[5] & 4 == 0:
        fail("__TEXT is not executable")
    data = segs[names.index("__DATA")]
    if data[5] & 2 == 0:
        fail("__DATA is not writable")
    if data[1] < text[1] + text[2]:
        fail("__DATA overlaps __TEXT")

    page = 0x4000 if arm else 0x1000
    for name, vmaddr, vmsize, fileoff, filesize, _p in segs:
        if name == "__PAGEZERO":
            continue
        if vmaddr % page:
            fail("segment %s is not %d-byte aligned" % (name, page))

    if entry is None:
        fail("no LC_UNIXTHREAD, so the kernel has no entry point")
    if not (text[1] <= entry < text[1] + text[2]):
        fail("entry 0x%x is not inside __TEXT" % entry)

    if sig is None:
        fail("no LC_CODE_SIGNATURE; arm64 macOS would refuse to run this")
    sigoff, sigsize = sig
    if sigoff + sigsize > len(d):
        fail("code signature runs past the end of the file")
    magic, length, count = struct.unpack_from(">III", d, sigoff)
    if magic != 0xFADE0CC0:
        fail("bad SuperBlob magic 0x%08x" % magic)
    if count < 1:
        fail("SuperBlob holds no blobs")
    slot_type, slot_off = struct.unpack_from(">II", d, sigoff + 12)
    if slot_type != 0:
        fail("first slot is type %d, expected 0 (CodeDirectory)" % slot_type)
    cd = sigoff + slot_off
    cd_magic, cd_len, version, cd_flags, hash_off, ident_off, nspecial, nslots, \
        code_limit = struct.unpack_from(">IIIIIIIII", d, cd)
    if cd_magic != 0xFADE0C02:
        fail("bad CodeDirectory magic 0x%08x" % cd_magic)
    hash_size, hash_type, _plat, page_log = struct.unpack_from(">BBBB", d, cd + 36)
    if hash_type != 2 or hash_size != 32:
        fail("hash type %d size %d, expected SHA-256/32" % (hash_type, hash_size))
    if page_log != 12:
        fail("code signing page size is 2^%d, expected 2^12" % page_log)
    if not cd_flags & 2:
        fail("CS_ADHOC is not set")
    if code_limit != sigoff:
        fail("codeLimit %d does not reach the signature at %d" % (code_limit, sigoff))
    if nslots != (code_limit + 4095) // 4096:
        fail("nCodeSlots %d does not cover %d bytes" % (nslots, code_limit))
    ident = d[cd + ident_off:]
    ident = ident[:ident.index(b"\0")].decode()

    for i in range(nslots):
        start = i * 4096
        want = hashlib.sha256(d[start:min(start + 4096, code_limit)]).digest()
        got = d[cd + hash_off + i * 32:cd + hash_off + (i + 1) * 32]
        if want != got:
            fail("page hash %d does not match the file contents" % i)

    print("machocheck: ok - %s, %d segments, entry 0x%x, ad-hoc signature over "
          "%d pages verified, identifier %r"
          % ("arm64" if arm else "x86_64", len(segs), entry, nslots, ident))


if __name__ == "__main__":
    main(sys.argv[1])
