"""Write docs/pbrange_test.mid: MPE file where member channels have DIFFERENT pitch-bend
ranges (ch2 +/-2 st, ch3 +/-12 st). The same full-scale bend is sent on both, one note
after the other; a host that keeps per-channel ranges untouched gives +2 st vs +12 st."""
import struct, sys

def vlq(n):
    b = [n & 0x7f]; n >>= 7
    while n:
        b.append((n & 0x7f) | 0x80); n >>= 7
    return bytes(reversed(b))

TPS = 960
ev = []
def add(t, *b): ev.append((int(round(t * TPS)), bytes(b)))

def cc(ch, n, v): add(0.0, 0xB0 | ch, n, v)
# MPE lower zone, 15 members (RPN 6 on the master channel)
cc(0, 101, 0); cc(0, 100, 6); cc(0, 6, 15)
# per-channel bend range (RPN 0): channel index 1 = ch2, 2 = ch3
R2, R3 = (int(sys.argv[2]), int(sys.argv[3])) if len(sys.argv) > 3 else (2, 12)
for ch, semis in ((1, R2), (2, R3)):
    cc(ch, 101, 0); cc(ch, 100, 0); cc(ch, 6, semis); cc(ch, 38, 0)

def phrase(ch, t0):
    add(t0, 0xE0 | ch, 0x00, 0x40)          # centre
    add(t0, 0x90 | ch, 57, 100)             # A3 = 220 Hz
    for k in range(0, 41):                  # pressure up (some instruments sound from pressure)
        add(t0 + k * 0.01, 0xD0 | ch, min(127, 30 + k * 3))
    add(t0 + 0.4, 0xD0 | ch, 110)
    add(t0 + 1.0, 0xE0 | ch, 0x7f, 0x7f)    # full-scale up bend
    add(t0 + 2.0, 0xE0 | ch, 0x00, 0x40)    # back to centre
    add(t0 + 2.6, 0x80 | ch, 57, 0)

phrase(1, 0.5)   # ch2, range +/-2
phrase(2, 4.0)   # ch3, range +/-12

ev.sort(key=lambda e: e[0])
trk = bytearray(vlq(0) + b'\xff\x51\x03\x07\xa1\x20'); last = 0
for t, b in ev:
    trk += vlq(t - last) + b; last = t
trk += vlq(0) + b'\xff\x2f\x00'
out = sys.argv[1] if len(sys.argv) > 1 else 'docs/pbrange_test.mid'
open(out, 'wb').write(b'MThd' + struct.pack('>IHHH', 6, 0, 1, 480) + b'MTrk' + struct.pack('>I', len(trk)) + bytes(trk))
print(out)
