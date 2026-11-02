"""Decrypt the Pia packets of a game log (online_menu.inc SendTo / RecvFrom lines) with the arena's session key.

    python tools/net/pia_decrypt.py <game.log> <session key hex> <gid> [--all]

Pia 5.18-5.21 (header byte 4 = 0x84): AES-128-GCM, key = the session key's first 16 bytes, nonce = connection id (header
0x5) + (gid & 0xFFFFFF) big-endian (3 bytes) + the 8-byte nonce field (header 0x8), tag = header 0x10..0x20, ciphertext
from 0x20 (Kinnay's NintendoClients wiki, "PIA Protocol"). Pure Python (no crypto module in deps\\python).
"""
import re
import sys

# ---------------------------------------------------------------- AES-128 (encryption only: CTR / GCM need no inverse)
SBOX = [0] * 256


def _init_sbox():
    p = q = 1
    while True:
        p = p ^ ((p << 1) & 0xFF) ^ (0x1B if p & 0x80 else 0)
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        x = q ^ ((q << 1) | (q >> 7)) & 0xFF ^ ((q << 2) | (q >> 6)) & 0xFF ^ ((q << 3) | (q >> 5)) & 0xFF ^ ((q << 4) | (q >> 4)) & 0xFF
        SBOX[p] = (x ^ 0x63) & 0xFF
        if p == 1:
            break
    SBOX[0] = 0x63


_init_sbox()


def xtime(a):
    return ((a << 1) ^ 0x1B) & 0xFF if a & 0x80 else a << 1


def expand_key(key):
    w = [list(key[i:i + 4]) for i in range(0, 16, 4)]
    rcon = 1
    for i in range(4, 44):
        t = list(w[i - 1])
        if i % 4 == 0:
            t = t[1:] + t[:1]
            t = [SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = xtime(rcon)
        w.append([w[i - 4][j] ^ t[j] for j in range(4)])
    return [sum(w[r * 4:r * 4 + 4], []) for r in range(11)]


def aes_encrypt(rk, block):
    s = [block[i] ^ rk[0][i] for i in range(16)]
    for r in range(1, 11):
        s = [SBOX[b] for b in s]
        s = [s[(i + 4 * (i % 4)) % 16] for i in range(16)]          # ShiftRows (column-major state)
        if r != 10:
            n = []
            for c in range(4):
                a = s[4 * c:4 * c + 4]
                t = a[0] ^ a[1] ^ a[2] ^ a[3]
                n += [a[i] ^ t ^ xtime(a[i] ^ a[(i + 1) % 4]) for i in range(4)]
            s = n
        s = [s[i] ^ rk[r][i] for i in range(16)]
    return bytes(s)


# ---------------------------------------------------------------- GCM
def gf_mul(x, y):
    R = 0xE1000000000000000000000000000000
    z, v = 0, y
    for i in range(127, -1, -1):
        if (x >> i) & 1:
            z ^= v
        v = (v >> 1) ^ R if v & 1 else v >> 1
    return z


def ghash(h, aad, ct):
    def blocks(b):
        b = b + b"\0" * (-len(b) % 16)
        return [int.from_bytes(b[i:i + 16], "big") for i in range(0, len(b), 16)]
    y = 0
    for blk in blocks(aad) + blocks(ct) + [(len(aad) * 8) << 64 | (len(ct) * 8)]:
        y = gf_mul(y ^ blk, h)
    return y


def gcm_decrypt(key, nonce, ct, tag, aad=b""):
    rk = expand_key(key)
    h = int.from_bytes(aes_encrypt(rk, b"\0" * 16), "big")
    j0 = nonce + b"\0\0\0\1"
    out = bytearray()
    for i in range(0, len(ct), 16):
        ctr = nonce + (2 + i // 16).to_bytes(4, "big")
        ks = aes_encrypt(rk, ctr)
        out += bytes(a ^ b for a, b in zip(ct[i:i + 16], ks))
    s = ghash(h, aad, ct)
    t = (int.from_bytes(aes_encrypt(rk, j0), "big") ^ s).to_bytes(16, "big")
    return bytes(out), t == tag


def self_test():
    # FIPS-197 C.1 (AES-128)
    rk = expand_key(bytes(range(16)))
    assert aes_encrypt(rk, bytes.fromhex("00112233445566778899aabbccddeeff")).hex() == "69c4e0d86a7b0430d8cdb78070b4c55a", "AES self-test"
    # GCM test case 3 (McGrew / Viega), 96-bit IV
    k = bytes.fromhex("feffe9928665731c6d6a8f9467308308")
    iv = bytes.fromhex("cafebabefacedbaddecaf888")
    ct = bytes.fromhex("42831ec2217774244b7221b784d0d49ce3aa212f2c02a4e035c17e2329aca12e21d514b25466931c7d8f6a5aac84aa051ba30b396a0aac973d58e091473f5985")
    tag = bytes.fromhex("4d5c2af327cd64a62cf35abd2ba6fab4")
    pt, ok = gcm_decrypt(k, iv, ct, tag)
    assert ok and pt.hex().startswith("d9313225f88406e5"), "GCM self-test"


# ---------------------------------------------------------------- the log
LINE = re.compile(r"\[online-menu\] (SendTo|RecvFrom)\(fd \d+, ([\d.]+:\d+)\) (\d+) bytes: ([0-9a-f ]+)")


def messages(pt):
    """Pia 5.18-5.26 messages (presence flags; fields not present copied from the previous message)."""
    o, prev, out = 0, {"mflags": 0, "size": 0, "ptype": 0, "pport": 0, "dst": 0, "src": 0}, []
    while o < len(pt) and pt[o] != 0xFF:
        f = pt[o]
        o += 1
        m = dict(prev)
        if f & 1:
            m["mflags"] = pt[o]; o += 1
        if f & 2:
            m["size"] = int.from_bytes(pt[o:o + 2], "big"); o += 2
        if f & 4:
            m["ptype"] = pt[o]; m["pport"] = int.from_bytes(pt[o + 1:o + 4], "big"); o += 4
        if f & 8:
            m["dst"] = int.from_bytes(pt[o:o + 8], "big"); o += 8
        if f & 16:
            m["src"] = int.from_bytes(pt[o:o + 8], "big"); o += 8
        m["payload"] = pt[o:o + m["size"]]
        o += m["size"]
        o += -o % 4
        out.append(m)
        prev = m
        if m["size"] == 0 and not f:
            break
    return out


def main():
    self_test()
    log, keyhex, gid = sys.argv[1], sys.argv[2], int(sys.argv[3])
    key = bytes.fromhex(keyhex)[:16]
    for line in open(log, encoding="utf-8", errors="replace"):
        m = LINE.search(line)
        if not m:
            continue
        d = bytes.fromhex(m.group(4).replace(" ", ""))
        if len(d) < 32 or d[:4] != b"\x32\xab\x98\x64" or not d[4] & 0x80:
            continue
        nonce = bytes([d[5]]) + (gid & 0xFFFFFF).to_bytes(3, "big") + d[8:16]
        pt, ok = gcm_decrypt(key, nonce, d[32:], d[16:32])
        if not ok:                                         # try the header as additional data, then little-endian gid
            for aad, n2 in ((d[:16], nonce), (b"", bytes([d[5]]) + (gid & 0xFFFFFF).to_bytes(3, "little") + d[8:16])):
                pt2, ok2 = gcm_decrypt(key, n2, d[32:], d[16:32], aad)
                if ok2:
                    pt, ok = pt2, True
                    break
        arrow = "->" if m.group(1) == "SendTo" else "<-"
        print("%s %s cid %d pkt %d %d bytes tag %s" % (arrow, m.group(2), d[5], int.from_bytes(d[6:8], "big"), len(d), "OK" if ok else "BAD"))
        if ok:
            for msg in messages(pt):
                print("     proto %3d port %d  dst %016x src %016x mflags %x  %d bytes: %s" % (
                    msg["ptype"], msg["pport"], msg["dst"], msg["src"], msg["mflags"], len(msg["payload"]), msg["payload"].hex(" ")))


if __name__ == "__main__":
    main()
