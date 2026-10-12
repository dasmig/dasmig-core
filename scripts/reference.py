"""Reference implementation of the dasmig-core random algorithms.

Implements the engine and the distributions again, in Python, from their
specification in dasmig/core/*.hpp, and writes the known-answer vectors the
C++ tests compare against (tests/kat_vectors.hpp).

Python floats are IEEE 754 doubles with correctly rounded +, -, *, / and sqrt
and no fused multiply-add, so an algorithm that uses only those operations
gives the same bits here as in C++ on any conforming platform. Agreement
between this file and the C++ code is the check that the C++ output does not
depend on the compiler, the standard library or the CPU.

The engine itself is checked against published test vectors first.

Usage: python scripts/reference.py [output.hpp]   (default tests/kat_vectors.hpp)
"""

import math
import struct
import sys
from pathlib import Path

M64 = (1 << 64) - 1


# --- engine ----------------------------------------------------------------

def splitmix64_mix(z):
    z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & M64
    z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & M64
    return z ^ (z >> 31)


class SplitMix64:
    def __init__(self, seed):
        self.state = seed & M64

    def __call__(self):
        self.state = (self.state + 0x9E3779B97F4A7C15) & M64
        return splitmix64_mix(self.state)


def fnv1a64(data: bytes):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & M64
    return h


def rotl(x, k):
    return ((x << k) | (x >> (64 - k))) & M64


class Rng:
    """xoshiro256** seeded through SplitMix64."""

    def __init__(self, seed=None, state=None):
        if state is not None:
            self.s = list(state)
            self.seed = None
        else:
            self.seed = seed & M64
            expand = SplitMix64(seed)
            self.s = [expand() for _ in range(4)]

    def __call__(self):
        s = self.s
        result = (rotl((s[1] * 5) & M64, 7) * 9) & M64
        t = (s[1] << 17) & M64
        s[2] ^= s[0]
        s[3] ^= s[1]
        s[1] ^= s[2]
        s[0] ^= s[3]
        s[2] ^= t
        s[3] = rotl(s[3], 45)
        return result

    def child_seed_label(self, label: str):
        return splitmix64_mix(self.seed ^ splitmix64_mix(fnv1a64(label.encode("utf-8"))))

    def child_seed_index(self, index):
        return splitmix64_mix(self.seed ^ splitmix64_mix(index ^ 0x6A09E667F3BCC909))


# --- math -------------------------------------------------------------------

LN2_HI = 6.93147180369123816490e-01
LN2_LO = 1.90821492927058770002e-10
INV_LN2 = 1.44269504088896338700e+00
SQRT_HALF = 7.07106781186547524401e-01
EXP_MAX = 7.09782712893383973096e+02
EXP_MIN = -7.45133219101941108420e+02


def _exp_coeff():
    c, factorial = [], 1.0
    for i in range(14):
        if i > 0:
            factorial *= float(i)
        c.append(1.0 / factorial)
    return c


EXP_COEFF = _exp_coeff()
LOG_COEFF = [2.0 / float(2 * k + 1) for k in range(10)]


def det_exp(x):
    if math.isnan(x):
        return x
    if x > EXP_MAX:
        return math.inf
    if x < EXP_MIN:
        return 0.0
    k = float(math.floor((x * INV_LN2) + 0.5))
    r = (x - (k * LN2_HI)) - (k * LN2_LO)
    p = EXP_COEFF[-1]
    for c in reversed(EXP_COEFF[:-1]):
        p = (p * r) + c
    return math.ldexp(p, int(k))


def det_log(x):
    if math.isnan(x) or x < 0.0:
        return math.nan
    if x == 0.0:
        return -math.inf
    if math.isinf(x):
        return x
    m, e = math.frexp(x)
    if m < SQRT_HALF:
        m *= 2.0
        e -= 1
    f = m - 1.0
    s = f / (m + 1.0)
    z = s * s
    p = LOG_COEFF[-1]
    for c in reversed(LOG_COEFF[:-1]):
        p = (p * z) + c
    log_m = s * p
    k = float(e)
    return (k * LN2_HI) + (log_m + (k * LN2_LO))


# --- distributions ------------------------------------------------------------

def bounded(r, n):
    x = r()
    low = (x * n) & M64
    if low < n:
        threshold = ((-n) & M64) % n
        while low < threshold:
            x = r()
            low = (x * n) & M64
    return (x * n) >> 64


def uniform_int(r, lo, hi, bits, signed):
    span = (hi - lo) & M64
    offset = r() if span == M64 else bounded(r, span + 1)
    v = (lo + offset) & ((1 << bits) - 1)
    if signed and v >= 1 << (bits - 1):
        v -= 1 << bits
    return v


def uniform01(r):
    return float(r() >> 11) * 2.0 ** -53


def uniform_real(r, lo, hi):
    width = hi - lo
    u = uniform01(r)
    if width == 0.0:
        return lo
    x = lo + (width * u)
    return x if x < hi else math.nextafter(hi, lo)


def bernoulli(r, p):
    return uniform01(r) < p


def normal(r, mean, sd):
    while True:
        u = (2.0 * uniform01(r)) - 1.0
        v = (2.0 * uniform01(r)) - 1.0
        s = (u * u) + (v * v)
        if not (s >= 1.0 or s == 0.0):
            break
    factor = math.sqrt((-2.0 * det_log(s)) / s)
    return mean + (sd * (u * factor))


def lognormal(r, mu, sigma):
    return det_exp(normal(r, mu, sigma))


class Discrete:
    def __init__(self, weights):
        self.cumulative, total, self.last_positive = [], 0.0, 0
        for i, w in enumerate(weights):
            total += w
            if w > 0.0:
                self.last_positive = i
            self.cumulative.append(total)

    def __call__(self, r):
        target = uniform01(r) * self.cumulative[-1]
        for i, c in enumerate(self.cumulative):  # upper_bound
            if c > target:
                return i
        return self.last_positive


def shuffle(r, items):
    for i in range(len(items) - 1, 0, -1):
        j = bounded(r, i + 1)
        items[i], items[j] = items[j], items[i]
    return items


def sample_indices(r, n, k):
    idx = list(range(n))
    for i in range(k):
        j = i + bounded(r, n - i)
        idx[i], idx[j] = idx[j], idx[i]
    return idx[:k]


# --- published vectors ----------------------------------------------------------

def check_published():
    # SplitMix64, seed 1234567 (reference values from Vigna's splitmix64.c).
    sm = SplitMix64(1234567)
    expected = [6457827717110365317, 3203168211198807973, 9817491932198370423,
                4593380528125082431, 16408922859458223821]
    got = [sm() for _ in expected]
    assert got == expected, f"splitmix64 mismatch: {got}"

    # xoshiro256** from state {1, 2, 3, 4} (the rand_xoshiro test vector).
    r = Rng(state=[1, 2, 3, 4])
    expected = [11520, 0, 1509978240, 1215971899390074240, 1216172134540287360,
                607988272756665600, 16172922978634559625, 8476171486693032832,
                10595114339597558777, 2904607092377533576]
    got = [r() for _ in expected]
    assert got == expected, f"xoshiro256** mismatch: {got}"

    assert fnv1a64(b"") == 0xCBF29CE484222325
    assert fnv1a64(b"a") == 0xAF63DC4C8601EC8C


# --- known-answer vectors ----------------------------------------------------------

def bits(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def u64_list(values):
    return ", ".join(f"0x{v:016x}ULL" for v in values)


def int_list(values):
    return ", ".join(str(v) for v in values)


def emit():
    out = []
    add = out.append
    count = 16

    add("// Generated by scripts/reference.py. Do not edit.")
    add("// Doubles are stored as their IEEE 754 bit patterns.")
    add("#ifndef DASMIG_CORE_KAT_VECTORS_HPP")
    add("#define DASMIG_CORE_KAT_VECTORS_HPP")
    add("#include <array>")
    add("#include <cstdint>")
    add("namespace kat")
    add("{")
    add(f"inline constexpr std::size_t count = {count};")

    for seed, name in [(0, "seed_0"), (42, "seed_42"), (M64, "seed_max")]:
        r = Rng(seed)
        add(f"inline constexpr std::array<std::uint64_t, count> engine_{name}{{{u64_list(r() for _ in range(count))}}};")

    r = Rng(42)
    labels = ["", "height", "weight", "état"]
    add(f"inline constexpr std::array<std::uint64_t, {len(labels)}> child_label_42{{{u64_list(r.child_seed_label(l) for l in labels)}}};")
    indices = [0, 1, 2, M64]
    add(f"inline constexpr std::array<std::uint64_t, {len(indices)}> child_index_42{{{u64_list(r.child_seed_index(i) for i in indices)}}};")

    bounds = [1, 2, 3, 6, 10, 1000, (1 << 32) + 1, (1 << 63) + 1, M64]
    add(f"inline constexpr std::array<std::uint64_t, {len(bounds)}> bounded_n{{{u64_list(bounds)}}};")
    rows = []
    for n in bounds:
        r = Rng(7)
        rows.append("{" + u64_list(bounded(r, n) for _ in range(count)) + "}")
    add(f"inline constexpr std::array<std::array<std::uint64_t, count>, {len(bounds)}> bounded_7{{{{{', '.join(rows)}}}}};")

    r = Rng(11)
    add(f"inline constexpr std::array<std::int8_t, count> int8_full_11{{{int_list(uniform_int(r, -128, 127, 8, True) for _ in range(count))}}};")
    r = Rng(11)
    add(f"inline constexpr std::array<int, count> int_m5_5_11{{{int_list(uniform_int(r, -5, 5, 32, True) for _ in range(count))}}};")
    r = Rng(11)
    add(f"inline constexpr std::array<std::uint64_t, count> int64_full_11{{{u64_list(uniform_int(r, -(1 << 63), (1 << 63) - 1, 64, True) & M64 for _ in range(count))}}};")

    r = Rng(13)
    add(f"inline constexpr std::array<std::uint64_t, count> uniform01_13{{{u64_list(bits(uniform01(r)) for _ in range(count))}}};")
    r = Rng(13)
    add(f"inline constexpr std::array<std::uint64_t, count> uniform_real_13{{{u64_list(bits(uniform_real(r, -3.5, 10.25)) for _ in range(count))}}};")
    r = Rng(13)
    add(f"inline constexpr std::array<bool, count> bernoulli_13{{{', '.join('true' if bernoulli(r, 0.3) else 'false' for _ in range(count))}}};")

    r = Rng(17)
    add(f"inline constexpr std::array<std::uint64_t, count> normal_17{{{u64_list(bits(normal(r, 0.0, 1.0)) for _ in range(count))}}};")
    r = Rng(17)
    add(f"inline constexpr std::array<std::uint64_t, count> normal_height_17{{{u64_list(bits(normal(r, 170.0, 7.5)) for _ in range(count))}}};")
    r = Rng(17)
    add(f"inline constexpr std::array<std::uint64_t, count> lognormal_17{{{u64_list(bits(lognormal(r, 3.3, 0.2)) for _ in range(count))}}};")

    weights = [0.5, 0.0, 2.25, 1.0, 0.25]
    d, r = Discrete(weights), Rng(19)
    add(f"inline constexpr std::array<std::size_t, count> discrete_19{{{int_list(d(r) for _ in range(count))}}};")

    r = Rng(23)
    add(f"inline constexpr std::array<int, 10> shuffle_23{{{int_list(shuffle(r, list(range(10))))}}};")
    r = Rng(23)
    add(f"inline constexpr std::array<std::size_t, 5> sample_23{{{int_list(sample_indices(r, 20, 5))}}};")

    # exp and log on fixed inputs, including the reduction boundaries.
    exp_in = [0.0, -0.0, 1.0, -1.0, 0.5, 1e-10, -1e-10, 0.34657359027997264,
              10.0, -10.0, 100.0, -100.0, 700.0, 709.78, -708.0, -745.0, 3.3]
    log_in = [1.0, 2.0, 0.5, 10.0, 0.1, 1e-300, 1e300, 5e-324, 0.7071067811865476,
              1.4142135623730951, 1.0000000001, 0.9999999999, 123456.789, 3.0,
              0.30000000000000004, 1.7976931348623157e308, 2.2250738585072014e-308]
    add(f"inline constexpr std::array<std::uint64_t, {len(exp_in)}> exp_in{{{u64_list(bits(x) for x in exp_in)}}};")
    add(f"inline constexpr std::array<std::uint64_t, {len(exp_in)}> exp_out{{{u64_list(bits(det_exp(x)) for x in exp_in)}}};")
    add(f"inline constexpr std::array<std::uint64_t, {len(log_in)}> log_in{{{u64_list(bits(x) for x in log_in)}}};")
    add(f"inline constexpr std::array<std::uint64_t, {len(log_in)}> log_out{{{u64_list(bits(det_log(x)) for x in log_in)}}};")

    # Hash of many normal draws: catches a difference anywhere in a long run.
    r, h = Rng(29), 0xCBF29CE484222325
    for _ in range(100000):
        for byte in struct.pack("<d", normal(r, 0.0, 1.0)):
            h = ((h ^ byte) * 0x100000001B3) & M64
    add(f"inline constexpr std::uint64_t normal_29_hash_100k = 0x{h:016x}ULL;")

    add("} // namespace kat")
    add("#endif // DASMIG_CORE_KAT_VECTORS_HPP")
    return "\n".join(out) + "\n"


def main():
    check_published()
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent / "tests" / "kat_vectors.hpp"
    target.write_text(emit(), encoding="utf-8", newline="\n")
    print(f"published vectors match; wrote {target}")


if __name__ == "__main__":
    main()
