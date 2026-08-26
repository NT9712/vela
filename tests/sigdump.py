#!/usr/bin/env python3
"""Dump the embedded signature SuperBlob of a Mach-O, for CI diagnostics."""
import struct
import sys

d = open(sys.argv[1], "rb").read()
off = 32
for _ in range(64):
    cmd, size = struct.unpack_from("<II", d, off)
    if cmd == 0x1D:
        soff, ssz = struct.unpack_from("<II", d, off + 8)
        print("LC_CODE_SIGNATURE: offset %d size %d" % (soff, ssz))
        sb = d[soff:]
        magic, length, count = struct.unpack_from(">III", sb, 0)
        print("SuperBlob magic 0x%08x length %d count %d" % (magic, length, count))
        for i in range(count):
            t, o = struct.unpack_from(">II", sb, 12 + i * 8)
            print("  slot type %d at +%d" % (t, o))
            cm, cl, ver, fl = struct.unpack_from(">IIII", sb, o)
            print("    magic 0x%08x length %d version 0x%x flags 0x%x" % (cm, cl, ver, fl))
            if cm == 0xFADE0C02:
                ho, io, ns, nc, limit = struct.unpack_from(">IIIII", sb, o + 16)
                hs, ht, plat, plog = struct.unpack_from(">BBBB", sb, o + 36)
                ident = sb[o + io:o + io + 40].split(b"\0")[0]
                print("    hashOff %d identOff %d (%r) nSpecial %d nSlots %d codeLimit %d"
                      % (ho, io, ident, ns, nc, limit))
                print("    hashSize %d hashType %d platform %d pageSize 2^%d" % (hs, ht, plat, plog))
                first = sb[o + ho:o + ho + min(64, max(0, cl - nc * 32))]
                h0 = sb[o + ho:o + ho + 32]
                import hashlib
                want = hashlib.sha256(d[0:4096]).digest()
                print("    page0 stored %s" % h0.hex())
                print("    page0 sha256(file[0:4096]) %s" % want.hex())
        sys.exit(0)
    if not cmd:
        break
    off += size
print("no LC_CODE_SIGNATURE found")
sys.exit(1)
