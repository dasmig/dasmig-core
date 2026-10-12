# Usage Guide

This guide covers every part of dasmig-core and specifies its algorithms exactly. For a quick overview, see the [README](../README.md). For the API reference, run `doxygen Doxyfile` from the repository root and open `doc/api/html/index.html`.

[TOC]

<!-- GitHub-rendered TOC (Doxygen uses [TOC] above instead) -->
<ul>
<li><a href="#the-output-contract">The Output Contract</a></li>
<li><a href="#vendoring-and-versions">Vendoring and Versions</a></li>
<li><a href="#the-engine">The Engine</a></li>
<li><a href="#child-streams">Child Streams</a></li>
<li><a href="#distributions">Distributions</a></li>
<li><a href="#exp-and-log">exp and log</a></li>
<li><a href="#floating-point-requirements">Floating-Point Requirements</a></li>
<li><a href="#using-the-core-in-a-generator">Using the Core in a Generator</a></li>
<li><a href="#testing">Testing</a></li>
<li><a href="#error-reference">Error Reference</a></li>
</ul>

## The Output Contract

Everything below that turns a seed into values is part of the contract: for a given core major version, a seed gives the same bits on every supported compiler, standard library and platform. Changing any algorithm, constant or the number of engine values a function consumes is a major version change.

## Vendoring and Versions

Each library copies `dasmig/core.hpp` and `dasmig/core/` next to its own header and checks the version in use:

```cpp
#include "core.hpp"
DASMIG_CORE_REQUIRE(0, 1);   // major must equal 0, minor must be >= 1
```

When two libraries are included in one translation unit, the include guards keep only the first copy of the core. `DASMIG_CORE_REQUIRE` turns a silent mismatch into a compile error.

The `dasmig::dataset` enum (`lite`, `full`) lives in the core. The core also defines `DASMIG_DATASET_DEFINED`, so library headers released before the core skip their own copy.

## The Engine

`dasmig::core::rng` is xoshiro256\*\* (Blackman and Vigna, 2018) with 256 bits of state.

- **Seeding.** `rng{seed}` and `seed(s)` fill the four state words with four consecutive outputs of SplitMix64 started at `s`. Every 64-bit seed is valid.
- **Output.** `result = rotl(s1 * 5, 7) * 9`, then the standard xoshiro256 state update.
- **`seed()`** returns the seed the engine was last seeded with; constructing an engine from it replays the sequence.
- **`unseed()`** and `rng::from_entropy()` take a seed from `std::random_device`. Use them only for the instance engine behind non-seeded calls; anything that must replay takes a seed.

`rng` satisfies `std::uniform_random_bit_generator`, but use the core's distributions with it: the `std::` ones differ between standard libraries.

## Child Streams

`split(label)` and `split(index)` derive a child engine from the parent's **seed** (not its current state), so the stream a part of a generator receives does not change when another part draws more or fewer values.

| Call | Child seed |
| --- | --- |
| `split(label)` | `mix(seed ^ mix(fnv1a64(label)))` |
| `split(index)` | `mix(seed ^ mix(index ^ 0x6a09e667f3bcc909))` |

`mix` is the SplitMix64 output function. `fnv1a64` hashes the label's bytes: pass UTF-8, because a `std::wstring` has different bytes on Windows (UTF-16) and elsewhere (UTF-32). `child_seed(...)` returns the seed without building the engine, for storing in a result's replay signature.

```cpp
const core::rng person{seed};
auto body = person.split("body");
auto mind = person.split("mind");
auto sibling = person.split(2);
```

## Distributions

Each function takes the engine by reference. Unless noted, it consumes exactly the number of engine values given.

| Function | Algorithm | Values consumed |
| --- | --- | --- |
| `bounded(r, n)` | Lemire 2019: `x = r()`; if `low64(x * n) < n`, redraw while `low64(x * n) < (2^64 - n) mod n`; return `high64(x * n)` | 1, rarely more |
| `uniform_int(r, lo, hi)` | `lo + bounded(r, hi - lo + 1)` in 64-bit unsigned arithmetic; the full 64-bit range returns `r()` | 1, rarely more |
| `uniform01(r)` | `(r() >> 11) * 2^-53`, in [0, 1) | 1 |
| `uniform_real(r, lo, hi)` | `lo + (hi - lo) * uniform01(r)`; if that rounds to `hi`, the largest double below `hi`; `lo` when `lo == hi` | 1 |
| `bernoulli(r, p)` | `uniform01(r) < p` | 1 |
| `normal(r, mean, sd)` | Marsaglia polar: `u = 2 * uniform01(r) - 1`, `v` likewise, until `s = u*u + v*v` is in (0, 1); return `mean + sd * (u * sqrt((-2 * log(s)) / s))`. The variate from `v` is discarded | 2 per attempt (2.55 on average) |
| `lognormal(r, mu, sigma)` | `exp(normal(r, mu, sigma))` | as `normal` |
| `discrete{weights}(r)` | Cumulative sums added left to right; the first index whose sum exceeds `uniform01(r) * total`; zero weights are never chosen | 1 |
| `shuffle(r, range)` | Fisher-Yates from the end: for i = n-1 down to 1, swap i with `bounded(r, i + 1)` | n - 1, rarely more |
| `sample_indices(r, n, k)` | Partial Fisher-Yates from the front over 0..n-1: for i = 0 to k-1, swap i with `i + bounded(r, n - i)`; the first k | k, rarely more |
| `pick(r, range)` | `range[bounded(r, size)]`; returns a reference | 1, rarely more |

`log` and `exp` above are the core's own (next section).

```cpp
core::rng r{7};
std::size_t eye = core::discrete{0.79, 0.12, 0.09}(r);
std::vector<int> deck{1, 2, 3, 4, 5};
core::shuffle(r, deck);
int& card = core::pick(r, deck);
```

## exp and log

`std::exp` and `std::log` are not required to be correctly rounded, and glibc, the MSVC runtime, Apple's libm and Emscripten's musl differ in the last bit for some inputs. `core::math::exp` and `core::math::log` use only +, -, \*, /, `sqrt`, `floor`, `frexp` and `ldexp`, which IEEE 754 defines exactly, so they give the same bits everywhere.

- **exp(x).** `k = floor(x * log2(e) + 0.5)`, `r = (x - k * ln2_hi) - k * ln2_lo`, a degree-13 Taylor polynomial in `r` by Horner's rule, scaled by `2^k`. Measured within 1 ulp of the Windows C runtime; the tests allow 2 against any platform libm.
- **log(x).** `x = m * 2^e` with m in [sqrt(1/2), sqrt(2)), `s = (m - 1) / (m + 1)`, `log(m) = s * (2 + 2/3 s^2 + ... + 2/19 s^18)` by Horner's rule in `s^2`, plus `e * ln2_hi + e * ln2_lo`. Measured within 3 ulp of the Windows C runtime; the tests allow 4.

They are accurate enough for sampling. They are not general replacements for `<cmath>`.

## Floating-Point Requirements

Bit-identical results also need every operation rounded on its own:

- **No fused multiply-add.** Compilers may fuse `a * b + c` into one instruction (GCC in GNU mode, clang with `-ffp-contract=fast`, ARM64 targets). Pragmas do not stop this reliably, so each product that feeds an addition in the core passes through `core::math::fenced()`, an empty optimization barrier. Use it the same way in library code that computes with doubles before a result is stored.
- **No fast-math.** `-ffast-math`, `/fp:fast` and `/fp:contract` are refused at compile time.
- **No x87.** `FLT_EVAL_METHOD` must be 0 (SSE2 on 32-bit x86).
- **Visual Studio 2022 or later**; earlier `/fp:precise` contracted.

The tests pass bit for bit at `-O0`, `-O2` and `-O3 -march=native -ffp-contract=fast` with clang, and with MSVC at `/O2` and `/O2 /arch:AVX2`.

## Using the Core in a Generator

The pattern every library follows:

```cpp
class xg
{
  public:
    // Non-seeded: draw a call seed from the instance engine.
    [[nodiscard]] result get() { return get(_engine()); }

    // Seeded: deterministic and const; does not touch the instance engine.
    [[nodiscard]] result get(std::uint64_t call_seed) const
    {
        const core::rng call{call_seed};
        auto height = call.split("height");
        auto eyes = call.split("eyes");
        // ... draw each field from its own stream ...
    }

    xg& seed(std::uint64_t s) { _engine.seed(s); return *this; }
    xg& unseed() { _engine.unseed(); return *this; }

  private:
    core::rng _engine{core::rng::from_entropy()};
};
```

Splitting per field keeps a result's other fields unchanged when one field's algorithm changes or a field is added.

## Testing

- `tests/kat_vectors.hpp` is generated by `scripts/reference.py` (`make kat`), an independent Python implementation of every algorithm. Python floats are IEEE doubles without fused operations, so agreement is bit for bit. The script first checks its engine against the published SplitMix64 and xoshiro256\*\* vectors.
- The C++ tests compare against those vectors, plus a hash of 100,000 normal draws, and check the statistics of each distribution (moments, chi-square) and the accuracy of `exp` and `log` against the platform libm.

## Error Reference

| Call | Throws | When |
| --- | --- | --- |
| `bounded` | `std::invalid_argument` | `n == 0` |
| `uniform_int` | `std::invalid_argument` | `lo > hi` |
| `uniform_real` | `std::invalid_argument` | a bound is not finite, `lo > hi`, or `hi - lo` overflows |
| `bernoulli` | `std::invalid_argument` | `p` not in [0, 1] or NaN |
| `normal`, `lognormal` | `std::invalid_argument` | mean or sd not finite, or sd negative |
| `discrete` | `std::invalid_argument` | no weights, a weight negative or not finite, sum 0 or not finite |
| `discrete::probability` | `std::out_of_range` | index out of range |
| `sample_indices` | `std::invalid_argument` | `k > n` |
| `pick` | `std::invalid_argument` | empty range |
