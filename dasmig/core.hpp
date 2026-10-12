#ifndef DASMIG_CORE_HPP
#define DASMIG_CORE_HPP

/// @file core.hpp
/// @brief dasmig-core: the shared foundation of the dasmig generator
///        libraries (random engine, distributions, data tiers).
/// @author Diego Dasso Migotto (diegomigotto at hotmail dot com)
/// @see See doc/usage.md for the narrative tutorial.
///
/// Libraries vendor this header and the `core/` directory next to their own
/// header, include it, and check compatibility with DASMIG_CORE_REQUIRE.

#include "core/version.hpp"

#include "core/dataset.hpp"
#include "core/distributions.hpp"
#include "core/math.hpp"
#include "core/rng.hpp"

#endif // DASMIG_CORE_HPP
