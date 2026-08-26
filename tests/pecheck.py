#!/usr/bin/env python3
"""Structural validation of a PE32+ executable.

Nothing here executes the file: it checks that the headers, the section table
and the import directory are self-consistent, so that a broken image is caught
even on machines with no way to run it.
"""
import struct, sys

def fail(m):
    print("pecheck: " + m, file=sys.stderr)
    sys.exit(1)

d = open(sys.argv[1], 'rb').read()
if d[:2] != b'MZ':
    fail("no MZ signature")
pe = struct.unpack_from('<I', d, 0x3c)[0]
if d[pe:pe+4] != b'PE\0\0':
    fail("no PE signature at e_lfanew")
machine, nsect, _, _, _, optsz, chars = struct.unpack_from('<HHIIIHH', d, pe + 4)
if machine != 0x8664:
    fail("machine is 0x%04x, expected 0x8664 (x86-64)" % machine)
if not chars & 0x0002:
    fail("IMAGE_FILE_EXECUTABLE_IMAGE not set")
opt = pe + 24
if struct.unpack_from('<H', d, opt)[0] != 0x20b:
    fail("not PE32+")
entry = struct.unpack_from('<I', d, opt + 16)[0]
base = struct.unpack_from('<Q', d, opt + 24)[0]
sect_align = struct.unpack_from('<I', d, opt + 32)[0]
file_align = struct.unpack_from('<I', d, opt + 36)[0]
image_size = struct.unpack_from('<I', d, opt + 56)[0]
subsystem = struct.unpack_from('<H', d, opt + 68)[0]
nrva = struct.unpack_from('<I', d, opt + 108)[0]
if subsystem != 3:
    fail("subsystem is %d, expected 3 (console)" % subsystem)
if sect_align < file_align:
    fail("SectionAlignment < FileAlignment")
if nrva < 16:
    fail("only %d data directories" % nrva)

sects = []
st = opt + optsz
for i in range(nsect):
    name, vsize, vaddr, rawsz, rawptr = struct.unpack_from('<8sIIII', d, st + i * 40)
    sects.append((name.rstrip(b'\0').decode(), vsize, vaddr, rawsz, rawptr))
    if rawptr and rawptr + rawsz > len(d):
        fail("section %s runs past end of file" % name)
    if rawptr % file_align:
        fail("section %s raw pointer not file-aligned" % name)
    if vaddr % sect_align:
        fail("section %s virtual address not section-aligned" % name)
    if vaddr + vsize > image_size:
        fail("section %s runs past SizeOfImage" % name)

def find(rva):
    for n, vs, va, rs, rp in sects:
        if va <= rva < va + max(vs, rs):
            return rp + (rva - va)
    return None

if find(entry) is None:
    fail("entry point RVA 0x%x is not inside any section" % entry)

imp_rva, imp_sz = struct.unpack_from('<II', d, opt + 112 + 8)
if not imp_rva:
    fail("no import directory")
off = find(imp_rva)
if off is None:
    fail("import directory RVA is not mapped")
ndll = 0
nfn = 0
while True:
    ilt, _, _, name_rva, iat = struct.unpack_from('<IIIII', d, off + ndll * 20)
    if not (ilt or name_rva or iat):
        break
    for r in (ilt, name_rva, iat):
        if find(r) is None:
            fail("import descriptor %d points outside the image" % ndll)
    nm = d[find(name_rva):]
    dll = nm[:nm.index(b'\0')].decode()
    if not dll.lower().endswith('.dll'):
        fail("import name %r is not a DLL" % dll)
    t = find(ilt)
    while True:
        e = struct.unpack_from('<Q', d, t)[0]
        if not e:
            break
        if not e & (1 << 63):        # by name, not ordinal
            if find(e & 0x7fffffff) is None:
                fail("hint/name entry outside the image")
            nfn += 1
        t += 8
    ndll += 1
if ndll == 0:
    fail("import table is empty")
print("pecheck: ok - PE32+ x86-64, %d sections, %d DLLs, %d imports, entry 0x%x"
      % (nsect, ndll, nfn, base + entry))
