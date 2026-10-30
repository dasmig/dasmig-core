#include "catch_amalgamated.hpp"

#include "../dasmig/core.hpp"
#include "kat_vectors.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <numbers>
#include <numeric>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

DASMIG_CORE_REQUIRE(1, 0);

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

TEST_CASE("sex enum is defined once behind the legacy guard", "[sex]")
{
#ifndef DASMIG_SEX_DEFINED
    FAIL("DASMIG_SEX_DEFINED not set: old library headers would redefine sex");
#endif
    STATIC_REQUIRE(dasmig::sex::male != dasmig::sex::female);
}

TEST_CASE("sex_name and parse_sex round-trip", "[sex]")
{
    STATIC_REQUIRE(core::sex_name(dasmig::sex::male) == "male");
    STATIC_REQUIRE(core::sex_name(dasmig::sex::female) == "female");
    STATIC_REQUIRE(core::sex_name(static_cast<dasmig::sex>(7)) == "unknown");
    STATIC_REQUIRE(core::parse_sex("male") == dasmig::sex::male);
    STATIC_REQUIRE(core::parse_sex("female") == dasmig::sex::female);
    for (const auto s : {dasmig::sex::male, dasmig::sex::female})
    {
        REQUIRE(core::parse_sex(core::sex_name(s)) == s);
    }
}

TEST_CASE("parse_sex is exact and case-sensitive", "[sex]")
{
    for (const char* text : {"", "m", "f", "Male", "FEMALE", " male", "male ", "other", "unknown"})
    {
        REQUIRE_FALSE(core::parse_sex(text).has_value());
    }
}

TEST_CASE("is_alpha2 accepts exactly two capital ASCII letters", "[country]")
{
    STATIC_REQUIRE(core::is_alpha2("BR"));
    STATIC_REQUIRE(core::is_alpha2("ZZ"));
    for (const char* text : {"", "B", "BRA", "br", "Br", "bR", "B1", "1B", "B ", "--", "B\xC3\x89"})
    {
        REQUIRE_FALSE(core::is_alpha2(text));
    }
}

TEST_CASE("unknown_country_error carries the code", "[country]")
{
    const auto error = core::unknown_country_error("XX");
    REQUIRE(std::string{error.what()} == "Unknown country code: XX");
    REQUIRE_THROWS_AS([] { throw core::unknown_country_error("QQ"); }(), std::invalid_argument);
    REQUIRE_THROWS_WITH([] { throw core::unknown_country_error(""); }(), "Unknown country code: ");
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
    REQUIRE_THROWS_AS(core::gamma(r, 0.0, 1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(core::gamma(r, 1.0, -1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(core::gamma(r, nan, 1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(core::beta(r, 1.0, 0.0), std::invalid_argument);
    REQUIRE_THROWS_AS(core::beta(r, inf, 1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(core::weibull(r, -2.0, 1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(core::weibull(r, 2.0, inf), std::invalid_argument);
    REQUIRE(core::gamma(r, 2.0, 0.0) == 0.0);
    REQUIRE(core::weibull(r, 2.0, 0.0) == 0.0);
}

TEST_CASE("gamma, beta and weibull match the reference", "[distributions][kat]")
{
    for (std::size_t i = 0; i < kat::gamma_shapes.size(); ++i)
    {
        const double shape = from_bits(kat::gamma_shapes[i]);
        INFO("gamma shape " << shape);
        rng r{31};
        for (const auto expected : kat::gamma_31[i])
        {
            REQUIRE(to_bits(core::gamma(r, shape, 2.0)) == expected);
        }
    }
    rng b{37};
    for (const auto expected : kat::beta_37)
    {
        REQUIRE(to_bits(core::beta(b, 0.6 * 4.0, 0.4 * 4.0)) == expected);
    }
    rng w{41};
    for (const auto expected : kat::weibull_41)
    {
        REQUIRE(to_bits(core::weibull(w, 2.902, 4.25)) == expected);
    }
}

TEST_CASE("100,000 gamma draws hash to the reference value", "[distributions][kat]")
{
    rng r{43};
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 100000; ++i)
    {
        std::uint64_t bits = to_bits(core::gamma(r, 0.4 + ((i % 7) * 0.5), 1.5));
        for (int byte = 0; byte < 8; ++byte)
        {
            h ^= bits & 0xffU;
            h *= 0x100000001b3ULL;
            bits >>= 8U;
        }
    }
    REQUIRE(h == kat::gamma_43_hash_100k);
}

TEST_CASE("gamma, beta and weibull have the right moments", "[distributions][stats]")
{
    constexpr int n = 200000;
    rng r{47};
    for (const double shape : {0.3, 0.8, 1.0, 2.5, 9.0})
    {
        INFO("gamma shape " << shape);
        double sum = 0.0;
        double sum_sq = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double x = core::gamma(r, shape, 2.0);
            REQUIRE(x >= 0.0);
            sum += x;
            sum_sq += x * x;
        }
        const double mean = sum / n;
        // mean k theta, variance k theta^2
        REQUIRE(mean == Catch::Approx(shape * 2.0).epsilon(0.01));
        REQUIRE(sum_sq / n - mean * mean == Catch::Approx(shape * 4.0).epsilon(0.03));
    }

    double sum = 0.0;
    double sum_sq = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double x = core::beta(r, 2.4, 1.6);
        REQUIRE((x >= 0.0 && x <= 1.0));
        sum += x;
        sum_sq += x * x;
    }
    // mean a / (a + b), variance a b / ((a + b)^2 (a + b + 1))
    const double beta_mean = sum / n;
    REQUIRE(beta_mean == Catch::Approx(0.6).margin(0.003));
    REQUIRE(sum_sq / n - beta_mean * beta_mean == Catch::Approx(0.048).margin(0.001));

    sum = 0.0;
    for (int i = 0; i < n; ++i) sum += core::weibull(r, 2.0, 3.0);
    // mean scale * Gamma(1 + 1 / k); Gamma(1.5) = sqrt(pi) / 2
    REQUIRE(sum / n == Catch::Approx(3.0 * std::sqrt(std::numbers::pi) / 2.0).epsilon(0.005));
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

TEST_CASE("trigonometry matches the reference bit for bit", "[math][kat]")
{
    for (std::size_t i = 0; i < kat::trig_in.size(); ++i)
    {
        const double x = from_bits(kat::trig_in[i]);
        INFO("sin_cos_deg(" << x << ")");
        const auto sc = core::math::sin_cos_deg(x);
        REQUIRE(to_bits(sc.sin) == kat::sin_deg_out[i]);
        REQUIRE(to_bits(sc.cos) == kat::cos_deg_out[i]);
        REQUIRE(to_bits(core::math::sin_deg(x)) == kat::sin_deg_out[i]);
        REQUIRE(to_bits(core::math::cos_deg(x)) == kat::cos_deg_out[i]);
    }
    for (std::size_t i = 0; i < kat::atan_in.size(); ++i)
    {
        INFO("atan(" << from_bits(kat::atan_in[i]) << ")");
        REQUIRE(to_bits(core::math::atan(from_bits(kat::atan_in[i]))) == kat::atan_out[i]);
    }
    for (std::size_t i = 0; i < kat::atan2_in.size(); ++i)
    {
        const double y = from_bits(kat::atan2_in[i][0]);
        const double x = from_bits(kat::atan2_in[i][1]);
        INFO("atan2_deg(" << y << ", " << x << ")");
        REQUIRE(to_bits(core::math::atan2_deg(y, x)) == kat::atan2_deg_out[i]);
    }
    for (std::size_t i = 0; i < kat::arc_in.size(); ++i)
    {
        const double x = from_bits(kat::arc_in[i]);
        INFO("asin_deg and acos_deg(" << x << ")");
        REQUIRE(to_bits(core::math::asin_deg(x)) == kat::asin_deg_out[i]);
        REQUIRE(to_bits(core::math::acos_deg(x)) == kat::acos_deg_out[i]);
    }
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 100000; ++i)
    {
        const auto sc = core::math::sin_cos_deg(-1000.0 + core::math::fenced(i * 0.0213));
        for (std::uint64_t bits : {to_bits(sc.sin), to_bits(sc.cos)})
        {
            for (int byte = 0; byte < 8; ++byte)
            {
                h ^= bits & 0xffU;
                h *= 0x100000001b3ULL;
                bits >>= 8U;
            }
        }
    }
    REQUIRE(h == kat::sin_cos_hash_100k);
}

TEST_CASE("trigonometry is within a few ulp of the platform libm", "[math]")
{
    constexpr double rad = std::numbers::pi / 180.0;
    rng r{53};
    double worst = 0.0;
    double worst_atan = 0.0;
    for (int i = 0; i < 200000; ++i)
    {
        const double d = core::uniform_real(r, -720.0, 720.0);
        const auto sc = core::math::sin_cos_deg(d);
        // Absolute error: near a zero of sin or cos an ulp count means nothing.
        worst = std::max({worst, std::abs(sc.sin - std::sin(d * rad)), std::abs(sc.cos - std::cos(d * rad))});
        const double t = core::uniform_real(r, -50.0, 50.0);
        worst_atan = std::max(worst_atan, ulp_distance(core::math::atan(t), std::atan(t)));
    }
    // std::sin(d * rad) itself carries the rounding of d * rad: half an ulp
    // of 4 pi is 8.9e-16.
    REQUIRE(worst < 2e-15);
    REQUIRE(worst_atan <= 2.0);
    for (const double x : {-0.999, -0.5, 0.0, 0.3, 0.7071, 0.999})
    {
        REQUIRE(core::math::asin_deg(x) == Catch::Approx(std::asin(x) / rad).margin(1e-12));
        REQUIRE(core::math::acos_deg(x) == Catch::Approx(std::acos(x) / rad).margin(1e-12));
    }
    REQUIRE(core::math::atan2_deg(-1.0, -1.0) == Catch::Approx(-135.0).margin(1e-12));
}

TEST_CASE("trigonometry special values", "[math]")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    REQUIRE(core::math::sin_deg(90.0) == 1.0);
    REQUIRE(core::math::cos_deg(0.0) == 1.0);
    REQUIRE(core::math::sin_deg(180.0) == 0.0);
    REQUIRE(core::math::cos_deg(-180.0) == -1.0);
    REQUIRE(std::isnan(core::math::sin_deg(nan)));
    REQUIRE(std::isnan(core::math::cos_deg(inf)));
    REQUIRE(core::math::atan(inf) == std::numbers::pi / 2.0);
    REQUIRE(std::isnan(core::math::atan(nan)));
    REQUIRE(core::math::atan2_deg(0.0, 0.0) == 0.0);
    REQUIRE(core::math::atan2_deg(2.0, 0.0) == 90.0);
    REQUIRE(std::isnan(core::math::atan2_deg(nan, 1.0)));
    REQUIRE(core::math::asin_deg(2.0) == 90.0);
    REQUIRE(core::math::acos_deg(-2.0) == 180.0);
    REQUIRE(std::isnan(core::math::asin_deg(nan)));
    REQUIRE(std::isnan(core::math::acos_deg(nan)));
}

// ---------------------------------------------------------------------------
// Check digits
// ---------------------------------------------------------------------------

namespace
{

// The national rules a library writes over weighted_sum (core design 2.4).
std::string cpf_with_digits(std::string cpf)
{
    for (const unsigned top : {10U, 11U})
    {
        std::array<unsigned, 10> w{};
        for (unsigned i = 0; i < cpf.size(); ++i) w.at(i) = top - i;
        const auto rem = core::weighted_sum(cpf, std::span{w}.first(cpf.size())) % 11;
        cpf += static_cast<char>('0' + (rem < 2 ? 0 : 11 - rem));
    }
    return cpf;
}

// The NHS number's check digit, or 'N' when the body is never issued.
char nhs_digit(std::string_view body)
{
    constexpr std::array<unsigned, 9> w{10, 9, 8, 7, 6, 5, 4, 3, 2};
    const auto c = 11 - (core::weighted_sum(body, w) % 11);
    if (c == 10) return 'N';
    return static_cast<char>('0' + (c == 11 ? 0 : c));
}

} // namespace

TEST_CASE("check digits match published values", "[check_digits][kat]")
{
    REQUIRE(core::luhn_digit("7992739871") == '3');
    REQUIRE(core::luhn_valid("79927398713"));

    REQUIRE(core::iban_valid("GB82WEST12345698765432"));
    REQUIRE(core::iban_valid("DE89370400440532013000"));
    REQUIRE(core::iban_check_digits("DE", "370400440532013000") == std::array{'8', '9'});
    // An ISO 11649 creditor reference moves its first four characters to the end.
    REQUIRE(core::mod97_10_valid("539007547034RF18"));
    REQUIRE(core::mod97_10_digits("539007547034RF") == std::array{'1', '8'});

    REQUIRE(core::mod11_2_digit("11010519491231002") == 'X'); // Chinese resident ID
    REQUIRE(core::mod11_2_valid("11010519491231002X"));
    REQUIRE(core::mod11_2_digit("000000021825009") == '7');   // ORCID 0000-0002-1825-0097
    REQUIRE(core::mod11_10_digit("13669597") == '6');         // VAT number DE136695976

    REQUIRE(cpf_with_digits("111444777") == "11144477735");
    REQUIRE(nhs_digit("943476591") == '9');

    REQUIRE(core::gtin_digit("400638133393") == '1');
    REQUIRE(core::gtin_valid("4006381333931"));

    // The ICAO 9303 specimen passport: number, birth date, expiry date.
    REQUIRE(core::icao9303_digit("L898902C3") == '6');
    REQUIRE(core::icao9303_digit("740812") == '2');
    REQUIRE(core::icao9303_digit("120415") == '9');
    REQUIRE(core::icao9303_valid("L898902C36"));

    REQUIRE(core::verhoeff_digit("236") == '3');
    REQUIRE(core::verhoeff_digit("12345") == '1');
    REQUIRE(core::verhoeff_digit("142857") == '0');
    REQUIRE(core::verhoeff_valid("2363"));
}

TEST_CASE("check digits evaluate at compile time", "[check_digits]")
{
    STATIC_REQUIRE(core::digit_value('7') == 7);
    STATIC_REQUIRE(core::alnum_value('Z') == 35);
    STATIC_REQUIRE(core::luhn_digit("7992739871") == '3');
    STATIC_REQUIRE(core::mod97_10_digits("539007547034RF") == std::array{'1', '8'});
    STATIC_REQUIRE(core::iban_check_digits("DE", "370400440532013000") == std::array{'8', '9'});
    STATIC_REQUIRE(core::iban_valid("GB82WEST12345698765432"));
    STATIC_REQUIRE(core::mod11_2_digit("11010519491231002") == 'X');
    STATIC_REQUIRE(core::mod11_10_digit("13669597") == '6');
    static constexpr std::array<unsigned, 9> cpf_weights{10, 9, 8, 7, 6, 5, 4, 3, 2};
    STATIC_REQUIRE(core::weighted_sum("111444777", cpf_weights) == 162);
    STATIC_REQUIRE(core::gtin_digit("400638133393") == '1');
    STATIC_REQUIRE(core::icao9303_digit("L898902C3") == '6');
    STATIC_REQUIRE(core::verhoeff_digit("236") == '3');
    STATIC_REQUIRE(core::verhoeff_valid("2363"));
}

TEST_CASE("check-digit errors name the function and the position", "[check_digits]")
{
    REQUIRE_THROWS_WITH(core::luhn_digit("123-4"), "luhn_digit: position 3: '-' is not a digit");
    REQUIRE_THROWS_WITH(core::luhn_digit(""), "luhn_digit: empty input");
    REQUIRE_THROWS_WITH(core::verhoeff_digit("1\xC3"), "verhoeff_digit: position 1: byte 0xC3 is not a digit");
    REQUIRE_THROWS_WITH(core::mod97_10_digits("12ab"),
                        "mod97_10_digits: position 2: 'a' is not a digit or capital letter");
    REQUIRE_THROWS_WITH(core::iban_check_digits("De", "1234"),
                        "iban_check_digits: country: position 1: 'e' is not a capital letter");
    REQUIRE_THROWS_WITH(core::iban_check_digits("DEU", "1234"),
                        "iban_check_digits: country must be two capital letters");
    REQUIRE_THROWS_WITH(core::iban_check_digits("DE", "12 4"),
                        "iban_check_digits: bban: position 2: ' ' is not a digit or capital letter");
    REQUIRE_THROWS_WITH(core::icao9303_digit("AB-"),
                        "icao9303_digit: position 2: '-' is not a digit, capital letter or '<'");
    REQUIRE_THROWS_WITH(core::digit_value('a'), "digit_value: 'a' is not a digit");
    REQUIRE_THROWS_WITH(core::alnum_value('a'), "alnum_value: 'a' is not a digit or capital letter");
    constexpr std::array<unsigned, 2> w{1, 2};
    REQUIRE_THROWS_WITH(core::weighted_sum("12x", w), "weighted_sum: position 2: digit_value: 'x' is not a digit");
    REQUIRE_THROWS_WITH(core::weighted_sum("1", std::span<const unsigned>{}), "weighted_sum: no weights");
    REQUIRE(core::weighted_sum("", w) == 0);
    REQUIRE(core::weighted_sum("Z1", w, &core::alnum_value) == 37);
}

TEST_CASE("check-digit functions reject every malformed input", "[check_digits]")
{
    using digit_fn = char (*)(std::string_view);
    for (const digit_fn f : {digit_fn{&core::luhn_digit}, digit_fn{&core::mod11_2_digit},
                             digit_fn{&core::mod11_10_digit}, digit_fn{&core::gtin_digit},
                             digit_fn{&core::verhoeff_digit}})
    {
        REQUIRE_THROWS_AS(f(""), std::invalid_argument);
        REQUIRE_THROWS_AS(f("12A4"), std::invalid_argument);
        REQUIRE_THROWS_AS(f(" 1"), std::invalid_argument);
    }
    REQUIRE_THROWS_AS(core::mod97_10_digits(""), std::invalid_argument);
    REQUIRE_THROWS_AS(core::iban_check_digits("DE", ""), std::invalid_argument);
    REQUIRE_THROWS_AS(core::icao9303_digit(""), std::invalid_argument);
    REQUIRE_THROWS_AS(core::icao9303_digit("ab"), std::invalid_argument);

    using valid_fn = bool (*)(std::string_view) noexcept;
    for (const valid_fn f : {valid_fn{&core::luhn_valid}, valid_fn{&core::mod97_10_valid},
                             valid_fn{&core::mod11_2_valid}, valid_fn{&core::mod11_10_valid},
                             valid_fn{&core::gtin_valid}, valid_fn{&core::icao9303_valid},
                             valid_fn{&core::verhoeff_valid}, valid_fn{&core::iban_valid}})
    {
        for (const char* text : {"", "0", "12-45", "1 2", "a1", "\xC3\xA9", "1234567890123456789x"})
        {
            REQUIRE_FALSE(f(text));
        }
    }
    REQUIRE(core::luhn_valid("00"));
    REQUIRE_FALSE(core::mod97_10_valid("1"));
    REQUIRE_FALSE(core::mod97_10_valid("539007547034RFX8"));
    REQUIRE_FALSE(core::mod11_2_valid("1X2"));
    REQUIRE_FALSE(core::mod11_2_valid("11010519491231002x"));
    REQUIRE(core::icao9303_valid("<<<0"));
    REQUIRE_FALSE(core::icao9303_valid("L898902C3<"));
    REQUIRE_FALSE(core::iban_valid("gb82WEST12345698765432"));
    REQUIRE_FALSE(core::iban_valid("GB82 WEST 1234 5698 7654 32"));
    REQUIRE_FALSE(core::iban_valid("GBX2WEST12345698765432"));
    REQUIRE_FALSE(core::iban_valid("GB82WEST1234569876543")); // one digit short
    REQUIRE_FALSE(core::iban_valid("NO938601111794"));        // 14 characters
    REQUIRE(core::iban_valid("NO9386011117947"));             // 15, the shortest
    REQUIRE_FALSE(core::iban_valid(std::string(35, '1')));
}

namespace
{

// A check-digit scheme and the errors it claims to catch (core design 2.3).
struct check_scheme
{
    const char* name;
    std::size_t check_size;          // check characters at the end
    std::string_view check_alphabet; // characters a check position can hold
    std::string (*complete)(std::string_view body);
    bool (*valid)(std::string_view) noexcept;
    // 'a': every adjacent transposition is caught; 'l': all but 09 <-> 90;
    // 'g': all but pairs that differ by 5; 'n': no claim.
    char transpositions;
};

const std::array<check_scheme, 7>& check_schemes()
{
    static const std::array<check_scheme, 7> schemes{{
        {"luhn", 1, "0123456789",
         [](std::string_view b) { return std::string{b} + core::luhn_digit(b); }, &core::luhn_valid, 'l'},
        {"mod97_10", 2, "0123456789",
         [](std::string_view b) {
             const auto d = core::mod97_10_digits(b);
             return std::string{b} + d[0] + d[1];
         },
         &core::mod97_10_valid, 'a'},
        {"mod11_2", 1, "0123456789X",
         [](std::string_view b) { return std::string{b} + core::mod11_2_digit(b); }, &core::mod11_2_valid, 'a'},
        {"mod11_10", 1, "0123456789",
         [](std::string_view b) { return std::string{b} + core::mod11_10_digit(b); }, &core::mod11_10_valid, 'n'},
        {"gtin", 1, "0123456789",
         [](std::string_view b) { return std::string{b} + core::gtin_digit(b); }, &core::gtin_valid, 'g'},
        {"icao9303", 1, "0123456789",
         [](std::string_view b) { return std::string{b} + core::icao9303_digit(b); }, &core::icao9303_valid, 'n'},
        {"verhoeff", 1, "0123456789",
         [](std::string_view b) { return std::string{b} + core::verhoeff_digit(b); }, &core::verhoeff_valid, 'a'},
    }};
    return schemes;
}

std::string random_digits(rng& r, std::size_t n)
{
    std::string s(n, '0');
    for (auto& c : s) c = static_cast<char>('0' + core::bounded(r, 10));
    return s;
}

} // namespace

TEST_CASE("check digits catch the errors each scheme claims", "[check_digits]")
{
    for (const check_scheme& s : check_schemes())
    {
        INFO(s.name);
        rng r{61};
        std::size_t failures = 0;
        std::size_t transpositions = 0;
        std::size_t missed_transpositions = 0;
        std::string example;
        for (int n = 0; n < 10000; ++n)
        {
            const std::string v = s.complete(random_digits(r, 11));
            if (!s.valid(v)) ++failures;
            std::string w = v;
            for (std::size_t i = 0; i < v.size(); ++i)
            {
                const bool check_position = i >= v.size() - s.check_size;
                for (const char c : check_position ? s.check_alphabet : std::string_view{"0123456789"})
                {
                    if (c == v[i]) continue;
                    w[i] = c;
                    if (s.valid(w))
                    {
                        ++failures;
                        example = w;
                    }
                }
                w[i] = v[i];
            }
            for (std::size_t i = 0; i + 1 < v.size(); ++i)
            {
                const char a = v[i];
                const char b = v[i + 1];
                if (a == b) continue;
                ++transpositions;
                std::swap(w[i], w[i + 1]);
                if (s.valid(w))
                {
                    ++missed_transpositions;
                    const int diff = a > b ? a - b : b - a;
                    const bool exempt = s.transpositions == 'n' ||
                                        (s.transpositions == 'l' && diff == 9) ||
                                        (s.transpositions == 'g' && diff == 5);
                    if (!exempt)
                    {
                        ++failures;
                        example = w;
                    }
                }
                std::swap(w[i], w[i + 1]);
            }
        }
        INFO("example: " << example);
        REQUIRE(failures == 0);
        // The exceptions each claim allows do occur.
        if (s.transpositions == 'l' || s.transpositions == 'g') REQUIRE(missed_transpositions > 0);
        if (std::string_view{s.name} == "mod11_10")
        {
            // ISO 7064's hybrid system misses a few transpositions, which depend
            // on the running state, not on the two digits alone.
            REQUIRE(missed_transpositions > 0);
            REQUIRE(static_cast<double>(missed_transpositions) < 0.05 * static_cast<double>(transpositions));
        }
    }
}

TEST_CASE("check digits round-trip through the valid functions", "[check_digits]")
{
    for (const check_scheme& s : check_schemes())
    {
        INFO(s.name);
        rng r{67};
        std::size_t failures = 0;
        for (int n = 0; n < 100000; ++n)
        {
            const auto length = static_cast<std::size_t>(core::uniform_int(r, 1, 20));
            if (!s.valid(s.complete(random_digits(r, length)))) ++failures;
        }
        REQUIRE(failures == 0);
    }
    rng r{71};
    for (int n = 0; n < 10000; ++n)
    {
        std::string bban = random_digits(r, 18);
        for (std::size_t i = 0; i < 4; ++i) bban.at(i) = static_cast<char>('A' + core::bounded(r, 26));
        const auto d = core::iban_check_digits("GB", bban);
        REQUIRE(core::iban_valid(std::string{"GB"} + d[0] + d[1] + bban));
    }
}

// ---------------------------------------------------------------------------
// Patterns
// ---------------------------------------------------------------------------

TEST_CASE("pattern parse errors give the position and the reason", "[pattern]")
{
    const std::vector<std::pair<std::string, std::string>> cases{
        {"[Z-A]", "position 1: reversed range 'Z'-'A'"},
        {"a|b", "position 1: '|' outside a group"},
        {"a)", "position 1: stray ')'"},
        {"#]", "position 1: stray ']'"},
        {"#}", "position 1: stray '}'"},
        {"(a", "position 0: unclosed '('"},
        {"(a|", "position 3: empty alternative"},
        {"(|a)", "position 1: empty alternative"},
        {"()", "position 1: empty alternative"},
        {"[AB", "position 0: unclosed '['"},
        {"[]", "position 0: empty class"},
        {"[AA]", "position 2: repeated 'A' in class"},
        {"[A-CB]", "position 4: repeated 'B' in class"},
        {"#{2", "position 1: unclosed '{'"},
        {"#{", "position 1: unclosed '{'"},
        {"#{a}", "position 2: expected a count, found 'a'"},
        {"#{2;3}", "position 3: expected '}', found ';'"},
        {"#{2,1}", "position 1: quantifier maximum below its minimum"},
        {"#{0}", "position 1: quantifier of zero"},
        {"#{0,0}", "position 1: quantifier of zero"},
        {"#{65}", "position 2: count above 64"},
        {"#{2}{3}", "position 4: quantifier after another quantifier"},
        {"{2}", "position 0: quantifier after nothing"},
        {"(#|{2})", "position 3: quantifier after nothing"},
        {"#\\", "position 1: dangling '\\'"},
        {"#\t", "position 1: byte 0x09: patterns are printable ASCII"},
        {"\xC3\xA9", "position 0: byte 0xC3: patterns are printable ASCII"},
        {"(((((((((#)))))))))", "position 8: groups nested deeper than 8"},
    };
    for (const auto& [text, message] : cases)
    {
        INFO(text);
        REQUIRE_THROWS_WITH(core::pattern::parse(text), "pattern::parse: " + message);
    }
    REQUIRE_NOTHROW(core::pattern::parse("((((((((#))))))))"));
    REQUIRE_THROWS_WITH(core::pattern::parse(std::string(1025, 'A')),
                        "pattern::parse: 1025 characters, above 1024");
    REQUIRE_NOTHROW(core::pattern::parse(std::string(256, 'A')));
    REQUIRE_THROWS_WITH(core::pattern::parse("#{64}#{64}#{64}#{64}#"),
                        "pattern::parse: makes strings of up to 257 characters, above 256");
    REQUIRE_THROWS_WITH(core::pattern::parse("((#{64}){64}){64}"),
                        "pattern::parse: makes strings of up to 262144 characters, above 256");
}

TEST_CASE("pattern draws match the reference implementation", "[pattern][kat]")
{
    for (std::size_t p = 0; p < kat::pattern_text.size(); ++p)
    {
        const auto pattern = core::pattern::parse(kat::pattern_text.at(p));
        for (std::size_t s = 0; s < kat::pattern_seed.size(); ++s)
        {
            INFO(kat::pattern_text.at(p) << " seed " << kat::pattern_seed.at(s));
            rng r{kat::pattern_seed.at(s)};
            REQUIRE(pattern.draw(r) == kat::pattern_draw.at(p).at(s));
        }
    }
    const auto last = core::pattern::parse(kat::pattern_text.back());
    rng r{11};
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (int i = 0; i < 1000; ++i)
    {
        for (const char c : last.draw(r) + "\n")
        {
            h ^= static_cast<std::uint8_t>(c);
            h *= 0x100000001b3ULL;
        }
    }
    REQUIRE(h == kat::pattern_11_hash_1k);
}

TEST_CASE("a fixed-length pattern consumes one value per variable character", "[pattern]")
{
    const auto p = core::pattern::parse("@@-###");
    rng a{7};
    rng b{7};
    (void)p.draw(a);
    for (int i = 0; i < 5; ++i) (void)b();
    REQUIRE(a == b);
}

TEST_CASE("drawn identifiers match their patterns", "[pattern]")
{
    for (const char* text : {
             "#########",                                              // CPF body
             "[A-CEGHJ-PR-TW-Z][A-CEGHJ-NPR-TW-Z]######[A-D]",         // UK National Insurance
             "##################",                                     // German BBAN
             "[A-HJ-NP-Z2-9]{6}",                                      // booking reference
             "###-##-####",                                            // US SSN shape
             "######(19|20)##(0[1-9]|1[0-2])(0[1-9]|[12]#|3[01])###", // Chinese ID body
             "(900|985)#{12}",                                         // ISO 11784 microchip
             "(DE|GB(##|@{2,4})|FR)-#{1,3}(\\.@){0,2}",
         })
    {
        INFO(text);
        const auto p = core::pattern::parse(text);
        const auto [shortest, longest] = p.lengths();
        rng r{73};
        for (int i = 0; i < 1000; ++i)
        {
            const std::string s = p.draw(r);
            REQUIRE(p.matches(s));
            REQUIRE(s.size() >= shortest);
            REQUIRE(s.size() <= longest);
        }
    }
}

TEST_CASE("pattern matching", "[pattern]")
{
    const auto p = core::pattern::parse("@@-###");
    REQUIRE(p.matches("AB-123"));
    for (const char* s : {"", "AB-12", "AB-1234", "ab-123", "AB123", "AB-12A", "A\xC3\x89-123"})
    {
        REQUIRE_FALSE(p.matches(s));
    }
    const auto escaped = core::pattern::parse("\\#\\{x\\}\\\\");
    REQUIRE(escaped.matches("#{x}\\"));
    REQUIRE(escaped.count() == 1);
    const auto classes = core::pattern::parse("[-A][B-][ -/]");
    REQUIRE(classes.matches("AB/"));
    REQUIRE(classes.matches("--."));
    REQUIRE(classes.count() == 2 * 2 * 16);
    // Alternatives and repetition counts that only fit one way.
    const auto alt = core::pattern::parse("(AB|A)(BC|C){1,2}D");
    for (const char* s : {"ABCD", "ACD", "ABBCD", "ABCBCD", "ACCD", "ABCCD"}) REQUIRE(alt.matches(s));
    for (const char* s : {"AD", "ABD", "ABCBCBCD", "ABC"}) REQUIRE_FALSE(alt.matches(s));
    const core::pattern empty;
    REQUIRE(empty.matches(""));
    REQUIRE_FALSE(empty.matches("A"));
    REQUIRE(empty == core::pattern::parse(""));
    REQUIRE(empty.count() == 1);
    REQUIRE(empty.lengths() == std::pair<std::size_t, std::size_t>{0, 0});
    rng r{1};
    REQUIRE(empty.draw(r).empty());
}

TEST_CASE("pattern matching stays fast on a pathological pattern", "[pattern]")
{
    const auto p = core::pattern::parse("(#{0,16}){0,15}#");
    REQUIRE(p.lengths().second == 241);
    const auto start = std::chrono::steady_clock::now();
    REQUIRE(p.matches(std::string(241, '7')));
    REQUIRE(p.matches(std::string(1, '7')));
    REQUIRE_FALSE(p.matches(std::string(240, '7') + "A"));
    REQUIRE_FALSE(p.matches(std::string(200, '7') + "A" + std::string(40, '7')));
    REQUIRE_FALSE(p.matches(std::string(242, '7')));
    REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds{2});
}

TEST_CASE("pattern count equals brute-force enumeration", "[pattern]")
{
    // Every string over an alphabet, up to the longest length, that matches.
    const auto brute = [](const core::pattern& p, std::string_view alphabet) {
        std::uint64_t matched = 0;
        std::string s;
        const auto longest = p.lengths().second;
        const std::function<void()> walk = [&] {
            if (p.matches(s)) ++matched;
            if (s.size() == longest) return;
            for (const char c : alphabet)
            {
                s += c;
                walk();
                s.pop_back();
            }
        };
        walk();
        return matched;
    };
    const std::vector<std::tuple<std::string, std::string, std::uint64_t>> cases{
        {"[ab]{0,4}c{0,1}", "abc", 62},
        {"(x|y[01]){1,3}", "xy01", 39},
        {"(0|1(0|1)){2}", "01", 9},
        {"#{2}", "0123456789", 100},
        {"[A-C]{2,3}(-|\\.)[xy]", "ABC-.xy", 144},
    };
    for (const auto& [text, alphabet, expected] : cases)
    {
        INFO(text);
        const auto p = core::pattern::parse(text);
        REQUIRE(p.count() == expected);
        REQUIRE(brute(p, alphabet) == expected);
    }
    // Overlapping alternatives: ABC is AB + C and A + BC, so count() is an upper bound.
    const auto overlap = core::pattern::parse("(AB|A)(BC|C)");
    REQUIRE(overlap.count() == 4);
    REQUIRE(brute(overlap, "ABC") == 3);
    REQUIRE(overlap.lengths() == std::pair<std::size_t, std::size_t>{2, 4});
    REQUIRE(core::pattern::parse("@@-###").count() == 676000);
    REQUIRE(core::pattern::parse("*{64}").count() == std::numeric_limits<std::uint64_t>::max());
}

TEST_CASE("pattern draws are uniform", "[pattern]")
{
    const auto p = core::pattern::parse("[A-CEGHJ-PR-TW-Z]#");
    constexpr std::string_view letters = "ABCEGHJKLMNOPRSTWXYZ";
    std::vector<std::size_t> first(letters.size());
    std::vector<std::size_t> second(10);
    rng r{79};
    for (int i = 0; i < 100000; ++i)
    {
        const std::string s = p.draw(r);
        ++first.at(letters.find(s[0]));
        ++second.at(static_cast<std::size_t>(s[1] - '0'));
    }
    REQUIRE(chi_square_uniform(first) < 43.8);  // 19 degrees of freedom, p = 0.001
    REQUIRE(chi_square_uniform(second) < 27.9); // 9 degrees of freedom

    // Alternatives are equally likely whatever their size, and so are counts.
    const auto alt = core::pattern::parse("(A|BBBBB|#)");
    const auto counted = core::pattern::parse("#{0,3}");
    std::vector<std::size_t> alternatives(3);
    std::vector<std::size_t> lengths(4);
    for (int i = 0; i < 30000; ++i)
    {
        const std::string s = alt.draw(r);
        ++alternatives.at(s[0] == 'A' ? 0 : s[0] == 'B' ? 1 : 2);
        ++lengths.at(counted.draw(r).size());
    }
    REQUIRE(chi_square_uniform(alternatives) < 13.8); // 2 degrees of freedom
    REQUIRE(chi_square_uniform(lengths) < 16.3);      // 3 degrees of freedom
}

TEST_CASE("patterns compare by text", "[pattern]")
{
    const auto a = core::pattern::parse("##");
    const auto b = core::pattern::parse("#{2}");
    REQUIRE(a == core::pattern::parse("##"));
    REQUIRE_FALSE(a == b);
    REQUIRE(a.count() == b.count());
    REQUIRE(b.text() == "#{2}");
    const core::pattern copy = b; // NOLINT(performance-unnecessary-copy-initialization)
    rng r1{5};
    rng r2{5};
    REQUIRE(copy.draw(r1) == b.draw(r2));
}

// ---------------------------------------------------------------------------
// Money
// ---------------------------------------------------------------------------

TEST_CASE("money rounds major amounts half away from zero", "[money]")
{
    REQUIRE(core::money::from_major(0.125, "BRL", 2).minor == 13);
    REQUIRE(core::money::from_major(-0.125, "BRL", 2).minor == -13);
    REQUIRE(core::money::from_major(2.5, "JPY", 0).minor == 3);
    REQUIRE(core::money::from_major(1.005, "BRL", 2).minor == 100); // 1.00499999999999989...
    REQUIRE(core::money::from_major(0.49999999999999994, "JPY", 0).minor == 0);
    REQUIRE(core::money::from_major(1234.5, "KWD", 3).minor == 1234500);
    REQUIRE(core::money::from_major(0.00005, "CLF", 4).minor == 1);
    REQUIRE(core::money::from_major(-0.0, "BRL", 2).minor == 0);
    for (std::uint8_t d = 0; d <= 4; ++d)
    {
        const auto m = core::money::from_major(12.0, "XTS", d);
        REQUIRE(m.digits == d);
        REQUIRE(m.major() == 12.0);
    }
}

TEST_CASE("money checks its invariants", "[money]")
{
    constexpr std::int64_t limit = std::int64_t{1} << 53;
    REQUIRE(core::money::from_minor(limit, "USD", 2).minor == limit);
    REQUIRE(core::money::from_minor(-limit, "USD", 2).minor == -limit);
    REQUIRE_THROWS_WITH(core::money::from_minor(limit + 1, "USD", 2),
                        "money::from_minor: 9007199254740993 minor units is beyond 2^53");
    REQUIRE_THROWS_AS(core::money::from_minor(-limit - 1, "USD", 2), std::invalid_argument);
    REQUIRE(core::money::from_major(9007199254740992.0, "JPY", 0).minor == limit);
    REQUIRE_THROWS_WITH(core::money::from_major(9007199254740994.0, "JPY", 0),
                        "money::from_major: amount is beyond 2^53 minor units");
    REQUIRE_THROWS_WITH(core::money::from_major(std::numeric_limits<double>::quiet_NaN(), "USD", 2),
                        "money::from_major: amount is not finite");
    REQUIRE_THROWS_AS(core::money::from_major(std::numeric_limits<double>::infinity(), "USD", 2),
                      std::invalid_argument);
    for (const char* code : {"", "US", "USDX", "usd", "U5D", "US "})
    {
        INFO(code);
        REQUIRE_THROWS_AS(core::money::from_minor(1, code, 2), std::invalid_argument);
    }
    REQUIRE_THROWS_WITH(core::money::from_minor(1, "usd", 2),
                        "money::from_minor: currency 'usd' is not three capital letters");
    REQUIRE_THROWS_WITH(core::money::from_major(1.0, "USD", 5),
                        "money::from_major: 5 minor-unit digits, above 4");
}

TEST_CASE("money as decimal text and JSON", "[money]")
{
    REQUIRE(core::money::from_minor(123450, "BRL", 2).decimal() == "1234.50");
    REQUIRE(core::money::from_minor(-5, "USD", 2).decimal() == "-0.05");
    REQUIRE(core::money::from_minor(0, "USD", 2).decimal() == "0.00");
    REQUIRE(core::money::from_minor(1000, "JPY", 0).decimal() == "1000");
    REQUIRE(core::money::from_minor(-7, "JPY", 0).decimal() == "-7");
    REQUIRE(core::money::from_minor(125, "KWD", 3).decimal() == "0.125");
    REQUIRE(core::money::from_minor(5, "MGA", 1).decimal() == "0.5");
    REQUIRE(core::money::from_minor(-123456789, "CLF", 4).decimal() == "-12345.6789");
    REQUIRE(core::money::from_minor(std::int64_t{1} << 53, "USD", 2).decimal() == "90071992547409.92");

    const auto json = [](const core::money& m) {
        core::json_writer w;
        m.write_json(w);
        return w.str();
    };
    REQUIRE(json(core::money::from_minor(123450, "BRL", 2)) == R"({"amount":1234.5,"currency":"BRL"})");
    REQUIRE(json(core::money::from_minor(1000, "JPY", 0)) == R"({"amount":1000,"currency":"JPY"})");
    REQUIRE(json(core::money::from_minor(1234, "KWD", 3)) == R"({"amount":1.234,"currency":"KWD"})");
    REQUIRE(json(core::money::from_minor(-5, "USD", 2)) == R"({"amount":-0.05,"currency":"USD"})");

    core::json_writer w;
    w.begin_object().key("income");
    core::money::from_minor(123450, "BRL", 2).write_json(w);
    w.end_object();
    REQUIRE(core::flatten_json(w.str()) ==
            std::vector<std::pair<std::string, std::string>>{{"income.amount", "1234.5"},
                                                             {"income.currency", "BRL"}});
    REQUIRE(core::money::from_minor(1, "USD", 2) != core::money::from_minor(1, "USD", 3));
}

TEST_CASE("money read back from its JSON number is exact", "[money]")
{
    // Below 2^51 minor units, major() times 10^digits rounds back to minor.
    rng r{83};
    constexpr std::int64_t bound = std::int64_t{1} << 51;
    for (int i = 0; i < 100000; ++i)
    {
        const auto digits = static_cast<std::uint8_t>(core::bounded(r, 5));
        const std::int64_t minor = i % 2 == 0 ? core::uniform_int<std::int64_t>(r, -bound, bound)
                                              : core::uniform_int<std::int64_t>(r, -1000000, 1000000);
        const auto m = core::money::from_minor(minor, "XTS", digits);
        core::json_writer w;
        w.value(m.major());
        const double read = core::detail::parse_number<double>(w.str()).value();
        REQUIRE(core::money::from_major(read, "XTS", digits) == m);
    }
}

// ---------------------------------------------------------------------------
// Time-zone rules
// ---------------------------------------------------------------------------

namespace
{

std::chrono::sys_seconds utc(int y, unsigned m, unsigned d, int h = 0, int mi = 0, int s = 0)
{
    // In seconds throughout: MSVC's std::chrono::minutes holds 32 bits, too few for the year 9999.
    return std::chrono::sys_seconds{std::chrono::sys_days{std::chrono::year{y} / std::chrono::month{m} /
                                                          std::chrono::day{d}}} +
           std::chrono::seconds{(std::int64_t{h} * 3600) + (std::int64_t{mi} * 60) + s};
}

std::chrono::local_seconds local(int y, unsigned m, unsigned d, int h = 0, int mi = 0)
{
    return std::chrono::local_seconds{utc(y, m, d, h, mi).time_since_epoch()};
}

// The offset in seconds, the daylight flag and the abbreviation at an instant.
void require_at(const core::tz_rules& z, std::chrono::sys_seconds t, int offset, bool dst,
                std::string_view abbreviation)
{
    INFO(core::detail::iso_utc(t));
    const auto info = z.at(t);
    REQUIRE(info.offset.count() == offset);
    REQUIRE(info.dst == dst);
    REQUIRE(info.abbreviation == abbreviation);
}

} // namespace

TEST_CASE("time zones follow POSIX rules", "[tz]")
{
    const auto berlin = core::tz_rules::parse("0/3600/CET/s", "CET-1CEST,M3.5.0,M10.5.0/3");
    require_at(berlin, utc(2024, 3, 31, 0, 59, 59), 3600, false, "CET");
    require_at(berlin, utc(2024, 3, 31, 1), 7200, true, "CEST");
    require_at(berlin, utc(2024, 10, 27, 0, 59, 59), 7200, true, "CEST");
    require_at(berlin, utc(2024, 10, 27, 1), 3600, false, "CET");

    const auto new_york = core::tz_rules::parse("0/-18000/EST/s", "EST5EDT,M3.2.0,M11.1.0");
    require_at(new_york, utc(2024, 3, 10, 6, 59, 59), -18000, false, "EST");
    require_at(new_york, utc(2024, 3, 10, 7), -14400, true, "EDT");
    require_at(new_york, utc(2024, 11, 3, 5, 59, 59), -14400, true, "EDT");
    require_at(new_york, utc(2024, 11, 3, 6), -18000, false, "EST");

    // Southern hemisphere: daylight time across New Year.
    const auto sydney = core::tz_rules::parse("0/36000/AEST/s", "AEST-10AEDT,M10.1.0,M4.1.0/3");
    require_at(sydney, utc(2024, 4, 6, 15, 59, 59), 39600, true, "AEDT");
    require_at(sydney, utc(2024, 4, 6, 16), 36000, false, "AEST");
    require_at(sydney, utc(2024, 10, 5, 15, 59, 59), 36000, false, "AEST");
    require_at(sydney, utc(2024, 10, 5, 16), 39600, true, "AEDT");
    require_at(sydney, utc(2024, 12, 31, 23), 39600, true, "AEDT");
    require_at(sydney, utc(2025, 1, 1, 12), 39600, true, "AEDT");
    require_at(sydney, utc(2025, 4, 5, 16), 36000, false, "AEST");

    const auto sao_paulo = core::tz_rules::parse("0/-10800/-03/s", "<-03>3");
    require_at(sao_paulo, utc(2024, 1, 15), -10800, false, "-03");
    require_at(sao_paulo, utc(2024, 7, 15), -10800, false, "-03");
    REQUIRE_FALSE(sao_paulo.observes_dst_after(utc(2024, 1, 1)));

    // Extended hours: Jerusalem starts at 26:00 on a Thursday, Nuuk at -1:00.
    const auto jerusalem = core::tz_rules::parse("0/7200/IST/s", "IST-2IDT,M3.4.4/26,M10.5.0");
    require_at(jerusalem, utc(2024, 3, 28, 23, 59, 59), 7200, false, "IST");
    require_at(jerusalem, utc(2024, 3, 29), 10800, true, "IDT");
    require_at(jerusalem, utc(2024, 10, 26, 22, 59, 59), 10800, true, "IDT");
    require_at(jerusalem, utc(2024, 10, 26, 23), 7200, false, "IST");
    const auto nuuk = core::tz_rules::parse("0/-7200/-02/s", "<-02>2<-01>,M3.5.0/-1,M10.5.0/0");
    require_at(nuuk, utc(2024, 3, 31, 0, 59, 59), -7200, false, "-02");
    require_at(nuuk, utc(2024, 3, 31, 1), -3600, true, "-01");
    require_at(nuuk, utc(2024, 10, 27, 0, 59, 59), -3600, true, "-01");
    require_at(nuuk, utc(2024, 10, 27, 1), -7200, false, "-02");

    // Offsets with minutes and seconds, and an explicit daylight offset.
    const auto newfoundland = core::tz_rules::parse("0/-12600/NST/s", "NST3:30NDT2:30,M3.2.0,M11.1.0");
    require_at(newfoundland, utc(2024, 7, 1), -9000, true, "NDT");
    require_at(newfoundland, utc(2024, 1, 1), -12600, false, "NST");
    const auto seconds = core::tz_rules::parse("0/0/LMT/s", "<LMT>-0:01:15<LDT>-1:01:15,J1/0,J2/0");
    require_at(seconds, utc(2024, 7, 1), 75, false, "LMT");
    require_at(seconds, utc(2024, 1, 1, 12), 3675, true, "LDT");
}

TEST_CASE("time-zone rule dates in leap and common years", "[tz]")
{
    // Jn never counts February 29: J60 is March 1 in every year.
    const auto julian = core::tz_rules::parse("0/0/AAA/s", "AAA0BBB,J60/0,J61/0");
    require_at(julian, utc(2024, 2, 29, 23, 59, 59), 0, false, "AAA");
    require_at(julian, utc(2024, 3, 1), 3600, true, "BBB");
    require_at(julian, utc(2023, 2, 28, 23, 59, 59), 0, false, "AAA");
    require_at(julian, utc(2023, 3, 1), 3600, true, "BBB");
    // n counts it from 0: day 59 is February 29 in a leap year, March 1 otherwise.
    const auto ordinal = core::tz_rules::parse("0/0/AAA/s", "AAA0BBB,59/0,61/0");
    require_at(ordinal, utc(2024, 2, 28, 23, 59, 59), 0, false, "AAA");
    require_at(ordinal, utc(2024, 2, 29), 3600, true, "BBB");
    require_at(ordinal, utc(2023, 2, 28, 23, 59, 59), 0, false, "AAA");
    require_at(ordinal, utc(2023, 3, 1), 3600, true, "BBB");
    // M2.5.0: the last Sunday of February, the 29th in 2004.
    const auto last_week = core::tz_rules::parse("0/0/AAA/s", "AAA0BBB,M2.5.0/0,M3.1.0/0");
    REQUIRE(last_week.transitions(utc(2020, 1, 1), utc(2020, 12, 31)).front().first == utc(2020, 2, 23));
    REQUIRE(last_week.transitions(utc(2015, 1, 1), utc(2015, 12, 31)).front().first == utc(2015, 2, 22));
    REQUIRE(last_week.transitions(utc(2004, 1, 1), utc(2004, 12, 31)).front().first == utc(2004, 2, 29));

    // Daylight time all year (RFC 8536 3.3.1).
    const auto all_year = core::tz_rules::parse("0/-18000/EST/s", "EST5EDT,0/0,J365/25");
    require_at(all_year, utc(1970, 1, 1, 4, 59, 59), -18000, false, "EST");
    require_at(all_year, utc(1970, 1, 1, 5), -14400, true, "EDT");
    require_at(all_year, utc(2024, 1, 1), -14400, true, "EDT");
    require_at(all_year, utc(2024, 7, 1), -14400, true, "EDT");
    require_at(all_year, utc(2024, 12, 31, 23, 59, 59), -14400, true, "EDT");
    REQUIRE(all_year.transitions(utc(2020, 1, 1), utc(2025, 1, 1)).empty());
    REQUIRE(all_year.transitions(utc(1969, 1, 1), utc(1971, 1, 1)).size() == 1);
}

TEST_CASE("time-zone periods come before the POSIX rule", "[tz]")
{
    const auto berlin = core::tz_rules::parse(
        "946684800/3600/CET/s 954032400/7200/CEST/d 972781200/3600/CET/s",
        "CET-1CEST,M3.5.0,M10.5.0/3");
    REQUIRE(berlin.valid_from() == utc(2000, 1, 1));
    require_at(berlin, utc(2000, 1, 1), 3600, false, "CET");
    require_at(berlin, utc(2000, 10, 29, 0, 59, 59), 7200, true, "CEST");
    require_at(berlin, utc(2000, 10, 29, 1), 3600, false, "CET"); // the last period
    require_at(berlin, utc(2001, 3, 25, 0, 59, 59), 3600, false, "CET");
    require_at(berlin, utc(2001, 3, 25, 1), 7200, true, "CEST"); // the rule
    const auto changes = berlin.transitions(utc(1999, 1, 1), utc(2002, 1, 1));
    REQUIRE(changes.size() == 4);
    REQUIRE(changes[0].first == utc(2000, 3, 26, 1));
    REQUIRE(changes[0].second.abbreviation == "CEST");
    REQUIRE(changes[1].first == utc(2000, 10, 29, 1));
    REQUIRE(changes[2].first == utc(2001, 3, 25, 1));
    REQUIRE(changes[3].first == utc(2001, 10, 28, 1));
    REQUIRE(changes[3].second.abbreviation == "CET");

    // Before the last period the rule is not used at all.
    const auto frozen = core::tz_rules::parse("0/3600/CET/s 1735689600/3600/CET/s",
                                              "CET-1CEST,M3.5.0,M10.5.0/3");
    require_at(frozen, utc(2024, 7, 1), 3600, false, "CET");
    require_at(frozen, utc(2025, 7, 1), 7200, true, "CEST");
    // A rule change at the last period's start does not override the period.
    const auto listed = core::tz_rules::parse("0/3600/CET/s 1711846800/3600/XYZ/s",
                                              "CET-1CEST,M3.5.0,M10.5.0/3");
    require_at(listed, utc(2024, 4, 1), 3600, false, "XYZ");
    require_at(listed, utc(2024, 10, 27, 1), 3600, false, "CET");
    require_at(listed, utc(2025, 3, 30, 1), 7200, true, "CEST");

    // Daylight time abolished: São Paulo's last period is standard time.
    const auto sao_paulo = core::tz_rules::parse("1541300400/-7200/-02/d 1550368800/-10800/-03/s", "<-03>3");
    REQUIRE(sao_paulo.observes_dst_after(utc(2018, 12, 1)));
    REQUIRE_FALSE(sao_paulo.observes_dst_after(utc(2019, 3, 1)));
    REQUIRE(berlin.observes_dst_after(utc(2030, 1, 1)));
    require_at(sao_paulo, utc(2019, 2, 17, 1, 59, 59), -7200, true, "-02");
    require_at(sao_paulo, utc(2030, 1, 1), -10800, false, "-03");
}

TEST_CASE("local time to UTC through gaps and repeats", "[tz]")
{
    const auto berlin = core::tz_rules::parse("0/3600/CET/s", "CET-1CEST,M3.5.0,M10.5.0/3");
    REQUIRE(berlin.to_utc(local(2024, 7, 1, 12)) == utc(2024, 7, 1, 10));
    REQUIRE(berlin.to_utc(local(2024, 1, 15, 12)) == utc(2024, 1, 15, 11));
    REQUIRE(berlin.to_local(utc(2024, 7, 1, 10)) == local(2024, 7, 1, 12));
    // 02:30 on 2024-03-31 does not exist: read with the offset before the gap.
    REQUIRE(berlin.to_utc(local(2024, 3, 31, 2, 30)) == utc(2024, 3, 31, 1, 30));
    REQUIRE(berlin.to_utc(local(2024, 3, 31, 2, 30), core::tz_choose::latest) == utc(2024, 3, 31, 1, 30));
    // 02:30 on 2024-10-27 happens twice.
    REQUIRE(berlin.to_utc(local(2024, 10, 27, 2, 30)) == utc(2024, 10, 27, 0, 30));
    REQUIRE(berlin.to_utc(local(2024, 10, 27, 2, 30), core::tz_choose::latest) == utc(2024, 10, 27, 1, 30));
    REQUIRE(berlin.to_utc(local(2024, 10, 27, 3)) == utc(2024, 10, 27, 2));
    // Every 17 minutes of a year goes there and back.
    std::size_t failures = 0;
    for (auto t = utc(2024, 1, 1); t < utc(2025, 1, 1); t += std::chrono::minutes{17})
    {
        const auto l = berlin.to_local(t);
        if (berlin.to_utc(l) != t && berlin.to_utc(l, core::tz_choose::latest) != t) ++failures;
    }
    REQUIRE(failures == 0);
    // A western zone just after the start of its data.
    const auto new_york = core::tz_rules::parse("0/-18000/EST/s", "EST5EDT,M3.2.0,M11.1.0");
    REQUIRE(new_york.to_utc(local(1969, 12, 31, 20)) == utc(1970, 1, 1, 1));
}

TEST_CASE("time zones refuse instants outside their data", "[tz]")
{
    const auto berlin = core::tz_rules::parse("0/3600/CET/s", "CET-1CEST,M3.5.0,M10.5.0/3");
    REQUIRE_THROWS_WITH(berlin.at(utc(1969, 12, 31, 23, 59, 59)),
                        "tz_rules::at: 1969-12-31T23:59:59Z is before the zone's data, which starts at "
                        "1970-01-01T00:00:00Z");
    REQUIRE_THROWS_AS(berlin.to_local(utc(1969, 6, 1)), std::out_of_range);
    REQUIRE_THROWS_AS(berlin.to_utc(local(1969, 6, 1)), std::out_of_range);
    REQUIRE_THROWS_AS(berlin.to_utc(std::chrono::local_seconds{std::chrono::seconds::min()}),
                      std::out_of_range);
    REQUIRE_THROWS_AS(berlin.at(std::chrono::sys_seconds{std::chrono::seconds::max()}), std::out_of_range);
    REQUIRE_NOTHROW(berlin.at(utc(9999, 12, 31, 23, 59, 59)));
    REQUIRE_THROWS_WITH(berlin.at(utc(10000, 1, 1)),
                        "tz_rules::at: 10000-01-01T00:00:00Z is after the last supported instant, "
                        "9999-12-31T23:59:59Z");
    REQUIRE(berlin.transitions(utc(9999, 1, 1), utc(10001, 1, 1)).size() == 2);
    REQUIRE(berlin.transitions(utc(2024, 6, 1), utc(2024, 6, 1)).empty());
}

TEST_CASE("time zones round-trip through their text form", "[tz]")
{
    const auto berlin = core::tz_rules::parse(
        "946684800/3600/CET/s 954032400/7200/CEST/d 972781200/3600/CET/s",
        "CET-1CEST,M3.5.0,M10.5.0/3");
    REQUIRE(berlin.periods_text() == "946684800/3600/CET/s 954032400/7200/CEST/d 972781200/3600/CET/s");
    REQUIRE(berlin.posix() == "CET-1CEST,M3.5.0,M10.5.0/3");
    REQUIRE(core::tz_rules::parse(berlin.periods_text(), berlin.posix()) == berlin);
    const core::tz_rules copy = berlin; // NOLINT(performance-unnecessary-copy-initialization)
    REQUIRE(copy == berlin);
    REQUIRE(copy.at(utc(2024, 7, 1)).abbreviation == "CEST");
    REQUIRE_FALSE(core::tz_rules::parse(berlin.periods_text(), "") == berlin);

    const auto india = core::tz_rules::fixed(std::chrono::seconds{19800}, "+0530");
    REQUIRE(india.posix() == "<+0530>-5:30");
    REQUIRE(core::tz_rules::parse(india.periods_text(), india.posix()) == india);
    require_at(india, utc(-5000, 1, 1), 19800, false, "+0530");
    require_at(india, utc(2024, 7, 1), 19800, false, "+0530");
    REQUIRE_FALSE(india.observes_dst_after(utc(2024, 1, 1)));
    REQUIRE(core::tz_rules::fixed(std::chrono::seconds{-12600}, "-0330").posix() == "<-0330>3:30");
    const auto lmt = core::tz_rules::fixed(std::chrono::seconds{3723}, "LMT");
    REQUIRE(lmt.posix() == "<LMT>-1:02:03");
    REQUIRE(core::tz_rules::parse(lmt.periods_text(), lmt.posix()) == lmt);
    const core::tz_rules utc_zone;
    REQUIRE(utc_zone == core::tz_rules::fixed(std::chrono::seconds{0}, "UTC"));
    REQUIRE(utc_zone.posix() == "<UTC>0");
    require_at(utc_zone, utc(2024, 7, 1), 0, false, "UTC");
    REQUIRE_THROWS_AS(core::tz_rules::fixed(std::chrono::seconds{86400}, "XXX"), std::invalid_argument);
    REQUIRE_THROWS_AS(core::tz_rules::fixed(std::chrono::seconds{0}, "U"), std::invalid_argument);
}

TEST_CASE("time-zone parse errors give the position", "[tz]")
{
    const std::vector<std::pair<std::string, std::string>> periods{
        {"", "periods position 0: no period"},
        {"0/3600/CET", "periods position 0: a period has four fields (start/offset/abbreviation/s|d)"},
        {"0/3600/CET/s/x", "periods position 0: a period has four fields"},
        {"0/3600/CET/s ", "periods position 13: a period has four fields (start/offset/abbreviation/s|d)"},
        {"x/3600/CET/s", "periods position 0: expected a start in seconds"},
        {"0/+3600/CET/s", "periods position 2: expected an offset in seconds"},
        {"0/86400/CET/s", "periods position 2: offset beyond 86399 s"},
        {"0/3600/C/s", "periods position 7: an abbreviation is 3 to 6 of A-Z a-z 0-9 + -"},
        {"0/3600/CET/x", "periods position 11: expected s or d"},
        {"5/0/UTC/s 5/0/UTC/s", "periods position 10: starts must increase"},
        {"-999999999999999/0/UTC/s", "periods position 0: start outside the years -9999 to 9999"},
    };
    for (const auto& [text, message] : periods)
    {
        INFO(text);
        REQUIRE_THROWS_WITH(core::tz_rules::parse(text, ""), "tz_rules::parse: " + message);
    }
    const std::vector<std::pair<std::string, std::string>> rules{
        {"CET", "posix position 3: expected hours"},
        {"CE-1", "posix position 0: expected an abbreviation of 3 to 6 letters"},
        {"<AB>3", "posix position 0: a quoted abbreviation is 3 to 6 of A-Z a-z 0-9 + -"},
        {"<ABC3", "posix position 0: unclosed '<'"},
        {"CET-25", "posix position 4: hours above 24"},
        {"CET-1:60", "posix position 6: minutes above 59"},
        {"CET-1CEST", "posix position 9: daylight time needs its rules (,start,end)"},
        {"CET-1CEST,M3.5.0", "posix position 16: expected ','"},
        {"CET-1CEST,M13.5.0,M10.5.0", "posix position 11: a month above 12"},
        {"CET-1CEST,M3.6.0,M10.5.0", "posix position 13: a week above 5"},
        {"CET-1CEST,M3.0.0,M10.5.0", "posix position 10: months are 1 to 12 and weeks 1 to 5"},
        {"CET-1CEST,J0,J365", "posix position 10: a J day is 1 to 365"},
        {"CET-1CEST,366,J365", "posix position 10: a day above 365"},
        {"CET-1CEST,M3.5.0/168,M10.5.0", "posix position 17: hours above 167"},
        {"CET-1CEST,X,M10.5.0", "posix position 10: expected a rule date (Jn, n or Mm.w.d)"},
        {"CET-1CEST,M3.5.0,M10.5.0/3x", "posix position 26: unexpected character"},
    };
    for (const auto& [text, message] : rules)
    {
        INFO(text);
        REQUIRE_THROWS_WITH(core::tz_rules::parse("0/0/UTC/s", text), "tz_rules::parse: " + message);
    }
}
