"""Write docs/topbend_test.mid: ONE channel, a held chord plus a top voice that is bent
heavily; a higher note joins as the new top halfway through (bend must not move to it)."""
import math, struct, sys

def vlq(n):
    b = [n & 0x7f]; n >>= 7
    while n:
        b.append((n & 0x7f) | 0x80); n >>= 7
    return bytes(reversed(b))

TPS = 960   # ticks per second at 480 tpq, 120 bpm
ev = []
def add(t, *b): ev.append((int(t * TPS), bytes(b)))

def bend(t, v):   # v in -1..1 of the default +/-2 semitone range
    raw = 8192 + int(v * 8191)
    add(t, 0xE0, raw & 0x7f, (raw >> 7) & 0x7f)

for n in (48, 55, 60, 64):                       # held chord, no bend
    add(0.0, 0x90, n, 90); add(5.0, 0x80, n, 0)
add(1.0, 0x90, 72, 100)                          # top voice
add(3.0, 0x90, 76, 100)                          # new top: old top (72) must return to centre
add(4.0, 0x80, 76, 0)                            # 76 off: 72 is top again (starts un-bent)
add(4.6, 0x80, 72, 0)
t = 1.0
while t < 4.6:                                   # dense bend: 12 Hz wobble growing, 100 events/s
    bend(t, 0.9 * math.sin(2 * math.pi * 3 * (t - 1.0)) * min(1.0, (t - 1.0) / 1.0))
    t += 0.01
bend(4.6, 0.0)

ev.sort(key=lambda e: e[0])
trk = bytearray(vlq(0) + b'\xff\x51\x03\x07\xa1\x20'); last = 0
for t, b in ev:
    trk += vlq(t - last) + b; last = t
trk += vlq(0) + b'\xff\x2f\x00'
out = sys.argv[1] if len(sys.argv) > 1 else 'docs/topbend_test.mid'
open(out, 'wb').write(b'MThd' + struct.pack('>IHHH', 6, 0, 1, 480) + b'MTrk' + struct.pack('>I', len(trk)) + bytes(trk))
print(out)
