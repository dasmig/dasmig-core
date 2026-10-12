# Usage Guide

This guide covers every part of dasmig-core and specifies its random algorithms exactly. For a quick overview, see the [README](../README.md). For the API reference, run `doxygen Doxyfile` from the repository root and open `doc/api/html/index.html`.

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
<li><a href="#data-files">Data Files</a></li>
<li><a href="#extending-data">Extending Data</a></li>
<li><a href="#finding-and-embedding-data">Finding and Embedding Data</a></li>
<li><a href="#weighted-tables">Weighted Tables</a></li>
<li><a href="#json-output">JSON Output</a></li>
<li><a href="#seed-signatures">Seed Signatures</a></li>
<li><a href="#utf-8">UTF-8</a></li>
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
DASMIG_CORE_REQUIRE(0, 3);   // major must equal 0, minor must be >= 3
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

## Data Files

Every library's `resources/` and every extension layer use one TSV format, read by `core::tsv_document`:

- UTF-8, with or without a byte order mark; LF or CRLF line ends.
- The first line names the columns. For headerless lists (word lists), pass the column names to `parse` or `load`.
- Cells are separated by one tab and are not quoted. A row may have fewer cells than columns (the rest are empty), not more.
- Empty lines and lines starting with `#` are skipped.

```cpp
auto doc = core::tsv_document::load("resources/lite/biodata.tsv");
for (const auto& row : doc.rows())
{
    std::string_view code = row["country_code"];
    double height = row.get<double>("male_height_cm");      // exact parse
    double sd = row.get_or("male_height_sd", 7.0);           // fallback if empty
    auto langs = row.list("languages");                      // split on ';'
}
```

`get<T>` accepts integers, doubles (finite, decimal or scientific, parsed exactly so every platform gets the same bits), `bool` (`1`, `0`, `true`, `false`), `std::string` and `std::string_view`. Errors are `core::tsv_error`, whose message reads `file:line: column 'name': problem`; `source()`, `line()` and `column()` give the parts.

`content_hash()` hashes the parsed columns and cells, not the bytes, so the same data checked out with LF or CRLF line ends, with or without a BOM or comments, hashes the same.

Rows are light views. They stay valid while the document, or any copy of it, is alive.

## Extending Data

`core::layered_table` assembles a table from a base layer and extension layers in the same TSV format, so users can add to the shipped data, change it or replace it without touching it.

```cpp
core::layered_table heights{{.key = {"country_code"},
                             .required = {"male_height_cm", "female_height_cm"},
                             .optional = {"note"}}};
heights.extend(core::tsv_document::load("resources/lite/biodata.tsv"), core::layer_mode::replace);
heights.extend(core::tsv_document::load("mods/heights.tsv"));   // override by default
heights.extend(core::tsv_document::parse(text_from_host, "scenario layer"), core::layer_mode::add);
```

| Mode | Effect |
| --- | --- |
| `add` | New keys only; a key already loaded is an error |
| `override` (default) | A row replaces the loaded row with the same key, in its position; new keys are appended |
| `replace` | The layer replaces every row loaded before it |

- **Keys.** Each table declares its key columns; together they identify a row. A key cell may not be empty, and a key may appear once per layer.
- **Removing rows.** A row with `disabled` set to `1` removes its key (`override` and `replace` modes).
- **Columns.** A layer must have the key columns. Rows that add or replace data also need the required columns, so a layer that only removes rows can be just the key and `disabled`. Columns the table does not know are ignored and listed in the layer's warnings.
- **Order.** Rows keep the order they first appeared in. Weighted choices depend on order, so it is part of the output.
- **Atomic.** A layer is checked completely before it is applied: keys, `disabled` values, add-mode clashes, and the library's own row check passed as the third argument to `extend`. If anything fails, the table is unchanged and `tsv_error` says where.
- **Record.** `layers()` lists each layer's source, mode, row counts (added, replaced, removed) and warnings. `data_hash()` combines the layers' content hashes and modes, in order.

A library typically keeps one layered table per data file, validates rows with the same code that reads them, and rebuilds its typed data from `rows()` after each `extend`.

## Finding and Embedding Data

A library's data lives in `resources/lite/` and `resources/full/`. The core finds it on disk, or reads it from the program itself when it was compiled in.

**On disk.** `core::find_resources(library, marker)` returns the first of these directories (relative to the working directory, or to a `base` you pass) that contains the marker file:

1. `resources`
2. `../resources`
3. `<library>/resources`
4. `../<library>/resources`

These cover running from the repository, from its `build/` directory, and from a parent directory holding several libraries side by side. The marker (for example `lite/biodata.tsv`) keeps one library from picking up another's `resources/`.

**Compiled in.** `scripts/embed.py` turns a tier into a header:

```sh
python3 scripts/embed.py resources --tier lite --name biodatagen \
    --output dasmig/biodatagen_data.hpp
```

The header defines `dasmig::embedded::biodatagen::files`. It is plain ASCII C++: bytes outside printable ASCII are octal escapes, so it compiles the same with or without `/utf-8`. Content is split into chunks below MSVC's string-literal limits, and CRLF is written as LF, so the header is the same from any checkout. City-generator's 3.1 MB `lite` tier becomes a 4 MB header that clang compiles in about 3 s and MSVC in about 7 s.

**Loading.** `core::load_resource(path, embedded, root)` reads the embedded copy when there is one, otherwise `root / path`, otherwise returns empty:

```cpp
#ifdef DASMIG_BIODATA_EMBED
#include "biodatagen_data.hpp"
inline constexpr std::span<const core::embedded_file> embedded_files = dasmig::embedded::biodatagen::files;
#else
inline constexpr std::span<const core::embedded_file> embedded_files{};
#endif

auto root = core::find_resources("biodata-generator", "lite/biodata.tsv");
auto doc = core::load_resource("lite/biodata.tsv", embedded_files, root);
```

The embedded and on-disk copies have the same `content_hash()`, so a WebAssembly build with compiled-in data replays the same seeds as a native build that reads files.

## Weighted Tables

`core::weighted_table<T>` holds values with weights. A pick uses the same algorithm as `discrete` and consumes one engine value.

```cpp
core::weighted_table<std::string> jobs{{"farmer", 3.0}, {"nurse", 2.0}, {"pilot", 0.2}};
const auto& job = jobs.pick(r);

// Context changes the weights for one call: return the base weight, a
// multiple of it, or 0 to exclude the entry.
const auto& teen_job = jobs.pick(r, [&](const std::string& j, double w) {
    return (j == "pilot" && age < 21) ? 0.0 : w;
});

auto hobbies = hobby_table.pick_distinct_indices(r, 3);   // 3 different entries
```

- `try_pick` returns `nullptr` when every adjusted weight is 0; it still consumes one value, so later draws do not shift.
- `pick_distinct_indices(r, k[, adjust])` draws k times without repeating an entry. It consumes exactly k values and returns fewer than k entries when fewer are eligible.
- Weights must be finite and not negative, before and after adjustment.

## JSON Output

`core::json_writer` builds one compact JSON document. It places commas itself and throws `std::logic_error` on misuse (a value without a key in an object, unbalanced ends, a second top-level value).

```cpp
core::json_writer w;
w.begin_object()
    .field("name", "Ana")
    .field("height_cm", 167.5)
    .key("tags").begin_array().value("calm").value("curious").end_array()
 .end_object();
std::string json = w.str();
```

Numbers are written with `std::to_chars`, the shortest text that reads back to the same double, so the JSON is identical on every platform. NaN and infinity are rejected. Strings must be UTF-8 and are escaped as RFC 8259 requires.

JavaScript numbers are doubles, so integers above 2^53 lose precision there. Write 64-bit values such as seeds as strings.

## Seed Signatures

`core::seed_signature` is what a result records so it can be generated again: the call seed and the hash of the data it came from.

```cpp
core::seed_signature sig{.seed = call_seed, .data = table.data_hash()};
sig.to_string();                          // "00000000000004d2-9ae16a3b2f90404f"
auto back = core::seed_signature::parse("00000000000004d2-9ae16a3b2f90404f");
sig.to_json(writer);                      // {"seed":"00000000000004d2","data":"9ae16a3b2f90404f"}
```

A seed reproduces a result only with the same data. When `data` differs from the data loaded now, a library can report the mismatch instead of returning a different result silently. `parse` also accepts a lone 16-digit seed (data 0, meaning "not recorded").

## UTF-8

The core works in UTF-8. Libraries with a `std::wstring` API convert at the boundary with `core::to_utf8` and `core::from_utf8`. They handle UTF-16 `wchar_t` (Windows) and UTF-32 (elsewhere), so the bytes are the same everywhere; that matters for split labels and for hashes. `core::is_utf8` and `core::utf8_invalid_offset` check input.

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
| `weighted_table::add`, `pick`, `try_pick` | `std::invalid_argument` | a weight (base or adjusted) negative or not finite; `pick` when nothing is eligible |
| `tsv_document::parse`, `load` | `core::tsv_error` | unreadable file, invalid UTF-8, no header, an unnamed or repeated column, a row with too many cells |
| `tsv_row::get` | `core::tsv_error` | no such column, empty cell, text that is not a valid value |
| `layered_table::extend` | `core::tsv_error` | see Extending Data above; the table is unchanged |
| `json_writer` | `std::logic_error` / `std::invalid_argument` | misuse / NaN, infinity or invalid UTF-8 |
| `seed_signature::parse` | `std::invalid_argument` | not `<16 hex>` or `<16 hex>-<16 hex>` |
| `from_utf8`, `to_utf8` | `std::invalid_argument` | malformed UTF-8 / unpaired surrogate |
