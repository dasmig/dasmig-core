#include "catch_amalgamated.hpp"

#include "../dasmig/core.hpp"
#include "kat_vectors.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace core = dasmig::core;

namespace
{

// A stand-in for a library's seeded getter: one draw from the item's seed.
std::uint64_t first_draw(std::uint64_t seed)
{
    core::rng r{seed};
    return r();
}

std::uint64_t fnv_words(const std::vector<std::uint64_t>& words)
{
    std::uint64_t h = 0xcbf29ce484222325ULL;
    for (const auto w : words)
    {
        for (int i = 0; i < 8; ++i)
        {
            h ^= (w >> (8U * static_cast<unsigned>(i))) & 0xFFU;
            h *= 0x100000001b3ULL;
        }
    }
    return h;
}

struct record
{
    std::uint64_t value{0};
    std::string to_json() const
    {
        core::json_writer w;
        w.begin_object().field("value", value).field("name", "n" + std::to_string(value % 7)).end_object();
        return w.str();
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Item seeds
// ---------------------------------------------------------------------------

TEST_CASE("item_seed matches the reference implementation", "[batch][kat]")
{
    for (std::size_t i = 0; i < kat::item_index.size(); ++i)
    {
        REQUIRE(core::item_seed(7, kat::item_index[i]) == kat::item_seed_7[i]);
    }
    for (std::uint64_t k = 0; k < kat::attempt_seed_7_5.size(); ++k)
    {
        REQUIRE(core::attempt_seed(7, 5, k) == kat::attempt_seed_7_5[k]);
    }
    REQUIRE(core::attempt_seed(7, 5, 0) == core::item_seed(7, 5));
}

TEST_CASE("item_seed is the index child stream of the batch seed", "[batch]")
{
    for (std::uint64_t i : {0ULL, 1ULL, 99ULL, 123456789ULL})
    {
        REQUIRE(core::item_seed(42, i) == core::rng{42}.split(i).seed());
        REQUIRE(core::item_seed(42, i) == core::rng{42}.child_seed(i));
    }
    static_assert(core::item_seed(7, 0) == core::rng{7}.child_seed(std::uint64_t{0}));
}

TEST_CASE("first draws of 10,000 item seeds hash to the reference value", "[batch][kat]")
{
    const auto items = core::run_batch({.count = 10000}, 99, first_draw);
    REQUIRE(items.size() == 10000);
    REQUIRE(fnv_words(items) == kat::batch_99_hash_10k);
}

TEST_CASE("neighbouring item seeds give uncorrelated first draws", "[batch][stats]")
{
    // The one property option A relies on: item i and item i + 1 share nothing
    // but the batch seed. Chi-square on the top byte, and lag-1 correlation.
    constexpr std::uint64_t n = 1'000'000;
    std::array<std::uint64_t, 256> buckets{};
    double prev = 0.0;
    double sum_xy = 0.0;
    double sum_x = 0.0;
    double sum_x2 = 0.0;
    for (std::uint64_t i = 0; i < n; ++i)
    {
        const std::uint64_t draw = first_draw(core::item_seed(0x5EED, i));
        ++buckets[draw >> 56U];
        const double u = static_cast<double>(draw >> 11U) * 0x1.0p-53;
        sum_x += u;
        sum_x2 += u * u;
        if (i > 0) sum_xy += prev * u;
        prev = u;
    }
    const double expected = static_cast<double>(n) / 256.0;
    double chi2 = 0.0;
    for (const auto b : buckets)
    {
        const double d = static_cast<double>(b) - expected;
        chi2 += d * d / expected;
    }
    // 255 degrees of freedom: mean 255, sd about 22.6. Six sd is far in the tail.
    REQUIRE(chi2 < 255.0 + (6.0 * 22.6));
    REQUIRE(chi2 > 255.0 - (6.0 * 22.6));

    const double m = sum_x / static_cast<double>(n);
    const double var = (sum_x2 / static_cast<double>(n)) - (m * m);
    const double corr = ((sum_xy / static_cast<double>(n - 1)) - (m * m)) / var;
    REQUIRE(std::fabs(corr) < 5.0 / std::sqrt(static_cast<double>(n)));
}

// ---------------------------------------------------------------------------
// run_batch and for_each_batch
// ---------------------------------------------------------------------------

TEST_CASE("item i of a batch equals the single call with its seed", "[batch]")
{
    const auto items = core::run_batch({.count = 500}, 11, first_draw);
    REQUIRE(items.size() == 500);
    for (std::uint64_t i = 0; i < items.size(); ++i)
    {
        REQUIRE(items[i] == first_draw(core::item_seed(11, i)));
    }
}

TEST_CASE("batches concatenate", "[batch]")
{
    const auto whole = core::run_batch({.count = 2000}, 5, first_draw);
    auto head = core::run_batch({.count = 1000}, 5, first_draw);
    const auto tail = core::run_batch({.count = 1000, .first = 1000}, 5, first_draw);
    head.insert(head.end(), tail.begin(), tail.end());
    REQUIRE(head == whole);
}

TEST_CASE("the thread count does not change the output", "[batch]")
{
    const auto serial = core::run_batch({.count = 10000, .threads = 1}, 3, first_draw);
    for (const std::size_t threads : {2U, 7U, 16U, 0U, 100000U})
    {
        REQUIRE(core::run_batch({.count = 10000, .threads = threads}, 3, first_draw) == serial);
    }
    // More threads than items.
    REQUIRE(core::run_batch({.count = 3, .threads = 16}, 3, first_draw) ==
            core::run_batch({.count = 3, .threads = 1}, 3, first_draw));
}

TEST_CASE("an empty batch is empty", "[batch]")
{
    REQUIRE(core::run_batch({.count = 0, .threads = 4}, 1, first_draw).empty());
    bool called = false;
    core::for_each_batch({.count = 0}, 1, first_draw, [&](std::uint64_t, std::uint64_t&) { called = true; });
    REQUIRE_FALSE(called);
}

TEST_CASE("for_each_batch streams in index order and equals run_batch", "[batch]")
{
    // More than one window of 4096 items per thread, so windows are crossed.
    for (const std::size_t threads : {1U, 3U})
    {
        const core::batch_options options{.count = 30000, .first = 17, .threads = threads};
        const auto expected = core::run_batch(options, 21, first_draw);
        std::uint64_t next = 17;
        std::size_t seen = 0;
        core::for_each_batch(options, 21, first_draw,
                             [&](std::uint64_t index, std::uint64_t& value)
                             {
                                 REQUIRE(index == next);
                                 REQUIRE(value == expected[seen]);
                                 ++next;
                                 ++seen;
                             });
        REQUIRE(seen == expected.size());
    }
}

TEST_CASE("move-only items work", "[batch]")
{
    const auto make = [](std::uint64_t seed) { return std::make_unique<std::uint64_t>(first_draw(seed)); };
    const auto items = core::run_batch({.count = 100, .threads = 4}, 8, make);
    REQUIRE(items.size() == 100);
    REQUIRE(*items[42] == first_draw(core::item_seed(8, 42)));
}

TEST_CASE("the failure of the lowest index is the one reported", "[batch][errors]")
{
    const auto make = [](std::uint64_t seed) -> std::uint64_t
    {
        for (const std::uint64_t bad : {300ULL, 700ULL})
        {
            if (seed == core::item_seed(4, bad)) throw std::runtime_error("item " + std::to_string(bad));
        }
        return seed;
    };
    for (const std::size_t threads : {1U, 2U, 4U, 8U})
    {
        try
        {
            (void)core::run_batch({.count = 1000, .threads = threads}, 4, make);
            FAIL("expected a failure");
        }
        catch (const std::runtime_error& e)
        {
            REQUIRE(std::string{e.what()} == "item 300");
        }
    }
}

TEST_CASE("first + count must not overflow", "[batch][errors]")
{
    REQUIRE_THROWS_AS(core::run_batch({.count = 2, .first = UINT64_MAX}, 1, first_draw), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// Uniqueness
// ---------------------------------------------------------------------------

namespace
{

// A small space on purpose: 40 items drawn from 64 values collide a lot.
std::uint64_t small_value(std::uint64_t seed)
{
    core::rng r{seed};
    return core::bounded(r, 64);
}

core::unique_rule by_value(std::size_t attempts = 100)
{
    return core::unique_rule::by<std::uint64_t>([](const std::uint64_t& v) { return std::to_string(v); }, attempts);
}

} // namespace

TEST_CASE("a unique batch retries collisions and is the same for any thread count", "[batch][unique]")
{
    core::batch_options options{.count = 40, .threads = 1, .unique = by_value()};
    const auto serial = core::run_batch(options, 6, small_value);
    REQUIRE(serial.size() == 40);
    REQUIRE(std::set<std::uint64_t>(serial.begin(), serial.end()).size() == 40);

    // Without the rule the same space repeats, so the rule really did retries.
    const auto plain = core::run_batch({.count = 40}, 6, small_value);
    REQUIRE(std::set<std::uint64_t>(plain.begin(), plain.end()).size() < 40);

    for (const std::size_t threads : {2U, 5U, 16U})
    {
        options.threads = threads;
        REQUIRE(core::run_batch(options, 6, small_value) == serial);
    }
}

TEST_CASE("a unique batch keeps items that do not collide as the single call gives them", "[batch][unique]")
{
    const core::batch_options options{.count = 40, .unique = by_value()};
    const auto unique = core::run_batch(options, 6, small_value);
    std::set<std::uint64_t> seen;
    for (std::uint64_t i = 0; i < unique.size(); ++i)
    {
        const std::uint64_t plain = small_value(core::item_seed(6, i));
        if (seen.insert(plain).second)
        {
            REQUIRE(unique[i] == plain);
        }
        else
        {
            // A retried item is the first attempt of its own that did not collide.
            bool matched = false;
            for (std::uint64_t k = 1; k < 100 && !matched; ++k)
            {
                matched = unique[i] == small_value(core::attempt_seed(6, i, k));
            }
            REQUIRE(matched);
            seen.insert(unique[i]);
        }
    }
}

TEST_CASE("a unique batch streams the same items", "[batch][unique]")
{
    const core::batch_options options{.count = 50, .threads = 3, .unique = by_value()};
    const auto expected = core::run_batch(options, 9, small_value);
    std::vector<std::uint64_t> streamed;
    core::for_each_batch(options, 9, small_value,
                         [&](std::uint64_t, std::uint64_t& v) { streamed.push_back(v); });
    REQUIRE(streamed == expected);
}

TEST_CASE("a unique batch fails with the item named when attempts run out", "[batch][unique][errors]")
{
    // Only 64 values exist, so 100 unique items cannot be found.
    const core::batch_options options{.count = 100, .unique = by_value(20)};
    try
    {
        (void)core::run_batch(options, 6, small_value);
        FAIL("expected a failure");
    }
    catch (const std::runtime_error& e)
    {
        const std::string message = e.what();
        REQUIRE(message.find("item ") != std::string::npos);
        REQUIRE(message.find("20 attempts") != std::string::npos);
    }

    // The same item fails for any thread count.
    std::string first_message;
    for (const std::size_t threads : {1U, 4U})
    {
        core::batch_options o = options;
        o.threads = threads;
        try
        {
            (void)core::run_batch(o, 6, small_value);
        }
        catch (const std::runtime_error& e)
        {
            if (first_message.empty()) first_message = e.what();
            REQUIRE(first_message == e.what());
        }
    }
}

TEST_CASE("a unique rule for another type or with no attempts is rejected", "[batch][unique][errors]")
{
    const auto wrong = core::unique_rule::by<std::string>([](const std::string& s) { return s; });
    REQUIRE_THROWS_AS(core::run_batch({.count = 1, .unique = wrong}, 1, first_draw), std::invalid_argument);
    REQUIRE_THROWS_AS(core::run_batch({.count = 1, .unique = by_value(0)}, 1, first_draw),
                      std::invalid_argument);
}

// ---------------------------------------------------------------------------
// JSON Lines and CSV
// ---------------------------------------------------------------------------

TEST_CASE("jsonl_writer writes one document per line", "[batch][sinks]")
{
    std::ostringstream out;
    core::jsonl_writer writer{out};
    core::for_each_batch({.count = 3}, 1, [](std::uint64_t seed) { return record{seed % 10}; }, writer);
    REQUIRE(writer.lines() == 3);

    std::istringstream in{out.str()};
    std::string line;
    std::size_t lines = 0;
    while (std::getline(in, line))
    {
        const auto cells = core::flatten_json(line);
        REQUIRE(cells.size() == 2);
        REQUIRE(cells[0].first == "value");
        ++lines;
    }
    REQUIRE(lines == 3);
    REQUIRE_THROWS_AS(writer.write_line("{\"a\":\n1}"), std::invalid_argument);
}

TEST_CASE("flatten_json flattens objects, arrays and scalars", "[batch][sinks]")
{
    using cells = std::vector<std::pair<std::string, std::string>>;
    REQUIRE(core::flatten_json(R"({"a":1,"b":{"c":"x","d":{"e":true}},"f":[1,{"g":2}],"h":null,"i":-2.5e3})") ==
            cells{{"a", "1"},
                  {"b.c", "x"},
                  {"b.d.e", "true"},
                  {"f", R"([1,{"g":2}])"},
                  {"h", ""},
                  {"i", "-2.5e3"}});
    REQUIRE(core::flatten_json(R"({"o":{},"p":[]})") == cells{{"o", "{}"}, {"p", "[]"}});
    REQUIRE(core::flatten_json("{}").empty());
    REQUIRE(core::flatten_json("42") == cells{{"", "42"}});
    REQUIRE(core::flatten_json(R"( { "a" : [ 1 , 2 ] } )") == cells{{"a", "[ 1 , 2 ]"}});
}

TEST_CASE("flatten_json unescapes strings", "[batch][sinks]")
{
    using cells = std::vector<std::pair<std::string, std::string>>;
    // Normal literals with doubled backslashes: the text holds a backslash
    // and a 'u', not a universal character name.
    REQUIRE(core::flatten_json("{\"s\":\"a\\\"b\\\\c\\/d\\n\\t\\u00e9\\u20ac\\ud83d\\ude00\"}") ==
            cells{{"s", "a\"b\\c/d\n\t\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"}});
    // Brackets and quotes inside strings do not confuse array scanning.
    const std::string array = "[\"]\\\"[\",{\"k\":\"}\"}]";
    REQUIRE(core::flatten_json("{\"a\":" + array + "}") == cells{{"a", array}});
}

TEST_CASE("flatten_json reads what json_writer writes", "[batch][sinks]")
{
    core::json_writer w;
    const std::string nasty = "q\"b\\n\n\r\t\x01 \xc3\xa9\xf0\x9f\x98\x80 <>&";
    w.begin_object().field("text", nasty).key("n").begin_object().field("x", 1.5).field("y", -3).end_object().end_object();
    const auto cells = core::flatten_json(w.str());
    REQUIRE(cells.size() == 3);
    REQUIRE(cells[0].second == nasty);
    REQUIRE(cells[1] == std::pair<std::string, std::string>{"n.x", "1.5"});
    REQUIRE(cells[2] == std::pair<std::string, std::string>{"n.y", "-3"});
}

TEST_CASE("flatten_json rejects malformed JSON", "[batch][sinks][errors]")
{
    for (const char* bad : {"", "{", R"({"a"})", R"({"a":})", R"({"a":1,})", R"({"a":1} x)", R"({"a":"\x"})",
                            R"({"a":"\u12"})", "{\"a\":\"\\ud83d\\u0041\"}", R"({"a":[1,)", R"({"a":"unterminated)"})
    {
        INFO(bad);
        REQUIRE_THROWS_AS(core::flatten_json(bad), std::invalid_argument);
    }
}

TEST_CASE("csv_writer writes a header from the first record", "[batch][sinks]")
{
    std::ostringstream out;
    core::csv_writer writer{out};
    writer.write_json(R"({"id":1,"person":{"name":"Ana, \"A\"","tags":["x","y"]},"note":null})");
    writer.write_json(R"({"id":2,"person":{"name":"Bo","tags":[]},"note":"line1\nline2"})");
    REQUIRE(writer.records() == 2);
    REQUIRE(writer.columns() == std::vector<std::string>{"id", "person.name", "person.tags", "note"});
    REQUIRE(out.str() ==
            "id,person.name,person.tags,note\n"
            "1,\"Ana, \"\"A\"\"\",\"[\"\"x\"\",\"\"y\"\"]\",\n"
            "2,Bo,[],\"line1\nline2\"\n");
}

TEST_CASE("csv_writer matches later records to the header by name", "[batch][sinks]")
{
    std::ostringstream out;
    core::csv_writer writer{out};
    writer.write_json(R"({"a":1,"b":2,"c":3})");
    writer.write_json(R"({"c":30,"a":10})");           // reordered and missing b
    writer.write_json(R"({"a":100,"z":9,"b":200,"c":300})"); // extra column dropped
    REQUIRE(out.str() == "a,b,c\n1,2,3\n10,,30\n100,200,300\n");
}

TEST_CASE("csv_writer is a batch sink", "[batch][sinks]")
{
    std::ostringstream out;
    core::csv_writer writer{out};
    core::for_each_batch({.count = 4, .threads = 2}, 2, [](std::uint64_t seed) { return record{seed % 100}; },
                         writer);
    std::istringstream in{out.str()};
    std::string line;
    std::getline(in, line);
    REQUIRE(line == "value,name");
    std::size_t rows = 0;
    while (std::getline(in, line)) ++rows;
    REQUIRE(rows == 4);
}
