#include "catch_amalgamated.hpp"

#include "../dasmig/core.hpp"
#include "kat_vectors.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <vector>

DASMIG_CORE_REQUIRE(0, 3);

using dasmig::core::rng;
namespace core = dasmig::core;

namespace
{

double from_bits(std::uint64_t b) { return std::bit_cast<double>(b); }
std::uint64_t to_bits(double d) { return std::bit_cast<std::uint64_t>(d); }

// Distance in units in the last place between two finite doubles of the
// same sign.
double ulp_distance(double a, double b)
{
    if (a == b) return 0.0;
    return std::abs(a - b) / std::abs(std::nextafter(b, 2 * b) - b);
}

// Pearson chi-square statistic of observed counts against equal expectation.
double chi_square_uniform(const std::vector<std::size_t>& counts)
{
    const double total = std::accumulate(counts.begin(), counts.end(), 0.0);
    const double expected = total / static_cast<double>(counts.size());
    double chi = 0.0;
    for (const auto c : counts)
    {
        const double d = static_cast<double>(c) - expected;
        chi += d * d / expected;
    }
    return chi;
}

} // namespace

// ---------------------------------------------------------------------------
// Version and dataset
// ---------------------------------------------------------------------------

TEST_CASE("version constant matches the macros", "[version]")
{
    STATIC_REQUIRE(core::version.major == DASMIG_CORE_VERSION_MAJOR);
    STATIC_REQUIRE(core::version.minor == DASMIG_CORE_VERSION_MINOR);
    STATIC_REQUIRE(core::version.patch == DASMIG_CORE_VERSION_PATCH);
}

TEST_CASE("dataset enum is defined once behind the legacy guard", "[dataset]")
{
#ifndef DASMIG_DATASET_DEFINED
    FAIL("DASMIG_DATASET_DEFINED not set: old library headers would redefine dataset");
#endif
    STATIC_REQUIRE(dasmig::dataset::lite != dasmig::dataset::full);
}

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------

TEST_CASE("splitmix64 matches the published reference values", "[rng][kat]")
{
    core::splitmix64 sm{1234567};
    REQUIRE(sm() == 6457827717110365317ULL);
    REQUIRE(sm() == 3203168211198807973ULL);
    REQUIRE(sm() == 9817491932198370423ULL);
    REQUIRE(sm() == 4593380528125082431ULL);
    REQUIRE(sm() == 16408922859458223821ULL);
}

TEST_CASE("fnv1a64 matches the published offset and a known value", "[rng][kat]")
{
    STATIC_REQUIRE(core::fnv1a64("") == 0xcbf29ce484222325ULL);
    STATIC_REQUIRE(core::fnv1a64("a") == 0xaf63dc4c8601ec8cULL);
}

TEST_CASE("engine output matches the reference implementation", "[rng][kat]")
{
    auto check = [](std::uint64_t seed, const auto& expected) {
        rng r{seed};
        for (const auto value : expected)
        {
            REQUIRE(r() == value);
        }
    };
    check(0, kat::engine_seed_0);
    check(42, kat::engine_seed_42);
    check(std::numeric_limits<std::uint64_t>::max(), kat::engine_seed_max);
}

TEST_CASE("engine works at compile time", "[rng]")
{
    constexpr auto first = [] {
        rng r{42};
        return r();
    }();
    STATIC_REQUIRE(first == kat::engine_seed_42[0]);
}

TEST_CASE("seed() reports and resets the sequence", "[rng]")
{
    rng r{42};
    REQUIRE(r.seed() == 42);
    const auto a = r();
    const auto b = r();
    r.seed(42);
    REQUIRE(r() == a);
    REQUIRE(r() == b);
    REQUIRE(rng{r.seed()} != r);
    REQUIRE(rng{42} == rng{42});
}

TEST_CASE("unseed() changes the seed", "[rng]")
{
    rng r{42};
    r.unseed();
    rng s{42};
    s.unseed();
    // Two entropy seeds colliding has probability 2^-64.
    REQUIRE(r.seed() != s.seed());
    REQUIRE(rng::from_entropy().seed() != rng::from_entropy().seed());
}

TEST_CASE("child seeds match the reference implementation", "[rng][kat]")
{
    const rng r{42};
    const std::array<std::string_view, 4> labels{"", "height", "weight", "\xc3\xa9tat"};
    for (std::size_t i = 0; i < labels.size(); ++i)
    {
        REQUIRE(r.child_seed(labels[i]) == kat::child_label_42[i]);
    }
    const std::array<std::uint64_t, 4> indices{0, 1, 2, std::numeric_limits<std::uint64_t>::max()};
    for (std::size_t i = 0; i < indices.size(); ++i)
    {
        REQUIRE(r.child_seed(indices[i]) == kat::child_index_42[i]);
    }
}

TEST_CASE("split() depends on the seed, not on the draws made", "[rng]")
{
    rng a{42};
    rng b{42};
    for (int i = 0; i < 100; ++i) (void)b();
    REQUIRE(a.split("height") == b.split("height"));
    REQUIRE(a.split(3) == b.split(3));
    REQUIRE(a.split("height") != a.split("weight"));
    REQUIRE(a.split(0) != a.split(1));
    REQUIRE(a.split("height").seed() == a.child_seed("height"));
}

TEST_CASE("split() streams do not collide over many labels and indices", "[rng]")
{
    const rng r{7};
    std::set<std::uint64_t> seeds;
    for (std::uint64_t i = 0; i < 10000; ++i)
    {
        seeds.insert(r.child_seed(i));
        seeds.insert(r.child_seed("label" + std::to_string(i)));
    }
    REQUIRE(seeds.size() == 20000);
}

// ---------------------------------------------------------------------------
// Integer distributions
// ---------------------------------------------------------------------------

TEST_CASE("bounded matches the reference implementation", "[distributions][kat]")
{
    for (std::size_t b = 0; b < kat::bounded_n.size(); ++b)
    {
        rng r{7};
        for (const auto expected : kat::bounded_7[b])
        {
            REQUIRE(core::bounded(r, kat::bounded_n[b]) == expected);
        }
    }
}

TEST_CASE("uniform_int matches the reference implementation", "[distributions][kat]")
{
    rng r8{11};
    for (const auto expected : kat::int8_full_11)
    {
        REQUIRE(core::uniform_int<std::int8_t>(r8, -128, 127) == expected);
    }
    rng ri{11};
    for (const auto expected : kat::int_m5_5_11)
    {
        REQUIRE(core::uniform_int(ri, -5, 5) == expected);
    }
    rng r64{11};
    for (const auto expected : kat::int64_full_11)
    {
        const auto v = core::uniform_int(r64, std::numeric_limits<std::int64_t>::min(),
                                         std::numeric_limits<std::int64_t>::max());
        REQUIRE(static_cast<std::uint64_t>(v) == expected);
    }
}

TEST_CASE("uniform_int stays in range and covers it evenly", "[distributions][stats]")
{
    rng r{1};
    std::vector<std::size_t> counts(6);
    for (int i = 0; i < 600000; ++i)
    {
        const int v = core::uniform_int(r, 1, 6);
        REQUIRE((v >= 1 && v <= 6));
        ++counts[static_cast<std::size_t>(v - 1)];
    }
    // 5 degrees of freedom; 35.9 is the p = 1e-6 critical value.
    REQUIRE(chi_square_uniform(counts) < 35.9);
}

TEST_CASE("uniform_int handles single-value and unsigned ranges", "[distributions]")
{
    rng r{1};
    REQUIRE(core::uniform_int(r, 5, 5) == 5);
    const auto u = core::uniform_int<std::uint8_t>(r, 250, 255);
    REQUIRE((u >= 250 && u <= 255));
    REQUIRE(core::uniform_int<std::uint64_t>(r, 0, 0) == 0);
}

TEST_CASE("integer distributions reject bad arguments", "[distributions][errors]")
{
    rng r{1};
    REQUIRE_THROWS_AS(core::bounded(r, 0), std::invalid_argument);
    REQUIRE_THROWS_AS(core::uniform_int(r, 2, 1), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Real distributions
// ---------------------------------------------------------------------------

TEST_CASE("uniform01, uniform_real and bernoulli match the reference", "[distributions][kat]")
{
    rng a{13};
    for (const auto expected : kat::uniform01_13)
    {
        REQUIRE(to_bits(core::uniform01(a)) == expected);
    }
    rng b{13};
    for (const auto expected : kat::uniform_real_13)
    {
        REQUIRE(to_bits(core::uniform_real(b, -3.5, 10.25)) == expected);
    }
    rng c{13};
    for (const bool expected : kat::bernoulli_13)
    {
        REQUIRE(core::bernoulli(c, 0.3) == expected);
    }
}

TEST_CASE("normal and lognormal match the reference", "[distributions][kat]")
{
    rng a{17};
    for (const auto expected : kat::normal_17)
    {
        REQUIRE(to_bits(core::normal(a, 0.0, 1.0)) == expected);
    }
    rng b{17};
    for (const auto expected : kat::normal_height_17)
    {
        REQUIRE(to_bits(core::normal(b, 170.0, 7.5)) == expected);
    }
    rng c{17};
    for (const auto expected : kat::lognormal_17)
    {
        REQUIRE(to_bits(core::lognormal(c, 3.3, 0.2)) == expected);
    }
}

TEST_CASE("100,000 normal draws hash to the reference value", "[distributions][kat]")
{
    rng r{29};
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 100000; ++i)
    {
        std::uint64_t bits = to_bits(core::normal(r, 0.0, 1.0));
        for (int byte = 0; byte < 8; ++byte)
        {
            h ^= bits & 0xffU;
            h *= 0x100000001b3ULL;
            bits >>= 8U;
        }
    }
    REQUIRE(h == kat::normal_29_hash_100k);
}

TEST_CASE("uniform_real stays in [lo, hi)", "[distributions]")
{
    rng r{3};
    for (int i = 0; i < 100000; ++i)
    {
        const double x = core::uniform_real(r, 1.0, 1.0000000000000004);
        REQUIRE((x >= 1.0 && x < 1.0000000000000004));
    }
    REQUIRE(core::uniform_real(r, 2.5, 2.5) == 2.5);
}

TEST_CASE("real distributions have the right moments", "[distributions][stats]")
{
    constexpr int n = 400000;
    rng r{5};

    double sum = 0.0;
    double sum_sq = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double x = core::uniform01(r);
        sum += x;
        sum_sq += x * x;
    }
    const double mean01 = sum / n;
    REQUIRE(mean01 == Catch::Approx(0.5).margin(0.003));
    REQUIRE(sum_sq / n - mean01 * mean01 == Catch::Approx(1.0 / 12.0).margin(0.002));

    sum = sum_sq = 0.0;
    double sum_cube = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double x = core::normal(r, 0.0, 1.0);
        sum += x;
        sum_sq += x * x;
        sum_cube += x * x * x;
    }
    REQUIRE(sum / n == Catch::Approx(0.0).margin(0.01));
    REQUIRE(sum_sq / n == Catch::Approx(1.0).margin(0.01));
    REQUIRE(sum_cube / n == Catch::Approx(0.0).margin(0.03));

    sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        sum += core::lognormal(r, 1.0, 0.5);
    }
    // E[X] = exp(mu + sigma^2 / 2)
    REQUIRE(sum / n == Catch::Approx(std::exp(1.125)).epsilon(0.005));

    int hits = 0;
    for (int i = 0; i < n; ++i)
    {
        hits += core::bernoulli(r, 0.3) ? 1 : 0;
    }
    REQUIRE(static_cast<double>(hits) / n == Catch::Approx(0.3).margin(0.003));
}

TEST_CASE("bernoulli edges and real-distribution errors", "[distributions][errors]")
{
    rng r{1};
    for (int i = 0; i < 1000; ++i)
    {
        REQUIRE_FALSE(core::bernoulli(r, 0.0));
        REQUIRE(core::bernoulli(r, 1.0));
    }
    REQUIRE(core::normal(r, 4.0, 0.0) == 4.0);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    REQUIRE_THROWS_AS(core::bernoulli(r, -0.1), std::invalid_argument);
    REQUIRE_THROWS_AS(core::bernoulli(r, 1.1), std::invalid_argument);
    REQUIRE_THROWS_AS(core::bernoulli(r, nan), std::invalid_argument);
    REQUIRE_THROWS_AS(core::uniform_real(r, 2.0, 1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(core::uniform_real(r, 0.0, inf), std::invalid_argument);
    REQUIRE_THROWS_AS(core::uniform_real(r, -1.7e308, 1.7e308), std::invalid_argument);
    REQUIRE_THROWS_AS(core::normal(r, 0.0, -1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(core::normal(r, nan, 1.0), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// discrete, shuffle, sample, pick
// ---------------------------------------------------------------------------

TEST_CASE("discrete matches the reference", "[distributions][kat]")
{
    const core::discrete d{0.5, 0.0, 2.25, 1.0, 0.25};
    rng r{19};
    for (const auto expected : kat::discrete_19)
    {
        REQUIRE(d(r) == expected);
    }
}

TEST_CASE("discrete follows its weights and never picks a zero weight", "[distributions][stats]")
{
    const std::vector<double> weights{0.5, 0.0, 2.25, 1.0, 0.25};
    const core::discrete d{weights};
    REQUIRE(d.size() == 5);
    REQUIRE(d.total() == 4.0);
    REQUIRE(d.probability(2) == 2.25 / 4.0);
    REQUIRE(d.probability(1) == 0.0);

    rng r{2};
    std::vector<std::size_t> counts(weights.size());
    constexpr int n = 400000;
    for (int i = 0; i < n; ++i)
    {
        ++counts[d(r)];
    }
    REQUIRE(counts[1] == 0);
    double chi = 0.0;
    for (std::size_t i = 0; i < weights.size(); ++i)
    {
        if (weights[i] == 0.0) continue;
        const double expected = n * d.probability(i);
        const double diff = static_cast<double>(counts[i]) - expected;
        chi += diff * diff / expected;
    }
    // 3 degrees of freedom; 30.7 is the p = 1e-6 critical value.
    REQUIRE(chi < 30.7);
}

TEST_CASE("discrete rejects bad weights", "[distributions][errors]")
{
    const std::vector<double> empty;
    REQUIRE_THROWS_AS(core::discrete{empty}, std::invalid_argument);
    REQUIRE_THROWS_AS((core::discrete{0.0, 0.0}), std::invalid_argument);
    REQUIRE_THROWS_AS((core::discrete{1.0, -1.0}), std::invalid_argument);
    REQUIRE_THROWS_AS((core::discrete{1.0, std::numeric_limits<double>::infinity()}),
                      std::invalid_argument);
    REQUIRE_THROWS_AS((core::discrete{1.7e308, 1.7e308}), std::invalid_argument);
    REQUIRE_THROWS_AS((core::discrete{1.0}.probability(1)), std::out_of_range);
}

TEST_CASE("shuffle and sample_indices match the reference", "[distributions][kat]")
{
    std::array<int, 10> items{};
    std::iota(items.begin(), items.end(), 0);
    rng a{23};
    core::shuffle(a, items);
    REQUIRE(items == kat::shuffle_23);

    rng b{23};
    const auto sample = core::sample_indices(b, 20, 5);
    REQUIRE(sample.size() == 5);
    for (std::size_t i = 0; i < sample.size(); ++i)
    {
        REQUIRE(sample[i] == kat::sample_23[i]);
    }
}

TEST_CASE("shuffle places each element evenly", "[distributions][stats]")
{
    rng r{4};
    std::vector<std::size_t> first_position(5);
    for (int i = 0; i < 200000; ++i)
    {
        std::vector<int> v{0, 1, 2, 3, 4};
        core::shuffle(r, v);
        ++first_position[static_cast<std::size_t>(v[0])];
    }
    // 4 degrees of freedom; 33.4 is the p = 1e-6 critical value.
    REQUIRE(chi_square_uniform(first_position) < 33.4);
}

TEST_CASE("sample_indices gives distinct indices; pick gives elements", "[distributions]")
{
    rng r{6};
    const auto all = core::sample_indices(r, 50, 50);
    REQUIRE(std::set<std::size_t>(all.begin(), all.end()).size() == 50);
    REQUIRE(core::sample_indices(r, 5, 0).empty());
    REQUIRE_THROWS_AS(core::sample_indices(r, 3, 4), std::invalid_argument);

    std::vector<int> v{10, 20, 30};
    int& chosen = core::pick(r, v);
    REQUIRE((chosen == 10 || chosen == 20 || chosen == 30));
    chosen = 99;
    REQUIRE(std::ranges::count(v, 99) == 1);
    std::vector<int> empty;
    REQUIRE_THROWS_AS(core::pick(r, empty), std::invalid_argument);

    std::vector<int> single{1};
    core::shuffle(r, single);
    std::vector<int> none;
    core::shuffle(r, none);
    REQUIRE(single == std::vector<int>{1});
}

// ---------------------------------------------------------------------------
// math
// ---------------------------------------------------------------------------

TEST_CASE("exp and log match the reference bit for bit", "[math][kat]")
{
    for (std::size_t i = 0; i < kat::exp_in.size(); ++i)
    {
        INFO("exp(" << from_bits(kat::exp_in[i]) << ")");
        REQUIRE(to_bits(core::math::exp(from_bits(kat::exp_in[i]))) == kat::exp_out[i]);
    }
    for (std::size_t i = 0; i < kat::log_in.size(); ++i)
    {
        INFO("log(" << from_bits(kat::log_in[i]) << ")");
        REQUIRE(to_bits(core::math::log(from_bits(kat::log_in[i]))) == kat::log_out[i]);
    }
}

TEST_CASE("exp and log are within a few ulp of the platform libm", "[math]")
{
    rng r{8};
    double worst_exp = 0.0;
    double worst_log = 0.0;
    for (int i = 0; i < 200000; ++i)
    {
        const double x = core::uniform_real(r, -700.0, 709.0);
        worst_exp = std::max(worst_exp, ulp_distance(core::math::exp(x), std::exp(x)));
        const double y = std::exp(core::uniform_real(r, -700.0, 700.0));
        worst_log = std::max(worst_log, ulp_distance(core::math::log(y), std::log(y)));
        const double z = core::uniform_real(r, 0.5, 2.0);
        worst_log = std::max(worst_log, ulp_distance(core::math::log(z), std::log(z)));
    }
    // The reference measures at most 1 ulp (exp) and 3 ulp (log) against
    // glibc; allow 1 more for the platform libm's own error.
    REQUIRE(worst_exp <= 2.0);
    REQUIRE(worst_log <= 4.0);
}

TEST_CASE("exp and log special values", "[math]")
{
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();
    REQUIRE(core::math::exp(0.0) == 1.0);
    REQUIRE(core::math::exp(1000.0) == inf);
    REQUIRE(core::math::exp(-1000.0) == 0.0);
    REQUIRE(core::math::exp(-inf) == 0.0);
    REQUIRE(std::isnan(core::math::exp(nan)));
    REQUIRE(core::math::log(1.0) == 0.0);
    REQUIRE(core::math::log(0.0) == -inf);
    REQUIRE(core::math::log(inf) == inf);
    REQUIRE(std::isnan(core::math::log(-1.0)));
    REQUIRE(std::isnan(core::math::log(nan)));
}
