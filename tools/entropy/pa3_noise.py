#!/usr/bin/env python3
"""Noise and min-entropy analysis of raw ADC samples (#14, docs/design.md "Entropy").

Input: raw blocks of little-endian uint16 ADC readings (12-bit values, 0xFFFF
marks a failed read), one file per block, grouped by a label:

    python3 -I tools/entropy/pa3_noise.py [--skip 64] [--h 1.0] \
        3cyc='/path/smp0_*.u16' 480cyc='/path/smp7_*.u16'

Per label it prints: sample count, mean and spread, how much of the spread
repeats from block to block at the same index (interference locked to the
sampling loop would), autocorrelation, per-bit bias, LSB histogram flatness,
and these min-entropy estimates from NIST SP 800-90B section 6.3 (from the
standard's description; checked against the structure of NIST's reference
code, not against its output):

  - MCV (6.3.1) on the full 12-bit value and on the low 8 bits,
  - t-tuple (6.3.5) and LRS (6.3.6) on the low 8 bits,
  - MultiMCW (6.3.7) and Lag (6.3.8) prediction on the low 8 bits,
  - Markov (6.3.3) on bit 0 alone, as a per-bit figure,

and one predictor of our own (not in SP 800-90B): each reading predicted
from what followed the nearest match of its last 4 readings in the previous
1024, which catches a repeating waveform with drifting phase.

Collision, compression, MultiMMC and LZ78Y are not implemented. Prediction
estimators use simplified tie rules (argmax), which changes their results only
slightly. The minimum over all estimators is the conservative per-sample
estimate. With --h it also prints the SP 800-90B 4.4 health-test cutoffs for a
claimed entropy of H bits per sample.

Captures stay outside git (docs/test-bench.md); this script writes nothing.
"""
import argparse
import glob
import math
import os
import sys

import numpy as np
from scipy import stats

Z = 2.5758293035489008  # 99 % upper bound, as in SP 800-90B


def load(pattern, skip):
    blocks = []
    for f in sorted(glob.glob(pattern)):
        a = np.fromfile(f, dtype='<u2')
        if a.size <= skip:
            continue
        blocks.append(a[skip:])
    if not blocks:
        sys.exit(f'no blocks for {pattern}')
    n = max(b.size for b in blocks)
    full = [b for b in blocks if b.size == n]  # a dump cut short is dropped
    return full, len(blocks) - len(full)


def h_from_p(p):
    return -math.log2(min(1.0, p))


def mcv(s):
    _, c = np.unique(s, return_counts=True)
    p = c.max() / s.size
    pu = min(1.0, p + Z * math.sqrt(p * (1 - p) / (s.size - 1)))
    return h_from_p(pu)


def tuple_counts(s, t):
    w = np.lib.stride_tricks.sliding_window_view(s, t)
    if t <= 8:
        key = np.zeros(w.shape[0], dtype=np.uint64)
        for j in range(t):
            key = (key << np.uint64(8)) | w[:, j].astype(np.uint64)
        _, c = np.unique(key, return_counts=True)
    else:
        v = np.ascontiguousarray(w).view(np.dtype((np.void, t))).ravel()
        _, c = np.unique(v, return_counts=True)
    return c


def t_tuple_and_lrs(s):
    L = s.size
    pmax, t, u = 0.0, 1, None
    while True:
        c = tuple_counts(s, t)
        q = c.max()
        if q < 35:
            u = t
            break
        pmax = max(pmax, (q / (L - t + 1)) ** (1.0 / t))
        t += 1
    pu = min(1.0, pmax + Z * math.sqrt(pmax * (1 - pmax) / (L - 1)))
    h_tt = h_from_p(pu)
    # LRS: W from u up to the longest repeated W-tuple length v
    pmax = 0.0
    W = u
    while True:
        c = tuple_counts(s, W)
        if c.max() < 2:
            break
        n = L - W + 1
        pw = float(np.sum(c * (c - 1) / 2.0)) / (n * (n - 1) / 2.0)
        pmax = max(pmax, pw ** (1.0 / W))
        W += 1
    if pmax == 0.0:
        return h_tt, float('nan')
    pu = min(1.0, pmax + Z * math.sqrt(pmax * (1 - pmax) / (L - 1)))
    return h_tt, h_from_p(pu)


def pred_fn(p, r, N):
    q = 1.0 - p
    x, xl = 1.0, 0.0
    for _ in range(66):
        if x - xl <= 1e-15 * x:
            break
        xl = x
        x = 1.0 + q * p**r * x ** (r + 1.0)
    return math.log(1.0 - p * x) - math.log((r + 1.0 - r * x) * q) - (N + 1.0) * math.log(x)


def prediction_estimate(C, N, run, k):
    pg = C / N
    pgp = min(1.0, pg + Z * math.sqrt(pg * (1 - pg) / (N - 1))) if pg > 0 else 1 - 0.01 ** (1 / N)
    cur = max(1.0 / k, pgp)
    r = run + 1
    if cur < 1.0 and pred_fn(cur, r, N) > math.log(0.99):
        lo, hi = cur, 1.0 - 1e-12
        for _ in range(200):
            mid = (lo + hi) / 2
            if pred_fn(mid, r, N) > math.log(0.99):
                lo = mid
            else:
                hi = mid
        cur = max(cur, lo)
    return h_from_p(cur)


def longest_run(b):
    if not b.any():
        return 0
    d = np.diff(np.concatenate(([0], b.astype(np.int8), [0])))
    return int((np.flatnonzero(d == -1) - np.flatnonzero(d == 1)).max())


def lag_estimate(s, D=128, chunk=1 << 15):
    """6.3.8: predict s[i] = s[i-d] with the lag d that scored best so far."""
    L = s.size
    score = np.zeros(D, dtype=np.int64)
    correct = np.zeros(L - D, dtype=bool)
    lags = np.arange(1, D + 1)
    for a in range(D, L, chunk):
        b = min(L, a + chunk)
        idx = np.arange(a, b)
        hits = s[idx][None, :] == s[idx[None, :] - lags[:, None]]  # D x n
        cum = np.cumsum(hits, axis=1) + score[:, None]
        before = np.concatenate((score[:, None], cum[:, :-1]), axis=1)
        win = np.argmax(before, axis=0)  # best lag index using scores before i
        correct[a - D:b - D] = hits[win, np.arange(b - a)]
        score = cum[:, -1]
    return prediction_estimate(int(correct.sum()), correct.size, longest_run(correct), 256)


def window_modes(s, w, start, chunk=1 << 14, k=256):
    """Most common value in s[i-w:i] for i >= start; ties go to the most recent."""
    L = s.size
    out = np.empty(L - start, dtype=np.int64)
    for a in range(start, L, chunk):
        b = min(L, a + chunk)
        seg = s[a - w:b]
        oh = np.zeros((seg.size, k), dtype=np.int32)
        oh[np.arange(seg.size), seg] = 1
        cs = np.cumsum(oh, axis=0)
        cs = np.vstack((np.zeros((1, k), dtype=np.int32), cs))
        cnt = cs[w:w + (b - a)] - cs[0:b - a]  # counts in the w samples before each i
        pos = np.where(oh == 1, np.arange(seg.size)[:, None], -1)
        last = np.maximum.accumulate(pos, axis=0)
        last = np.vstack((np.full((1, k), -1), last))[w:w + (b - a)]
        rel = last - np.arange(b - a)[:, None]  # -w..-1 inside the window
        key = cnt.astype(np.int64) * (w + 2) + np.clip(rel, -w - 1, 0) + w + 1
        out[a - start:b - start] = np.argmax(key, axis=1)
    return out


def multi_mcw_estimate(s, wins=(63, 255, 1023, 4095)):
    start = wins[-1]
    preds = np.stack([window_modes(s, w, start) for w in wins])  # 4 x N
    actual = s[start:]
    hits = preds == actual[None, :]
    cum = np.cumsum(hits, axis=1)
    before = np.concatenate((np.zeros((len(wins), 1), dtype=cum.dtype), cum[:, :-1]), axis=1)
    win = (len(wins) - 1) - np.argmax(before[::-1], axis=0)  # ties to the larger window
    correct = hits[win, np.arange(actual.size)]
    return prediction_estimate(int(correct.sum()), correct.size, longest_run(correct), 256)


def nearest_neighbour(blocks, k=4, W=1024, n_eval=20000, seed=1):
    """Not from SP 800-90B. Predicts each reading from the reading that
    followed the closest match (Euclidean, k readings) of its context in the
    previous W readings of the same block, as an attacker who knows the
    waveform shape would. Returns the residual sd and -log2 of the 99 % upper
    bound of the most likely residual: the min-entropy left after this
    prediction."""
    rng = np.random.default_rng(seed)
    A = np.stack(blocks).astype(np.float64)
    nb, L = A.shape
    picks = [(int(rng.integers(nb)), int(rng.integers(W + k, L))) for _ in range(n_eval)]
    res = []
    for bi, i in picks:
        x = A[bi]
        if (x[i - W - k:i + 1] == 0xFFFF).any():
            continue
        ctx = x[i - k:i]
        win = np.lib.stride_tricks.sliding_window_view(x[i - W - k:i - 1], k)  # contexts before i
        nxt = x[i - W:i]  # reading after each context
        d = np.sum((win[:W] - ctx) ** 2, axis=1)
        res.append(x[i] - nxt[int(np.argmin(d))])
    r = np.array(res, dtype=np.int64)
    _, c = np.unique(r, return_counts=True)
    p = c.max() / r.size
    pu = min(1.0, p + Z * math.sqrt(p * (1 - p) / (r.size - 1)))
    return float(r.std()), h_from_p(pu)


def markov_bit(b):
    """6.3.3 on one bit stream: min-entropy per bit of the most likely 128-bit path."""
    b = b.astype(np.int64)
    p1 = b.mean()
    p0 = 1 - p1
    t = np.zeros((2, 2))
    for x in (0, 1):
        m = b[:-1] == x
        t[x, 1] = b[1:][m].mean() if m.any() else 0.0
        t[x, 0] = 1 - t[x, 1]
    with np.errstate(divide='ignore'):
        lt = np.log2(t)
    paths = [
        math.log2(p0) + 127 * lt[0, 0] if p0 > 0 else -np.inf,
        math.log2(p0) + 63 * (lt[0, 1] + lt[1, 0]) + lt[0, 0] if p0 > 0 else -np.inf,
        math.log2(p0) + lt[0, 1] + 126 * lt[1, 1] if p0 > 0 else -np.inf,
        math.log2(p1) + lt[1, 0] + 126 * lt[0, 0] if p1 > 0 else -np.inf,
        math.log2(p1) + 63 * (lt[1, 0] + lt[0, 1]) + lt[1, 1] if p1 > 0 else -np.inf,
        math.log2(p1) + 127 * lt[1, 1] if p1 > 0 else -np.inf,
    ]
    return min(1.0, -max(paths) / 128.0)


def health_cutoffs(h, alpha_log2=-30, W=512):
    """SP 800-90B 4.4.1 RCT and 4.4.2 APT cutoffs for H bits per sample."""
    rct = 1 + math.ceil(-alpha_log2 / h)
    p = 2.0 ** -h
    alpha = 2.0**alpha_log2
    # C = 1 + CRITBINOM(W, p, 1 - alpha): smallest k with P(X <= k) >= 1 - alpha
    k = int(stats.binom.isf(alpha, W, p))
    while stats.binom.sf(k, W, p) > alpha:
        k += 1
    while k > 0 and stats.binom.sf(k - 1, W, p) <= alpha:
        k -= 1
    return rct, 1 + k


def analyse(label, pattern, skip):
    blocks, short = load(pattern, skip)
    A = np.stack(blocks).astype(np.int64)
    bad = A == 0xFFFF
    nbad = int(bad.sum())
    good = np.where(bad, np.nan, A.astype(float))
    print(f'== {label}: {A.shape[0]} blocks x {A.shape[1]} samples ({short} short blocks dropped), '
          f'{nbad} failed reads dropped')
    mu, sd = np.nanmean(good), np.nanstd(good)
    print(f'  value: mean {mu:.1f}, sd {sd:.2f} LSB, range {np.nanmin(good):.0f}-{np.nanmax(good):.0f}')
    # Block-to-block repeatability: mean over blocks at each index. Independent
    # noise gives sd/sqrt(nblocks); locked interference stays near sd.
    ens = np.nanmean(good, axis=0)
    print(f'  same index across blocks: sd of the mean {np.std(ens):.2f} LSB '
          f'(independent noise: {sd / math.sqrt(A.shape[0]):.2f})')
    X = np.nan_to_num(good - np.nanmean(good, axis=1, keepdims=True))
    ac = [float(np.mean(np.sum(X[:, :-k] * X[:, k:], 1) / np.sum(X * X, 1))) for k in (1, 2, 3, 4, 8, 16, 64)]
    print('  autocorrelation lag 1,2,3,4,8,16,64: ' + ' '.join(f'{a:+.3f}' for a in ac))

    s12 = A[~bad]
    bits = [(s12 >> j) & 1 for j in range(12)]
    print('  P(bit = 1), bit 0..11: ' + ' '.join(f'{b.mean():.3f}' for b in bits))
    s4 = s12 & 0xF
    c4 = np.bincount(s4, minlength=16)
    chi = stats.chisquare(c4)
    print(f'  low 4 bits: max/min count {c4.max() / c4.min():.3f}, chi2 p {chi.pvalue:.3g}')
    b0 = np.concatenate([((b[b != 0xFFFF]) & 1) for b in A])
    ac0 = float(np.corrcoef(b0[:-1], b0[1:])[0, 1])
    print(f'  bit 0: lag-1 correlation {ac0:+.4f}, Markov {markov_bit(b0):.3f} bit/bit')

    s8 = (s12 & 0xFF).astype(np.int64)
    est = {
        'MCV 12-bit': mcv(s12),
        'MCV low 8': mcv(s8),
    }
    est['t-tuple'], est['LRS'] = t_tuple_and_lrs(s8.astype(np.uint8))
    est['MultiMCW'] = multi_mcw_estimate(s8)
    est['Lag'] = lag_estimate(s8)
    nn_sd, est['nearest-neighbour'] = nearest_neighbour(blocks)
    print(f'  nearest-neighbour prediction residual: sd {nn_sd:.2f} LSB')
    print('  min-entropy, bits per sample: ' + ', '.join(f'{k} {v:.2f}' for k, v in est.items()))
    low = min(v for v in est.values() if not math.isnan(v))
    print(f'  minimum over estimators: {low:.2f} bits per sample')
    return low


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--skip', type=int, default=64, help='samples dropped at the start of each block (ADC settling)')
    ap.add_argument('--h', type=float, help='claimed bits per sample: print health-test cutoffs')
    ap.add_argument('sets', nargs='*', metavar='LABEL=GLOB')
    a = ap.parse_args()
    for item in a.sets:
        label, _, pattern = item.partition('=')
        analyse(label, os.path.expanduser(pattern), a.skip)
    if a.h:
        for alog in (-20, -30, -40):
            rct, apt = health_cutoffs(a.h, alog)
            print(f'health tests for H = {a.h} bit/sample, alpha 2^{alog}: RCT cutoff {rct}, APT cutoff {apt} of 512')


if __name__ == '__main__':
    main()
