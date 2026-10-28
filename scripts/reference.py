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


def c_round(x):
    """std::round: halves away from zero (Python's round() goes to even)."""
    f = math.floor(abs(x))
    if abs(x) - f >= 0.5:
        f += 1.0
    return math.copysign(f, x)


SIN_TERMS = [-1.0 / 1307674368000.0, 1.0 / 6227020800.0, -1.0 / 39916800.0, 1.0 / 362880.0,
             -1.0 / 5040.0, 1.0 / 120.0, -1.0 / 6.0, 1.0]
COS_TERMS = [-1.0 / 87178291200.0, 1.0 / 479001600.0, -1.0 / 3628800.0, 1.0 / 40320.0,
             -1.0 / 720.0, 1.0 / 24.0, -1.0 / 2.0, 1.0]


def det_sin_cos_deg(degrees):
    if not math.isfinite(degrees):
        return math.nan, math.nan
    q = c_round(degrees / 90.0)
    x = (degrees - (90.0 * q)) * (math.pi / 180.0)
    x2 = x * x
    s = 1.0 / 355687428096000.0
    c = 1.0 / 20922789888000.0
    for t in SIN_TERMS:
        s = (s * x2) + t
    for t in COS_TERMS:
        c = (c * x2) + t
    s *= x
    m = math.fmod(q, 4.0)
    quadrant = int(m + 4.0 if m < 0.0 else m)
    return {1: (c, -s), 2: (-s, -c), 3: (-c, s)}.get(quadrant, (s, c))


def det_atan(x):
    negative = x < 0.0
    if negative:
        x = -x
    inverted = x > 1.0
    if inverted:
        x = 1.0 / x
    base = 0.0
    if x > 0.41421356237309503:
        x = (x - 1.0) / (x + 1.0)
        base = math.pi / 4.0
    x2 = x * x
    total = 0.0
    for n in range(25, -1, -1):
        total = (total * x2) + ((1.0 if n % 2 == 0 else -1.0) / ((2.0 * n) + 1.0))
    r = base + (total * x)
    if inverted:
        r = (math.pi / 2.0) - r
    return -r if negative else r


def det_atan2_deg(y, x):
    deg = 180.0 / math.pi
    if math.isnan(x) or math.isnan(y):
        return math.nan
    if x > 0.0:
        return det_atan(y / x) * deg
    if x < 0.0:
        return (det_atan(y / x) * deg) + (180.0 if y >= 0.0 else -180.0)
    if y == 0.0:
        return 0.0
    return 90.0 if y > 0.0 else -90.0


def det_asin_deg(x):
    if math.isnan(x):
        return x
    return det_atan2_deg(x, math.sqrt(max(0.0, 1.0 - (x * x))))


def det_acos_deg(x):
    if math.isnan(x):
        return x
    return det_atan2_deg(math.sqrt(max(0.0, 1.0 - (x * x))), x)


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


def gamma(r, shape, scale):
    boost = 1.0
    if shape < 1.0:
        u = 1.0 - uniform01(r)
        boost = det_exp(det_log(u) / shape)
        shape += 1.0
    d = shape - (1.0 / 3.0)
    c = 1.0 / math.sqrt(9.0 * d)
    while True:
        x = normal(r, 0.0, 1.0)
        t = 1.0 + (c * x)
        if t <= 0.0:
            continue
        v = t * t * t
        u = uniform01(r)
        if u <= 0.0:
            continue
        bound = ((((0.5 * x) * x) + d) - (d * v)) + (d * det_log(v))
        if det_log(u) < bound:
            return ((d * v) * scale) * boost


def beta(r, a, b):
    x = gamma(r, a, 1.0)
    y = gamma(r, b, 1.0)
    return x / (x + y)


def weibull(r, shape, scale):
    u = 1.0 - uniform01(r)
    e = -det_log(u)
    return scale * det_exp(det_log(e) / shape) if e > 0.0 else 0.0


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


    # Gamma, Beta and Weibull draws, one stream each, across the shape < 1
    # boost and the shape >= 1 branch.
    gamma_shapes = [0.3, 1.0, 2.5, 9.0]
    rows = []
    for k in gamma_shapes:
        r = Rng(31)
        rows.append("{" + u64_list(bits(gamma(r, k, 2.0)) for _ in range(count)) + "}")
    add(f"inline constexpr std::array<std::uint64_t, {len(gamma_shapes)}> gamma_shapes{{{u64_list(bits(k) for k in gamma_shapes)}}};")
    add(f"inline constexpr std::array<std::array<std::uint64_t, count>, {len(gamma_shapes)}> gamma_31{{{{{', '.join(rows)}}}}};")
    r = Rng(37)
    add(f"inline constexpr std::array<std::uint64_t, count> beta_37{{{u64_list(bits(beta(r, 0.6 * 4.0, 0.4 * 4.0)) for _ in range(count))}}};")
    r = Rng(41)
    add(f"inline constexpr std::array<std::uint64_t, count> weibull_41{{{u64_list(bits(weibull(r, 2.902, 4.25)) for _ in range(count))}}};")
    r, h = Rng(43), 0xCBF29CE484222325
    for i in range(100000):
        for byte in struct.pack("<d", gamma(r, 0.4 + (i % 7) * 0.5, 1.5)):
            h = ((h ^ byte) * 0x100000001B3) & M64
    add(f"inline constexpr std::uint64_t gamma_43_hash_100k = 0x{h:016x}ULL;")

    # Trigonometry on fixed inputs: quadrant boundaries, both signs, the
    # tan(pi/8) and 1 folds of atan, the axes of atan2.
    trig_in = [0.0, -0.0, 1.0, -1.0, 30.0, 44.999999, 45.0, 45.000001, 60.0, 89.5, 90.0, 135.0,
               180.0, -180.0, 225.0, 270.0, 315.0, 360.0, -720.3, 23.44, 90.833, 1e6 + 0.25, 1e-9]
    add(f"inline constexpr std::array<std::uint64_t, {len(trig_in)}> trig_in{{{u64_list(bits(x) for x in trig_in)}}};")
    add(f"inline constexpr std::array<std::uint64_t, {len(trig_in)}> sin_deg_out{{{u64_list(bits(det_sin_cos_deg(x)[0]) for x in trig_in)}}};")
    add(f"inline constexpr std::array<std::uint64_t, {len(trig_in)}> cos_deg_out{{{u64_list(bits(det_sin_cos_deg(x)[1]) for x in trig_in)}}};")
    atan_in = [0.0, 1e-12, 0.1, 0.41421356237309503, 0.41421356237309515, 0.5, 1.0, 1.0000000000000002,
               2.0, 10.0, 1e10, math.inf, -0.3, -1.0, -50.0]
    add(f"inline constexpr std::array<std::uint64_t, {len(atan_in)}> atan_in{{{u64_list(bits(x) for x in atan_in)}}};")
    add(f"inline constexpr std::array<std::uint64_t, {len(atan_in)}> atan_out{{{u64_list(bits(det_atan(x)) for x in atan_in)}}};")
    atan2_in = [(1.0, 1.0), (1.0, -1.0), (-1.0, -1.0), (-1.0, 1.0), (0.0, -2.0), (-0.0, -2.0), (3.0, 0.0),
                (-3.0, 0.0), (0.0, 0.0), (0.5, 0.866), (1e-8, -1.0)]
    add(f"inline constexpr std::array<std::array<std::uint64_t, 2>, {len(atan2_in)}> atan2_in{{{{{', '.join('{' + u64_list([bits(y), bits(x)]) + '}' for y, x in atan2_in)}}}}};")
    add(f"inline constexpr std::array<std::uint64_t, {len(atan2_in)}> atan2_deg_out{{{u64_list(bits(det_atan2_deg(y, x)) for y, x in atan2_in)}}};")
    arc_in = [0.0, 0.5, -0.5, 0.9999, -0.9999, 1.0, -1.0, 0.25881904510252074, -0.0145, 1.5]
    add(f"inline constexpr std::array<std::uint64_t, {len(arc_in)}> arc_in{{{u64_list(bits(x) for x in arc_in)}}};")
    add(f"inline constexpr std::array<std::uint64_t, {len(arc_in)}> asin_deg_out{{{u64_list(bits(det_asin_deg(x)) for x in arc_in)}}};")
    add(f"inline constexpr std::array<std::uint64_t, {len(arc_in)}> acos_deg_out{{{u64_list(bits(det_acos_deg(x)) for x in arc_in)}}};")
    # A hash of sin and cos over many angles: catches a reduction or
    # quadrant difference anywhere in a long sweep.
    h = 0xCBF29CE484222325
    for i in range(100000):
        sc = det_sin_cos_deg(-1000.0 + i * 0.0213)
        for byte in struct.pack("<dd", *sc):
            h = ((h ^ byte) * 0x100000001B3) & M64
    add(f"inline constexpr std::uint64_t sin_cos_hash_100k = 0x{h:016x}ULL;")

    # Hash of many normal draws: catches a difference anywhere in a long run.
    r, h = Rng(29), 0xCBF29CE484222325
    for _ in range(100000):
        for byte in struct.pack("<d", normal(r, 0.0, 1.0)):
            h = ((h ^ byte) * 0x100000001B3) & M64
    add(f"inline constexpr std::uint64_t normal_29_hash_100k = 0x{h:016x}ULL;")

    # Batch seeds: item_seed(batch, i) is the index child seed; attempt 0 of a
    # unique batch is the item seed, attempt k > 0 is item_seed(item_seed, k).
    def item_seed(batch, i):
        return Rng(batch).child_seed_index(i)

    def attempt_seed(batch, i, k):
        base = item_seed(batch, i)
        return base if k == 0 else item_seed(base, k)

    item_indices = [0, 1, 2, 1000, (1 << 32), M64]
    add(f"inline constexpr std::array<std::uint64_t, {len(item_indices)}> item_index{{{u64_list(item_indices)}}};")
    add(f"inline constexpr std::array<std::uint64_t, {len(item_indices)}> item_seed_7{{{u64_list(item_seed(7, i) for i in item_indices)}}};")
    add(f"inline constexpr std::array<std::uint64_t, 4> attempt_seed_7_5{{{u64_list(attempt_seed(7, 5, k) for k in range(4))}}};")
    # The first draw of each of 10,000 items: catches a change in how item
    # seeds are derived or fed to the engine anywhere in a long batch.
    h = 0xCBF29CE484222325
    for i in range(10000):
        for byte in struct.pack("<Q", Rng(item_seed(99, i))()):
            h = ((h ^ byte) * 0x100000001B3) & M64
    add(f"inline constexpr std::uint64_t batch_99_hash_10k = 0x{h:016x}ULL;")

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
