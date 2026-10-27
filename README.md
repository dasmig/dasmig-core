# dasmig-core

> **Requires C++23** (e.g., `-std=c++23` for GCC/Clang, `/std:c++latest` for MSVC).

[![GitHub license](https://img.shields.io/badge/license-MIT-blue.svg)](https://raw.githubusercontent.com/dasmig/dasmig-core/main/LICENSE.MIT)
[![CI](https://github.com/dasmig/dasmig-core/actions/workflows/ci.yml/badge.svg)](https://github.com/dasmig/dasmig-core/actions/workflows/ci.yml)
[![C++23](https://img.shields.io/badge/standard-C%2B%2B23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Header-only](https://img.shields.io/badge/type-header--only-green.svg)](https://github.com/dasmig/dasmig-core#integration)

**[Usage Guide](doc/usage.md)**

The shared foundation of the dasmig generator libraries ([address](https://github.com/dasmig/address-generator), [biodata](https://github.com/dasmig/biodata-generator), [birth](https://github.com/dasmig/birth-generator), [city](https://github.com/dasmig/city-generator), [country](https://github.com/dasmig/country-generator), [device](https://github.com/dasmig/device-generator), [email](https://github.com/dasmig/email-generator), [entity](https://github.com/dasmig/entity-generator), [interest](https://github.com/dasmig/interest-generator), [name](https://github.com/dasmig/name-generator), [nickname](https://github.com/dasmig/nickname-generator), [occupation](https://github.com/dasmig/occupation-generator), [persona](https://github.com/dasmig/persona-generator), [phone](https://github.com/dasmig/phone-generator), [relationship](https://github.com/dasmig/relationship-generator), [trait](https://github.com/dasmig/trait-generator), [user](https://github.com/dasmig/user-generator)). Each library vendors a copy.

## Features

- **The same output everywhere.** A seed gives bit-identical results on GCC, Clang and MSVC, with libstdc++, libc++ or the MSVC STL, on x86-64, ARM64 and WebAssembly. The `std::` distributions cannot promise this: each standard library implements them differently.

- **xoshiro256\*\* engine** seeded through SplitMix64, with `seed()`, `unseed()` and `split(label)`. A split stream depends only on the parent seed and the label, so adding a draw in one part of a generator does not shift the others.

- **Distributions with fixed algorithms.** Uniform integer (Lemire), uniform real, Bernoulli, normal (Marsaglia polar), log-normal, weighted choice, shuffle (Fisher-Yates), sampling without replacement and pick.

- **Deterministic `exp` and `log`** built only from operations IEEE 754 defines exactly, because platform `std::exp` and `std::log` differ in the last bit.

- **Data tables with extension layers.** A TSV reader with typed, exactly parsed cells and located errors (`file:line: column: problem`), and layered tables: users add, override or replace rows with their own files, validated atomically, with a data hash for replay.

- **Data on disk or compiled in.** Finds a library's `resources/` from the usual working directories, or reads it from a generated header (`scripts/embed.py`) so a WebAssembly build needs no filesystem; both give the same data hash.

- **Weighted tables, JSON and seed signatures.** Weighted choice with per-call conditional weights and exclusions; a dependency-free JSON writer with platform-independent number formatting; a `seed-data` replay format.

- **Batch generation.** `item_seed`, `run_batch` and `for_each_batch` generate many results from one seed, in parallel if asked, with the same output for any thread count, optional uniqueness by a key, and JSON Lines and CSV writers for the results.

- **Checked against an independent implementation.** `scripts/reference.py` implements every algorithm again in Python and generates the known-answer vectors the C++ tests compare against, bit for bit. The engine is checked against published test vectors.

## Integration

Copy [`dasmig/core.hpp`](dasmig/core.hpp) and the [`dasmig/core/`](dasmig/core) directory next to your library header, then:

```cpp
#include "core.hpp"

// Fails to compile if another library in the same translation unit vendors
// an incompatible core.
DASMIG_CORE_REQUIRE(1, 0);
```

## Usage

```cpp
#include <dasmig/core.hpp>
namespace core = dasmig::core;

core::rng r{42};                                  // same sequence everywhere
auto body = r.split("body");                      // independent child stream

double height = core::normal(body, 170.0, 7.5);   // Marsaglia polar
double weight = core::lognormal(body, 4.25, 0.18);
int    mood   = core::uniform_int(r, 1, 10);      // inclusive
bool   left   = core::bernoulli(r, 0.1);

core::discrete eyes{0.79, 0.12, 0.09};            // weighted index
std::size_t eye = eyes(r);
```

See the [usage guide](doc/usage.md) for every function and the exact algorithms.

## Building

```sh
make test       # build and run the tests
make lint       # clang-tidy
make docs       # Doxygen
make kat        # regenerate tests/kat_vectors.hpp from scripts/reference.py
make embed-fixtures  # regenerate the resource fixtures and their embedded header
```

## License

MIT. See [LICENSE.MIT](LICENSE.MIT).
