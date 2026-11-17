"""Convert a source WAV into the device's sound format.

Output: PCM WAV, 16-bit, mono, 16000 Hz, peak -3 dBFS, short fade-in,
leading silence trimmed, optional length cap with a fade-out.

    python esphome/sounds/prepare_sound.py SRC.wav esphome/sounds/alert.wav --max 2.6 --fade-out 0.5

Requires numpy. Integer-ratio and arbitrary resampling use a windowed-sinc
low-pass so nothing above 8 kHz folds back as aliasing.
"""
import argparse
import wave

import numpy as np

RATE = 16000


def read(path):
    with wave.open(path) as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    if width == 2:
        x = np.frombuffer(raw, "<i2").astype(np.float64) / 32768.0
    elif width == 3:
        b = np.frombuffer(raw, np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        x = np.where(v & 0x800000, v - 0x1000000, v).astype(np.float64) / 8388608.0
    elif width == 4:
        x = np.frombuffer(raw, "<i4").astype(np.float64) / 2147483648.0
    elif width == 1:
        x = (np.frombuffer(raw, np.uint8).astype(np.float64) - 128.0) / 128.0
    else:
        raise SystemExit(f"unsupported sample width {width}")
    return x.reshape(-1, ch).mean(axis=1), rate


def resample(x, src, dst, taps=64):
    if src == dst:
        return x
    cutoff = 0.45 * min(src, dst) / src  # cycles per source sample
    t = np.arange(int(len(x) * dst / src)) * (src / dst)
    out = np.zeros(len(t))
    base = np.floor(t).astype(np.int64)
    for k in range(-taps, taps + 1):
        idx = base + k
        d = t - idx
        h = 2 * cutoff * np.sinc(2 * cutoff * d) * (0.5 + 0.5 * np.cos(np.pi * np.clip(d / (taps + 1), -1, 1)))
        valid = (idx >= 0) & (idx < len(x))
        out[valid] += x[idx[valid]] * h[valid]
    return out


def main():
    p = argparse.ArgumentParser()
    p.add_argument("src")
    p.add_argument("dst")
    p.add_argument("--max", type=float, default=0.0, help="cap length in seconds (0 = keep)")
    p.add_argument("--fade-in", type=float, default=0.005)
    p.add_argument("--fade-out", type=float, default=0.010)
    p.add_argument("--peak-db", type=float, default=-3.0)
    a = p.parse_args()

    x, rate = read(a.src)
    x = resample(x, rate, RATE)
    start = int(np.argmax(np.abs(x) > 10 ** (-50 / 20)))  # first sample above -50 dBFS
    x = x[start:]
    if a.max > 0:
        x = x[: int(a.max * RATE)]
    n_in, n_out = int(a.fade_in * RATE), int(a.fade_out * RATE)
    if n_in:
        x[:n_in] *= np.linspace(0.0, 1.0, n_in)
    if n_out:
        x[-n_out:] *= 0.5 + 0.5 * np.cos(np.linspace(0.0, np.pi, n_out))
    x *= 10 ** (a.peak_db / 20) / max(np.abs(x).max(), 1e-9)
    pcm = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")
    with wave.open(a.dst, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(pcm.tobytes())
    print(f"{a.dst}: {len(pcm) / RATE:.2f} s, {len(pcm) * 2 + 44} bytes")


if __name__ == "__main__":
    main()
