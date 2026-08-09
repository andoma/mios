#!/usr/bin/env python3
"""Estimate distance from two mios `cs_data` dumps (Channel Sounding mode-2 PBR).

Run CS between two mios nRF54L15 devices, then `cs_data` on each. Save the
initiator's output to one file and the reflector's to another (the "cs_tones N"
header line is ignored; tone lines are "channel i q quality"). Then:

    cs_distance.py <initiator_dump> <reflector_dump>

Phase-slope method (same as Nordic's connected_cs distance_estimation.c): per
common channel combine the two devices' tones (complex product), take the
phase, then the slope of unwrapped phase vs frequency gives the distance.
"""
import sys
import math

C = 299792458.0  # m/s


def load(path):
    tones = {}  # channel -> (i, q, quality)
    with open(path) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 4:
                continue
            try:
                ch, i, q, qual = (int(x) for x in parts)
            except ValueError:
                continue  # skips the "cs_tones N" header
            tones[ch] = (i, q, qual)
    return tones


def main():
    # Optional calibration flags:
    #   --offset M   subtract a known board/antenna offset (metres) from the result
    #   --true M     print the offset needed so the result equals M (one-point cal)
    argv = sys.argv[1:]
    offset = 0.0
    truth = None
    files = []
    i = 0
    while i < len(argv):
        if argv[i] == "--offset":
            offset = float(argv[i + 1]); i += 2
        elif argv[i] == "--true":
            truth = float(argv[i + 1]); i += 2
        else:
            files.append(argv[i]); i += 1
    if len(files) != 2:
        sys.exit("usage: cs_distance.py [--offset M | --true M] "
                 "<initiator_dump> <reflector_dump>")
    a = load(files[0])
    b = load(files[1])

    pts = []  # (freq_mhz, phase)
    for ch in sorted(set(a) & set(b)):
        li, lq, lqual = a[ch]
        pi, pq, pqual = b[ch]
        if lqual != 0 or pqual != 0:
            continue  # 0 = good tone quality
        # combined = local * peer  (cancels the frequency offset, doubles ToF)
        ci = li * pi - lq * pq
        cq = li * pq + lq * pi
        pts.append((2402.0 + ch, math.atan2(cq, ci)))

    if len(pts) < 2:
        sys.exit(f"not enough good common tones ({len(pts)})")

    pts.sort()
    freqs = [p[0] for p in pts]
    theta = [p[1] for p in pts]

    # 1-D phase unwrap
    for k in range(1, len(theta)):
        d = theta[k] - theta[k - 1]
        if d > math.pi:
            for j in range(k, len(theta)):
                theta[j] -= 2 * math.pi
        elif d < -math.pi:
            for j in range(k, len(theta)):
                theta[j] += 2 * math.pi

    # slope b in theta = a + b*freq  (freq in MHz)
    n = len(freqs)
    fm = sum(freqs) / n
    tm = sum(theta) / n
    num = sum((freqs[k] - fm) * (theta[k] - tm) for k in range(n))
    den = sum((freqs[k] - fm) ** 2 for k in range(n))
    slope = num / den  # rad per MHz

    # distance = -slope * c/(4pi); slope is rad/MHz so divide by 1e6 for meters
    dist = -slope * (C / (4 * math.pi)) / 1e6
    print(f"tones used: {n}")
    print(f"phase slope: {slope:.4f} rad/MHz")
    print(f"raw distance: {dist:.3f} m")
    if truth is not None:
        print(f"board offset (to read {truth:.3f} m): {dist - truth:.3f} m")
    if offset:
        print(f"calibrated distance: {dist - offset:.3f} m  (offset {offset:.3f} m)")


if __name__ == "__main__":
    main()
