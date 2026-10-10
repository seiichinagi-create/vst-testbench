"""Fundamental-frequency track of a mono-ish wav (peak of the spectrum near an expected pitch).
usage: pitch_track.py file.wav f_expected_hz [window_s] -> prints time, f0, cents vs expected"""
import sys, wave, struct
import numpy as np

def read(path):
    with open(path, 'rb') as f:
        data = f.read()
    # float32 wav written by JUCE: parse the chunks by hand (wave module rejects format 3)
    pos = 12; fmt = None
    while pos < len(data):
        cid = data[pos:pos+4]; size = struct.unpack('<I', data[pos+4:pos+8])[0]
        if cid == b'fmt ':
            tag, ch, rate, _, _, bits = struct.unpack('<HHIIHH', data[pos+8:pos+24]); fmt = (tag, ch, rate, bits)
        elif cid == b'data':
            tag, ch, rate, bits = fmt
            x = np.frombuffer(data[pos+8:pos+8+size], dtype='<f4' if tag == 3 else '<i2').astype(np.float64)
            return x.reshape(-1, ch).mean(axis=1), rate
        pos += 8 + size + (size & 1)

def track(x, rate, f_exp, win=0.15, lo=0.4, hi=3.2):
    n = int(win * rate); nfft = 1 << 18
    out = []
    for s in range(0, len(x) - n, n // 2):
        seg = x[s:s+n] * np.hanning(n)
        if np.sqrt(np.mean(seg ** 2)) < 1e-4:
            out.append((s / rate, None)); continue
        sp = np.abs(np.fft.rfft(seg, nfft)); fr = np.fft.rfftfreq(nfft, 1 / rate)
        m = (fr > f_exp * lo) & (fr < f_exp * hi)
        out.append((s / rate, fr[m][np.argmax(sp[m])]))
    return out

if __name__ == '__main__':
    x, rate = read(sys.argv[1]); fe = float(sys.argv[2])
    for t, f in track(x, rate, fe, float(sys.argv[3]) if len(sys.argv) > 3 else 0.15):
        print('%5.2f  %s' % (t, '  -' if f is None else '%7.1f Hz  %+7.0f cents' % (f, 1200 * np.log2(f / fe))))
