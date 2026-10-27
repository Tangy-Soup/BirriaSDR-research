"""FCS-constrained recovery of 1 Mbps DBPSK payloads (research tier).

A failed 1 Mbps payload whose PLCP header passed its CRC is re-sliced
coherently, then corrected within a bounded, FCS-checked search.

1. **Coherent slicing.** The carrier phase is estimated non-causally from the
   squared, amplitude-weighted symbols over +/-`HALF_WINDOW` symbols
   (squaring removes BPSK modulation) and unwrapped so the 180-degree
   ambiguity stays consistent. Each symbol is sliced on Re(z), and the
   differential decisions are XORs of consecutive symbol decisions, so a
   global flip cancels. A noisy symbol then corrupts both decisions that use
   it as one event of known confidence |Re z|. Ground truth from a paired
   stock receiver showed this needs far fewer unknowns than ranking
   differential products.
2. **GF(2) solve.** The descrambler (`d[j] = raw[j] ^ raw[j-4] ^ raw[j-7]`)
   and CRC-32 are linear, so each flip pattern changes the FCS syndrome by a
   fixed vector. Candidates are single-symbol flips in increasing |Re z|
   (scaled down for symbols over exact-zero runs), then single-decision
   phase slips. The first prefix of at most `MAX_UNKNOWNS` independent
   patterns whose span contains the syndrome, with a unique solution, is
   accepted. With N unknowns, at least 32 - N FCS bits remain as an
   independent check. A random syndrome passes with probability about
   2**(N - 32) per prefix: about 1.9e-6 in total for N <= 12 (measured
   1.5e-6 over 2.05 million random syndromes on real candidate columns).

Decisions before the payload stay fixed (the PLCP CRC covered them). Nothing
here uses reference or expected bytes. A coherent re-slice that passes the FCS
with no flips keeps all 32 check bits. Every result is reported in a separate
tier, never as an unaided FCS-valid frame.
"""
import zlib

import numpy as np

MAX_UNKNOWNS = 12
HALF_WINDOW = 16


def coherent_decisions(sym, erased=None, half_window=HALF_WINDOW):
    """Return (symbol decisions b, derotated symbols z)."""
    w = np.abs(sym)
    if erased is not None:
        w = w * (1 - erased[:len(sym)])
    s2 = (sym / np.maximum(np.abs(sym), 1e-30)) ** 2 * w
    csum = np.r_[0, np.cumsum(s2)]
    n = len(sym)
    lo = np.clip(np.arange(n) - half_window, 0, n)
    hi = np.clip(np.arange(n) + half_window + 1, 0, n)
    phase = np.unwrap(np.angle(csum[hi] - csum[lo])) / 2
    z = sym * np.exp(-1j * phase)
    return (np.real(z) < 0).astype(np.int8), z


def descramble_frame(raw, start, length):
    d = raw[start:start + length] ^ raw[start - 4:start + length - 4] ^ raw[start - 7:start + length - 7]
    return np.packbits(d.astype(np.uint8), bitorder='little').tobytes()


def syndrome(frame):
    return zlib.crc32(frame[:-4]) ^ int.from_bytes(frame[-4:], 'little')


def _flip_effect(raw_indices, start, length):
    """Payload bit indices flipped by flipping the given raw decisions."""
    hit = set()
    for j in raw_indices:
        for i in (j, j + 4, j + 7):
            if start <= i < start + length:
                hit ^= {i - start}
    return sorted(hit)


def _syndrome_delta(bits, nbytes, zero_crc):
    e = np.zeros(nbytes * 8, np.uint8)
    e[bits] = 1
    eb = np.packbits(e, bitorder='little').tobytes()
    return zlib.crc32(eb[:-4]) ^ zero_crc ^ int.from_bytes(eb[-4:], 'little')


def candidates(z, start, length, erased=None):
    """Flip patterns (as raw-decision tuples) in increasing confidence."""
    conf = np.abs(np.real(z))
    conf = conf / max(float(np.median(conf[start:start + length + 1])), 1e-30)
    if erased is not None:
        conf = conf * (1 - erased[:len(conf)])
    items = []
    for k in range(start + 1, start + length + 1):     # symbol k feeds decisions k-1 and k
        raws = tuple(r for r in (k - 1, k) if start <= r < start + length)
        items.append((float(conf[k]), raws))
    for j in range(start, start + length):             # a phase slip flips one decision
        items.append((float(conf[j] + conf[j + 1]), (j,)))
    items.sort(key=lambda t: t[0])
    return [raws for _, raws in items]


def recover(frame, sym, start, length, erased=None, max_unknowns=MAX_UNKNOWNS):
    """Try to correct a failed 1 Mbps frame. Returns (frame or None, info)."""
    nbytes = length // 8
    if nbytes < 5 or len(frame) != nbytes or len(sym) < start + length + 1 or start < 7:
        return None, {'reason': 'unsupported geometry'}
    b, z = coherent_decisions(sym, erased)
    raw = (np.real(sym[1:] * np.conj(sym[:-1])) < 0).astype(np.int8)   # keeps PLCP-protected history
    raw[start:start + length] = b[start + 1:start + length + 1] ^ b[start:start + length]
    base = descramble_frame(raw, start, length)
    target = syndrome(base)
    info = {'method': 'coherent'}
    if target == 0:
        return base, info | {'unknowns': 0, 'check_bits_left': 32, 'flipped_payload_bits': 0}
    zero_crc = zlib.crc32(bytes(nbytes - 4))
    basis, pattern_basis, columns = {}, {}, []
    for raws in candidates(z, start, length, erased):
        mask = sum(1 << (r - start) for r in raws)
        if not _insert(dict(pattern_basis), mask, 0):
            continue                                   # pattern already spanned
        bits = _flip_effect(raws, start, length)
        delta = _syndrome_delta(bits, nbytes, zero_crc)
        idx = len(columns)
        if not _insert(basis, delta, 1 << idx):
            continue                                   # would make the solution ambiguous
        _insert(pattern_basis, mask, 0)
        columns.append((raws, bits))
        solution = _solve(basis, target)
        if solution is not None:
            chosen = [columns[i] for i in range(idx + 1) if solution >> i & 1]
            fixed = bytearray(base)
            for _, flip in chosen:
                for bit in flip:
                    fixed[bit // 8] ^= 1 << (bit % 8)
            fixed = bytes(fixed)
            if syndrome(fixed) != 0:
                return None, info | {'reason': 'internal inconsistency'}
            return fixed, info | {'unknowns': idx + 1, 'check_bits_left': 32 - (idx + 1),
                                  'flips': [list(r) for r, _ in chosen],
                                  'flipped_payload_bits': sum(len(f) for _, f in chosen)}
        if idx + 1 >= max_unknowns:
            break
    return None, info | {'reason': 'no solution within limit', 'unknowns': len(columns)}


def _insert(basis, vec, comb):
    while vec:
        top = vec.bit_length() - 1
        if top not in basis:
            basis[top] = (vec, comb)
            return True
        bv, bc = basis[top]
        vec ^= bv
        comb ^= bc
    return False


def _solve(basis, target):
    vec, comb = target, 0
    while vec:
        top = vec.bit_length() - 1
        if top not in basis:
            return None
        bv, bc = basis[top]
        vec ^= bv
        comb ^= bc
    return comb
