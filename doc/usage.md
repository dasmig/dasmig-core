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
<li><a href="#exp-log-and-trigonometry">exp, log and trigonometry</a></li>
<li><a href="#floating-point-requirements">Floating-Point Requirements</a></li>
<li><a href="#data-files">Data Files</a></li>
<li><a href="#extending-data">Extending Data</a></li>
<li><a href="#finding-and-embedding-data">Finding and Embedding Data</a></li>
<li><a href="#weighted-tables">Weighted Tables</a></li>
<li><a href="#json-output">JSON Output</a></li>
<li><a href="#seed-signatures">Seed Signatures</a></li>
<li><a href="#utf-8">UTF-8</a></li>
<li><a href="#check-digits">Check Digits</a></li>
<li><a href="#patterns">Patterns</a></li>
<li><a href="#money">Money</a></li>
<li><a href="#time-zones">Time Zones</a></li>
<li><a href="#using-the-core-in-a-generator">Using the Core in a Generator</a></li>
<li><a href="#testing">Testing</a></li>
<li><a href="#error-reference">Error Reference</a></li>
</ul>

## The Output Contract

Everything below that turns a seed into values is part of the contract: for a given core major version, a seed gives the same bits on every supported compiler, standard library and platform. Changing any algorithm, constant or the number of engine values a function consumes is a major version change. That includes `pattern::draw` (see Patterns below).

The check digits, money and time-zone functions draw nothing, but their results are part of the contract too: a library's output depends on them. A wrong result fixed later is a bug fix in a minor version, and the known-answer tests show it.

## Vendoring and Versions

Each library copies `dasmig/core.hpp` and `dasmig/core/` next to its own header and checks the version in use:

```cpp
#include "core.hpp"
DASMIG_CORE_REQUIRE(1, 0);   // major must equal 1, minor must be >= 0
```

When two libraries are included in one translation unit, the include guards keep only the first copy of the core. `DASMIG_CORE_REQUIRE` turns a silent mismatch into a compile error.

The `dasmig::dataset` enum (`lite`, `full`) lives in the core. The core also defines `DASMIG_DATASET_DEFINED`, so library headers released before the core skip their own copy.

## Shared Types

### Sex

`dasmig::sex` (`male`, `female`) lives in the core, so every library that takes or returns a sex uses one type. `core::sex_name(sex)` gives the lower-case name used in data files and JSON (`"male"`, `"female"`), and `core::parse_sex(text)` reads it back, returning an empty optional for anything else. Matching is exact: `"Male"` and `"m"` are rejected. Both are `constexpr`.

Like `dataset`, the enum sits behind a guard (`DASMIG_SEX_DEFINED`), so library headers released before core 1.2 skip their own identical copy when included together.

```cpp
dasmig::sex s = core::parse_sex(row["sex"]).value_or(dasmig::sex::female);
json.field("sex", core::sex_name(s));
```

### Country codes

`core::is_alpha2(text)` is `true` for exactly two capital ASCII letters (`"BR"`, not `"br"` or `"BRA"`). It checks the shape only, not whether the code is assigned or whether a library has data for it. `core::unknown_country_error(code)` returns the `std::invalid_argument` libraries throw for a code they have no data for; its message is `Unknown country code: <code>`.

```cpp
if (!core::is_alpha2(code)) throw row.error("country_code", "expected two capital letters (ISO 3166-1 alpha-2)");
// ...
if (it == index.end()) throw core::unknown_country_error(code);
```

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
| `gamma(r, shape, scale)` | Marsaglia and Tsang 2000: `d = shape - 1/3`, `c = 1 / sqrt(9 d)`; draw `x = normal(r, 0, 1)` until `t = 1 + c x > 0`, then `u = uniform01(r)`; accept when `u > 0` and `log(u) < x^2/2 + d - d v + d log(v)` with `v = t^3`; return `d * v * scale`. A shape below 1 first draws `u0 = 1 - uniform01(r)`, runs with shape + 1 and multiplies by `exp(log(u0) / shape)` | rejection: about 2.6 per attempt plus 1 for a shape below 1 |
| `beta(r, a, b)` | `x / (x + y)`, `x = gamma(r, a, 1)` drawn before `y = gamma(r, b, 1)` | two `gamma` draws |
| `weibull(r, shape, scale)` | `e = -log(1 - uniform01(r))`; `scale * exp(log(e) / shape)`, or 0 when `e` is 0 | 1 |
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

## exp, log and trigonometry

`std::exp` and `std::log` are not required to be correctly rounded, and glibc, the MSVC runtime, Apple's libm and Emscripten's musl differ in the last bit for some inputs. `core::math::exp` and `core::math::log` use only +, -, \*, /, `sqrt`, `floor`, `frexp` and `ldexp`, which IEEE 754 defines exactly, so they give the same bits everywhere.

- **exp(x).** `k = floor(x * log2(e) + 0.5)`, `r = (x - k * ln2_hi) - k * ln2_lo`, a degree-13 Taylor polynomial in `r` by Horner's rule, scaled by `2^k`. Measured within 1 ulp of the Windows C runtime; the tests allow 2 against any platform libm.
- **log(x).** `x = m * 2^e` with m in [sqrt(1/2), sqrt(2)), `s = (m - 1) / (m + 1)`, `log(m) = s * (2 + 2/3 s^2 + ... + 2/19 s^18)` by Horner's rule in `s^2`, plus `e * ln2_hi + e * ln2_lo`. Measured within 3 ulp of the Windows C runtime; the tests allow 4.

Trigonometry, added in 1.3.0 for weather-generator's sun and for great-circle distances, works in degrees so the angle reduction is exact:

- **sin_cos_deg(d)**, **sin_deg**, **cos_deg.** `q = round(d / 90)` (halves away from zero), `x = (d - 90 q) pi / 180` with `|x| <= pi/4`, Taylor series of sin to `x^17` and of cos to `x^16` by Horner's rule in `x^2`, then swapped and negated by `q mod 4`. Multiples of 90 give exact 0 and 1; NaN and infinity give NaN.
- **atan(x)**, in radians. `|x| > 1` folds to `pi/2 - atan(1/x)`, `x > tan(pi/8)` moves to `(x - 1) / (x + 1)` plus `pi/4`, then the odd Taylor series to `x^51` by Horner's rule in `x^2`. Measured within 1 ulp of the Windows C runtime; the tests allow 2.
- **atan2_deg(y, x)**, **asin_deg(x)**, **acos_deg(x)**, in degrees. `atan2_deg` is `atan(y / x)` in degrees, plus or minus 180 for negative `x`, and 90 or -90 on the y axis; `asin_deg(x) = atan2_deg(x, sqrt(1 - x^2))` and `acos_deg(x) = atan2_deg(sqrt(1 - x^2), x)`, with `1 - x^2` floored at 0, so arguments beyond [-1, 1] give the end of the range.

They are accurate enough for sampling and for geometry on the globe. They are not general replacements for `<cmath>`.

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
- **Record.** `layers()` lists each layer's source, mode, row counts (added, replaced, removed) and warnings. `data_hash()` combines the layers' content hashes and modes, in order. A `replace` layer starts the record again, since nothing before it affects the data: reloading the same base gives the same hash.

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

## Batch Generation

A batch is `count` results from one batch seed. Item `i` is exactly what the single-call getter returns for `core::item_seed(batch_seed, i)`, which is `rng{batch_seed}.split(i).seed()`. So an item replays on its own, a range is computed directly (`first = 1000` starts at item 1000), two batches of 1,000 equal one of 2,000, and the result does not depend on how the work is divided.

```cpp
// A library's batch getter is a thin wrapper over this.
auto items = core::run_batch({.count = 1000, .threads = 4}, batch_seed,
    [&](std::uint64_t seed) { return gen.get_x(request, seed); });
```

`core::batch_options` has `count`, `first` (default 0), `threads` (default 1; 0 means the hardware count) and `unique`. The `make` callable runs on several threads, so it must be callable concurrently; the libraries' const seeded getters are. If a call throws, the exception for the lowest failing index is rethrown, whatever the thread count. Without threads (Emscripten without `-pthread`) the batch runs serially with the same result.

`core::for_each_batch(options, batch_seed, make, sink)` calls `sink(index, item)` in index order on the calling thread and keeps only a few thousand items per thread alive, so a batch of millions does not have to fit in memory. `index` includes `first`.

### Uniqueness

```cpp
core::batch_options options{.count = 1000, .unique = core::unique_rule::by<user>(
    [](const user& u) { return u.email(); }, /*max_attempts=*/50)};
```

Items are generated, then checked in index order. An item whose key was already produced earlier in the call is generated again with `core::attempt_seed(batch_seed, i, k)` (attempt 0 is the plain item seed, so items that do not collide equal the single call; attempt `k > 0` is `item_seed(item_seed(batch_seed, i), k)`) until it is new. If `max_attempts` run out, the call throws `std::runtime_error` naming the item, instead of looping. The result is the same for any thread count. Uniqueness holds within one call, not across two concatenated batches. The rule is checked against the item type and rejected with `std::invalid_argument` if it was made for another.

### JSON Lines and CSV

```cpp
core::jsonl_writer lines{std::cout};          // one to_json() per line
core::csv_writer csv{file};                   // header from the first record
core::for_each_batch(options, seed, make, csv);
```

Both are sinks for results that have `to_json()`. `csv_writer` flattens each result's JSON into dotted columns (`birth.date`, `city.name`) with `core::flatten_json`: strings are unescaped, `null` is an empty cell, and an array (or an empty nested object) is one cell holding its JSON text. The header comes from the first record; later records are matched to it by column name (a missing column is empty, an extra one is dropped). Fields with a comma, quote or line break are quoted, and rows end with `\n`.

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

## Check Digits

`check_digits.hpp` has the check-digit schemes identifiers use. Inputs are the electronic form, without spaces, dashes or dots; a library that shows a grouped form (`DE89 3704 0044 0532 0130 00`) groups after computing.

| Function | Algorithm |
| --- | --- |
| `luhn_digit(body)` | ISO/IEC 7812-1: over the digits from the right, with i = 0 for the rightmost, the digit doubled when i is even (minus 9 when the double exceeds 9), summed; `(10 - sum mod 10) mod 10` |
| `mod97_10_digits(body)` | ISO 7064 MOD 97-10: each character's value (digits as themselves, A to Z as 10 to 35) written in decimal and fed into `r = (r * 10 + digit) mod 97`, then two zeros; the two digits `98 - r` |
| `iban_check_digits(country, bban)` | ISO 13616: `mod97_10_digits(bban + country)` |
| `mod11_2_digit(body)` | ISO 7064 MOD 11-2: `p = ((p + digit) * 2) mod 11` from 0; `(12 - p) mod 11`, written `X` for 10 |
| `mod11_10_digit(body)` | ISO 7064 MOD 11,10: from p = 10, `s = (p + digit) mod 10` (10 when 0) and `p = 2 s mod 11`; `(11 - p) mod 10` |
| `weighted_sum(body, weights[, value_of])` | `sum of value_of(body[i]) * weights[i mod size]`, left to right, in 64-bit unsigned arithmetic; `value_of` is `digit_value` by default |
| `gtin_digit(body)` | GS1: weights 3 and 1 from the right, starting with 3; `(10 - sum mod 10) mod 10`. Any length (GTIN-8 to -14, SSCC, ISBN-13) |
| `icao9303_digit(field)` | ICAO Doc 9303: weights 7, 3, 1 from the left, `<` as 0, A to Z as 10 to 35; `sum mod 10` |
| `verhoeff_digit(body)` | Verhoeff (1969): the dihedral group D5's multiplication table, the position permutation and the inverse |

Each `*_digit` function has a `*_valid` partner that checks a value ending in its check digits (`iban_valid` checks a whole IBAN, 15 to 34 characters; the length each country requires is the library's data). `digit_value(c)` and `alnum_value(c)` give a character's value.

- **Errors.** A `*_digit` function throws `std::invalid_argument` for an empty or malformed body, naming the function and the 0-based position: `luhn_digit: position 3: '-' is not a digit`. A `*_valid` function never throws; malformed input is not valid.
- **Compile time.** Every function is `constexpr` and none allocates, except to build an error message.
- **What each catches.** The tests check each claim over every single substitution and adjacent transposition of 10,000 values: all schemes catch every single substitution of a digit; MOD 97-10, MOD 11-2 and Verhoeff catch every adjacent transposition; Luhn all but `09` and `90`; GTIN all but pairs that differ by 5; MOD 11,10 about 98%; ICAO 9303 claims none.

The national mod-11 rules each reduce a weighted sum in their own way, so they stay in the libraries, built on `weighted_sum`:

```cpp
// Brazil's CPF: weights 10 to 2, then 11 to 2; a digit is 0 when the remainder is below 2.
std::string cpf = body_pattern.draw(r);                       // "111444777"
for (const unsigned top : {10U, 11U})
{
    std::array<unsigned, 10> w{};
    for (unsigned i = 0; i < cpf.size(); ++i) w[i] = top - i;
    const auto rem = core::weighted_sum(cpf, std::span{w}.first(cpf.size())) % 11;
    cpf += static_cast<char>('0' + (rem < 2 ? 0 : 11 - rem));
}                                                             // "11144477735"
```

## Patterns

`core::pattern` is a small language for identifier shapes. Letters are literals and the tokens are symbols, so a pattern reads like the value it makes:

| Syntax | Meaning | Strings |
| --- | --- | --- |
| `#` | a digit, `0`-`9` | 10 |
| `@` | a capital letter, `A`-`Z` | 26 |
| `*` | a digit or capital letter, `0`-`9` then `A`-`Z` | 36 |
| `[...]` | one character of a class, in the order written; `A-Z` ranges expand upwards; `-` is literal first or last | as written |
| `(a\|b\|c)` | one of the alternatives, each a sequence; groups nest | the sum of the alternatives |
| `{n}`, `{n,m}` | the item before it, exactly n, or n to m times (n may be 0, so `{0,1}` is optional) | the sum over the counts |
| `\x` | the character x as a literal (`\#`, `\{`, `\\`) | 1 |
| anything else | itself, as a literal (`-`, space, `AB`) | 1 |

```cpp
static const auto nino = core::pattern::parse("[A-CEGHJ-PR-TW-Z][A-CEGHJ-NPR-TW-Z]######[A-D]");
std::string id = nino.draw(r);       // "JG103759A"
bool ok = nino.matches(id);          // true
std::uint64_t space = nino.count();  // 20 * 19 * 10^6 * 4
```

**Parsing.** `pattern::parse` throws `std::invalid_argument` with the position (`pattern::parse: position 1: reversed range 'Z'-'A'`) for: a byte outside printable ASCII; a `|` outside a group; an empty alternative; an unclosed `(`, `[` or `{`; a stray `)`, `]` or `}`; a dangling `\`; an empty class, a reversed range or a repeated character in a class; a quantifier after nothing or after another quantifier; a count above 64, a maximum below the minimum, or a maximum of 0. The limits are 1,024 characters of text, groups nested 8 deep and strings of 256 characters. Parse a library's patterns when its tables load, so a bad one is a load error with its row. A parsed pattern is immutable, copyable and safe to share between threads; two patterns are equal when their text is.

**Drawing** is part of the output contract. Left to right:

1. A literal appends itself and draws nothing.
2. A token or class appends `chars[bounded(r, chars.size())]`, its characters in the order of the table above.
3. A group draws `k = bounded(r, alternatives)`, then alternative k.
4. `{n}` draws its item n times; `{n,m}` with n < m first draws `c = uniform_int(r, n, m)`, then its item c times.

So a fixed-length pattern of k variable characters consumes k engine values (rarely more). Alternatives are equally likely whatever their size; a library that wants weights uses its weighted table with one pattern per row.

**Matching.** `matches(s)` is true when the whole of `s` is a string the pattern can make. It computes the set of positions each item can end at, memoised per group, so the time is polynomial in the length of `s` whatever the pattern and input. It does not check check digits: call the check-digit function after.

**Counting.** `count()` is the number of strings, counting alternatives as disjoint and different repetition counts as different strings, so it is an upper bound when they overlap (`(AB|A)(BC|C)` counts `ABC` twice). It saturates at 2^64 - 1. Use it to refuse a unique batch larger than the space. `lengths()` gives the shortest and longest lengths.

## Money

`core::money` is an amount in integer minor units with its ISO 4217 code and minor unit. The core knows no currencies: the minor unit (2 for BRL, 0 for JPY, 3 for KWD) comes from the caller, typically country-generator's `currency_digits`.

```cpp
auto income = core::money::from_major(core::lognormal(r, 10.4, 0.6), "BRL", 2);
auto price = core::money::from_minor(129990, "BRL", 2);     // exact: 1299.90
price.decimal();                                            // "1299.90"
w.key("income");
income.write_json(w);                                       // {"amount":32861.07,"currency":"BRL"}
```

- **Invariants.** `from_minor` and `from_major` throw `std::invalid_argument` for a code that is not three capital letters, digits above 4, or more than 2^53 minor units either way; `from_major` also for NaN and infinity.
- **Rounding.** `from_major(a, ...)` computes `y = |a| * 10^digits` (one correctly rounded multiplication by an exact power of ten), takes `f = floor(y)`, adds 1 when `y - f >= 0.5`, and puts the sign back: halves away from zero, and never `floor(y + 0.5)`, which rounds 0.49999999999999994 up. 0.125 at two digits is 13 cents; 1.005 is 100, because the double 1.005 is just below 1.005. Draw minor units with `from_minor` when an exact decimal matters; amounts drawn from a distribution (incomes, prices) are not decimal to begin with, and `from_major` is right for them.
- **Text.** `decimal()` is integer arithmetic: `"1234.50"`, `"-0.05"`, `"1000"`, `"0.125"`. No grouping and no locale; country-generator's `format_money` does that.
- **JSON.** `write_json` writes `{"amount":1234.5,"currency":"BRL"}`, the amount as `json_writer` writes a double. Every other quantity in the libraries' JSON is a number, so amounts sort, sum and chart as such; the CSV writer flattens an amount under `income` into `income.amount` and `income.currency`. `major()` is `minor / 10^digits`, the closest double to the amount; up to 2^51 minor units, multiplying the JSON number by 10^digits and rounding gives the minor units back exactly.
- **No arithmetic** beyond equality, and no conversion. An order's total is a sum of `minor` values of one currency, written by the library.

## Time Zones

`core::tz_rules` holds a zone's rules as text the caller gives it: a list of periods, then a POSIX TZ rule (the footer of a TZif file, RFC 8536) for the time after the last period. It does not use `std::chrono::tzdb`, which reads the operating system's database: that differs by platform and by update level and is missing from Emscripten and older libc++, so the same seed would give different local times on different machines. Country-generator ships the zone data; a library takes a `core::tz_rules` as a plain value.

```cpp
auto berlin = core::tz_rules::parse("946684800/3600/CET/s 954032400/7200/CEST/d 972781200/3600/CET/s",
                                    "CET-1CEST,M3.5.0,M10.5.0/3");
auto info = berlin.at(t);                     // offset (east of UTC), dst, abbreviation
auto local = berlin.to_local(t);              // std::chrono::local_seconds
auto back = berlin.to_utc(local);             // std::chrono::sys_seconds
auto fixed = core::tz_rules::fixed(3h, "+03");
```

**Periods** are `utc-seconds/offset-seconds/abbreviation/s|d` separated by single spaces: the start in seconds since 1970-01-01T00:00:00Z, the offset east of UTC (within ±86,399), an abbreviation of 3 to 6 of `A-Z a-z 0-9 + -`, and `s` or `d` for standard or daylight time. Starts strictly increase; the first is `valid_from()`. `periods_text()` and `posix()` give the text back, and parse to an equal zone.

**POSIX rules** are `std offset [dst [offset] [,start[/time],end[/time]]]`:

- Names are 3 to 6 letters, or `<...>` around 3 to 6 of `A-Z a-z 0-9 + -` (`<-03>`).
- Offsets are `[+|-]hh[:mm[:ss]]` west of UTC (`EST5` is UTC-5), hours up to 24. A daylight name without an offset is one hour ahead of standard. A daylight name needs the two rules.
- Dates are `Jn` (1 to 365, February 29 never counted, so `J60` is always March 1), `n` (0 to 365, counting it) or `Mm.w.d` (weekday d, 0 for Sunday, of week w, 5 for the last, of month m).
- Times are local, standard time for the start and daylight time for the end, from -167 to 167 hours (RFC 8536 3.3.1); 02:00 by default. So Jerusalem's `M3.4.4/26` (Friday 02:00) and Nuuk's `M3.5.0/-1` work, and so does daylight time all year (`EST5EDT,0/0,J365/25`).
- An empty rule means the last period holds forever.

**`at(t)`** is the period that contains `t`, found by binary search, while `t` is before the last period's start or the rule has no daylight time. After it, it is the latest change of the rule in the UTC years around `t` that is after the last period's start and not after `t` (daylight time wins two changes at one instant), or the last period when there is none. A change of the rule never overrides the list.

**`to_utc(l, choose)`.** With o1 and o2 the offsets in force one day before and after `l` read as UTC, the candidates are `l - o1` and `l - o2`; a candidate is valid when the offset at it is the one that made it. Two valid candidates mean a repeated local time (clocks going back), resolved by `tz_choose::earliest` (the default) or `latest`. None means a skipped local time, and the answer is `l - o1`: 02:30 on a spring-forward night in Berlin is 01:30Z, which is 03:30 CEST. This assumes a zone changes its offset at most once in two days, which tzdata respects since 1970.

**The rest.** `transitions(from, to)` lists the changes in `[from, to)`: every listed period after the first, and every change of the rule that changes the offset or the daylight flag. `observes_dst_after(t)` is whether daylight time is in force at `t` or begins after it. A default-constructed zone is UTC. A `tz_info`'s abbreviation is a view into the zone, valid while the zone lives.

**Range.** Instants from `valid_from()` to the end of the year 9999 are supported; `at`, `to_local` and `to_utc` throw `std::out_of_range` outside it, rather than guess: `tz_rules::at: 1969-12-31T23:59:59Z is before the zone's data, which starts at 1970-01-01T00:00:00Z`. A zone is immutable and nothing is cached, so it is safe to share between threads.

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
- Item seeds and a hash of the first draws of 10,000 items are in the vectors too. A chi-square and lag-1 test over a million consecutive item indices checks that neighbouring items are uncorrelated, which item seeds rely on. The CI runs the batch tests under ThreadSanitizer.
- The C++ tests compare against those vectors, plus hashes of 100,000 normal draws, 100,000 gamma draws and 100,000 sines and cosines, and check the statistics of each distribution (moments, chi-square) and the accuracy of `exp`, `log`, `sin_cos_deg` and `atan` against the platform libm.
- `scripts/reference.py` also parses and draws patterns, so the pattern draws in the vectors (five patterns, three seeds, and a hash of 1,000 draws) pin `pattern::draw` independently.
- Check digits are tested against published values (IBANs, the ICAO 9303 specimen passport, ORCID, a Chinese resident ID) and against their claims, over every single substitution and adjacent transposition of 10,000 values per scheme. Patterns are tested against brute-force enumeration of their strings, and time zones at both sides of every transition of hand-written rules; country-generator compares its zone data with Python's `zoneinfo` at every transition.

## Error Reference

| Call | Throws | When |
| --- | --- | --- |
| `bounded` | `std::invalid_argument` | `n == 0` |
| `uniform_int` | `std::invalid_argument` | `lo > hi` |
| `uniform_real` | `std::invalid_argument` | a bound is not finite, `lo > hi`, or `hi - lo` overflows |
| `bernoulli` | `std::invalid_argument` | `p` not in [0, 1] or NaN |
| `normal`, `lognormal` | `std::invalid_argument` | mean or sd not finite, or sd negative |
| `gamma`, `weibull` | `std::invalid_argument` | shape not finite and positive, or scale not finite or negative |
| `beta` | `std::invalid_argument` | a shape not finite and positive |
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
| `unknown_country_error` | returns `std::invalid_argument` | the caller throws it for a country code with no data |
| `*_digit`, `iban_check_digits`, `digit_value`, `alnum_value` | `std::invalid_argument` | an empty body, or a character the scheme does not accept (with its position); a country that is not two capital letters |
| `weighted_sum` | `std::invalid_argument` | no weights, or `value_of` throws it for a character (with its position) |
| `*_valid`, `pattern::matches` | never | malformed input is not valid |
| `pattern::parse` | `std::invalid_argument` | see Patterns above; the message has the position |
| `money::from_minor`, `from_major` | `std::invalid_argument` | a code that is not three capital letters, digits above 4, more than 2^53 minor units, or (`from_major`) NaN or infinity |
| `tz_rules::parse` | `std::invalid_argument` | malformed periods or rule (with the position), no period, starts that do not increase, a start outside the years -9999 to 9999, an offset beyond ±86,399 s |
| `tz_rules::fixed` | `std::invalid_argument` | an offset beyond ±86,399 s, or a bad abbreviation |
| `tz_rules::at`, `to_local`, `to_utc` | `std::out_of_range` | an instant before `valid_from()` or after the year 9999 |
