// Tests for the data parts of the core: utf8, json, tsv, weighted_table,
// seed_signature, layered_table and resources.

#include "catch_amalgamated.hpp"

#include "../dasmig/core.hpp"
#include "embedded_fixture.hpp"

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using dasmig::core::rng;
namespace core = dasmig::core;

// ---------------------------------------------------------------------------
// utf8
// ---------------------------------------------------------------------------

TEST_CASE("utf8 round-trips 1- to 4-byte characters", "[utf8]")
{
    const std::string text = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x98\x80"; // a, e-acute, euro, emoji
    REQUIRE(core::is_utf8(text));
    const std::wstring wide = core::from_utf8(text);
    REQUIRE(wide.size() == (sizeof(wchar_t) == 2 ? 5U : 4U));
    REQUIRE(core::to_utf8(wide) == text);
    REQUIRE(core::to_utf8(L"José") == "Jos\xc3\xa9");
    REQUIRE(core::from_utf8("").empty());
}

TEST_CASE("utf8 rejects malformed input", "[utf8][errors]")
{
    REQUIRE(core::utf8_invalid_offset("ok\x80") == 2);  // stray continuation byte
    REQUIRE_FALSE(core::is_utf8("\xc0\x80"));           // overlong NUL
    REQUIRE_FALSE(core::is_utf8("\xed\xa0\x80"));       // surrogate
    REQUIRE_FALSE(core::is_utf8("\xf4\x90\x80\x80"));   // above U+10FFFF
    REQUIRE_FALSE(core::is_utf8("\xe2\x82"));           // truncated
    REQUIRE_FALSE(core::is_utf8("\xff"));
    REQUIRE_THROWS_AS(core::from_utf8("\xc3"), std::invalid_argument);
    const std::wstring lone_surrogate(1, static_cast<wchar_t>(0xD800));
    REQUIRE_THROWS_AS(core::to_utf8(lone_surrogate), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// json
// ---------------------------------------------------------------------------

TEST_CASE("json_writer writes nested documents compactly", "[json]")
{
    core::json_writer w;
    w.begin_object()
        .field("name", "Ana")
        .field("age", 34)
        .field("height", 167.5)
        .field("alive", true)
        .key("spouse").value(nullptr)
        .key("tags").begin_array().value("calm").value(std::string{"curious"}).end_array()
        .key("empty").begin_object().end_object()
        .key("none").begin_array().end_array()
        .end_object();
    REQUIRE(w.complete());
    REQUIRE(w.str() == R"({"name":"Ana","age":34,"height":167.5,"alive":true,"spouse":null,)"
                       R"("tags":["calm","curious"],"empty":{},"none":[]})");
}

TEST_CASE("json_writer escapes strings and formats numbers exactly", "[json]")
{
    std::string out;
    core::json_writer::escape(out, "q\"b\\n\nt\tc\x01\x1f\xc3\xa9/");
    REQUIRE(out == "\"q\\\"b\\\\n\\nt\\tc\\u0001\\u001f\xc3\xa9/\"");

    auto one = [](auto v) {
        core::json_writer w;
        w.value(v);
        return w.str();
    };
    REQUIRE(one(0.1) == "0.1");
    REQUIRE(one(1e21) == "1e+21");
    REQUIRE(one(-0.0) == "-0");
    REQUIRE(one(2.5) == "2.5");
    REQUIRE(one(std::numeric_limits<std::int64_t>::min()) == "-9223372036854775808");
    REQUIRE(one(std::numeric_limits<std::uint64_t>::max()) == "18446744073709551615");
    REQUIRE(one(false) == "false");
    REQUIRE(one("x") == "\"x\""); // a literal is a string, not a bool
}

TEST_CASE("json_writer::escape matches a byte-at-a-time escape", "[json]")
{
    // The escape as first written, one byte at a time.
    const auto reference = [](std::string_view text) {
        std::string out = "\"";
        for (const char c : text)
        {
            const auto b = static_cast<std::uint8_t>(c);
            switch (c)
            {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (b < 0x20U) out += std::format("\\u{:04x}", b);
                else out += c;
            }
        }
        return out + "\"";
    };
    // Appends to what is already in the string.
    const auto escaped = [](std::string_view text) {
        std::string out = "prefix";
        core::json_writer::escape(out, text);
        REQUIRE(out.starts_with("prefix"));
        return out.substr(6);
    };
    REQUIRE(escaped("") == "\"\"");
    for (int a = 0; a < 0x80; ++a)
    {
        const std::string one(1, static_cast<char>(a));
        REQUIRE(escaped(one) == reference(one));
        for (int b = 0; b < 0x80; ++b)
        {
            const std::string text = "ab" + one + std::string(1, static_cast<char>(b)) + "\xc3\xa9z";
            REQUIRE(escaped(text) == reference(text));
            REQUIRE(escaped(one + std::string(1, static_cast<char>(b))) == reference(one + std::string(1, static_cast<char>(b))));
        }
    }
}

TEST_CASE("json_writer rejects misuse", "[json][errors]")
{
    core::json_writer a;
    REQUIRE_THROWS_AS(a.key("k"), std::logic_error);
    REQUIRE_THROWS_AS(a.str(), std::logic_error);
    a.begin_object();
    REQUIRE_THROWS_AS(a.value(1), std::logic_error);     // value without a key
    REQUIRE_THROWS_AS(a.end_array(), std::logic_error);
    a.key("k");
    REQUIRE_THROWS_AS(a.key("again"), std::logic_error);
    REQUIRE_THROWS_AS(a.end_object(), std::logic_error); // key without a value
    a.value(1).end_object();
    REQUIRE(a.str() == R"({"k":1})");
    REQUIRE_THROWS_AS(a.value(2), std::logic_error);     // second top-level value

    core::json_writer b;
    REQUIRE_THROWS_AS(b.value(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
    REQUIRE_THROWS_AS(b.value(std::numeric_limits<double>::infinity()), std::invalid_argument);
    REQUIRE_THROWS_AS(b.value(std::string_view{"\xff"}), std::invalid_argument);
}

// ---------------------------------------------------------------------------
// tsv
// ---------------------------------------------------------------------------

TEST_CASE("tsv parses a header, rows, comments and short rows", "[tsv]")
{
    const auto doc = core::tsv_document::parse("code\tname\theight\tlangs\n"
                                               "# a comment\n"
                                               "\n"
                                               "BR\tBrazil\t173.6\tPortuguese\n"
                                               "JP\tJapan\t+172\tJapanese;Ainu\n"
                                               "XX\tNowhere\n",
                                               "countries.tsv");
    REQUIRE(doc.columns() == std::vector<std::string>{"code", "name", "height", "langs"});
    REQUIRE(doc.size() == 3);
    const auto jp = doc.row(1);
    REQUIRE(jp.line() == 5);
    REQUIRE(jp["name"] == "Japan");
    REQUIRE(jp.get<double>("height") == 172.0);
    REQUIRE(jp.list("langs") == std::vector<std::string_view>{"Japanese", "Ainu"});
    const auto xx = doc.row(2);
    REQUIRE(xx["height"].empty());
    REQUIRE_FALSE(xx.has("height"));
    REQUIRE(xx.get_or("height", 1.5) == 1.5);
    REQUIRE(xx.list("langs").empty());
    REQUIRE(doc.row(0).get<double>("height") == 173.6);
    REQUIRE(doc.column_index("name") == std::size_t{1});
    REQUIRE_FALSE(doc.has_column("missing"));
    REQUIRE_THROWS_AS(doc.row(3), std::out_of_range);
}

TEST_CASE("tsv converts typed cells exactly and reports bad ones", "[tsv]")
{
    const auto doc = core::tsv_document::parse("i\td\tb\ts\n"
                                               "-42\t0.1\t1\ttext\n"
                                               "1.5\tinf\tyes\t\n"
                                               "x\t1e-300\tfalse\t\n",
                                               "typed.tsv");
    const auto r0 = doc.row(0);
    REQUIRE(r0.get<int>("i") == -42);
    REQUIRE(r0.get<double>("d") == 0.1);
    REQUIRE(r0.get<bool>("b"));
    REQUIRE(r0.get<std::string>("s") == "text");
    REQUIRE(r0.get<std::string_view>("s") == "text");
    REQUIRE(doc.row(2).get<double>("d") == 1e-300);
    REQUIRE_FALSE(doc.row(2).get<bool>("b"));

    const auto r1 = doc.row(1);
    REQUIRE_THROWS_WITH(r1.get<int>("i"), "typed.tsv:3: column 'i': expected a number, got '1.5'");
    REQUIRE_THROWS_AS(r1.get<double>("d"), core::tsv_error); // not finite
    REQUIRE_THROWS_AS(r1.get<bool>("b"), core::tsv_error);
    REQUIRE_THROWS_WITH(r1.get<std::string>("s"), "typed.tsv:3: column 's': missing value");
    REQUIRE_THROWS_WITH(r1["nope"], "typed.tsv:3: column 'nope': no such column");
    try
    {
        (void)doc.row(2).get<int>("i");
        FAIL("expected tsv_error");
    }
    catch (const core::tsv_error& e)
    {
        REQUIRE(e.source() == "typed.tsv");
        REQUIRE(e.line() == 4);
        REQUIRE(e.column() == "i");
    }
}

TEST_CASE("tsv parses decimals to the same doubles as a correctly rounded parser", "[tsv]")
{
    // The reference: strtod in the C locale, correctly rounded on every
    // platform the core supports.
    auto reference = [](const std::string& text) {
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        REQUIRE(end == text.c_str() + text.size());
        return std::bit_cast<std::uint64_t>(value);
    };
    auto parsed = [](const std::string& text) {
        const auto value = core::detail::parse_number<double>(text);
        REQUIRE(value);
        return std::bit_cast<std::uint64_t>(*value);
    };

    // The fast path takes these; the last ones are at its limits.
    for (const std::string text : {"0.111803", "173.6", "-0.0", "0", "007", ".5", "5.", "-.5", "1.e3",
                                   "1E+05", "00012.5000", "2.5e-3", "9007199254740992", "1e22", "1e-22",
                                   "9007199254740991e-22"})
    {
        INFO(text);
        REQUIRE(core::detail::parse_decimal_fast(text));
        REQUIRE(parsed(text) == reference(text));
    }
    // These go to the general parser: too many digits, past 2^53, past 10^22.
    for (const std::string text : {"9007199254740993", "12345678901234567890", "1e23", "1e-23",
                                   "123456789012345678e-22", "1e300", "0.1000000000000000055511151231257827"})
    {
        INFO(text);
        REQUIRE_FALSE(core::detail::parse_decimal_fast(text));
        REQUIRE(parsed(text) == reference(text));
    }
    for (const std::string text : {"", "-", ".", "-.", "1.2.3", "1e", "1e+", "+-1", "--1", "1 ", " 1", "1,5",
                                   "e5", "0x10"})
    {
        INFO(text);
        REQUIRE_FALSE(core::detail::parse_decimal_fast(text));
    }
    REQUIRE_FALSE(core::detail::parse_number<double>("1e10000")); // overflows: not finite

    // Random decimals: 1 to 19 digits, any point position, optional sign and exponent.
    rng r{20261015};
    for (int n = 0; n < 50000; ++n)
    {
        std::string text = core::bernoulli(r, 0.3) ? "-" : "";
        const auto digits = core::uniform_int(r, 1, 19);
        const auto point = core::uniform_int(r, 0, digits);
        for (int d = 0; d < digits; ++d)
        {
            if (d == point && core::bernoulli(r, 0.8)) text += '.';
            text += static_cast<char>('0' + core::uniform_int(r, 0, 9));
        }
        if (core::bernoulli(r, 0.3)) text += "e" + std::to_string(core::uniform_int(r, -40, 40));
        INFO(text);
        REQUIRE(parsed(text) == reference(text));
    }
}

TEST_CASE("tsv rejects malformed files with their location", "[tsv][errors]")
{
    REQUIRE_THROWS_WITH(core::tsv_document::parse("a\tb\n1\t2\t3\n", "x.tsv"),
                        "x.tsv:2: row has 3 cells but there are 2 columns");
    REQUIRE_THROWS_WITH(core::tsv_document::parse("a\ta\n", "x.tsv"),
                        "x.tsv:1: column 'a': column name repeated");
    REQUIRE_THROWS_AS(core::tsv_document::parse("a\t\tc\n", "x.tsv"), core::tsv_error);
    REQUIRE_THROWS_WITH(core::tsv_document::parse("# only a comment\n", "x.tsv"),
                        "x.tsv: no header line");
    REQUIRE_THROWS_WITH(core::tsv_document::parse("a\nok\n\xff\n", "x.tsv"),
                        "x.tsv:3: invalid UTF-8");
    REQUIRE_THROWS_AS(core::tsv_document::load("does/not/exist.tsv"), core::tsv_error);
    REQUIRE_THROWS_AS(core::tsv_document::parse("w\n", "x", std::vector<std::string>{}),
                      core::tsv_error);
}

TEST_CASE("tsv content hash ignores line ends, BOM and comments", "[tsv]")
{
    const auto lf = core::tsv_document::parse("a\tb\n1\t2\n", "lf");
    const auto crlf =
        core::tsv_document::parse("\xEF\xBB\xBF" "a\tb\r\n# note\r\n1\t2\r\n", "crlf");
    const auto other = core::tsv_document::parse("a\tb\n1\t3\n", "other");
    REQUIRE(lf.content_hash() == crlf.content_hash());
    REQUIRE(lf.content_hash() != other.content_hash());
    REQUIRE(crlf.row(0)["b"] == "2");
}

TEST_CASE("tsv headerless documents, files, and rows outliving the original", "[tsv]")
{
    const auto words = core::tsv_document::parse("Deer\nOtter\n", "animals.words", {"word"});
    REQUIRE(words.size() == 2);
    REQUIRE(words.row(1)["word"] == "Otter");

    const auto path = std::filesystem::temp_directory_path() / "dasmig_core_test.tsv";
    {
        std::ofstream out{path, std::ios::binary};
        out << "k\tv\r\nA\t1\r\n";
    }
    std::optional<core::tsv_document> keep;
    std::vector<core::tsv_row> rows;
    {
        const auto doc = core::tsv_document::load(path);
        REQUIRE(doc.source() == path.generic_string());
        keep = doc; // a copy shares the data
        rows = doc.rows();
    } // the original is gone; the copy keeps the rows valid
    REQUIRE(rows[0].get<int>("v") == 1);
    REQUIRE(rows[0]["k"] == "A");
    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// weighted_table
// ---------------------------------------------------------------------------

TEST_CASE("weighted_table picks exactly like discrete", "[weighted_table]")
{
    const core::weighted_table<char> table{{'a', 0.5}, {'b', 0.0}, {'c', 2.25}, {'d', 1.0}, {'e', 0.25}};
    const core::discrete d{0.5, 0.0, 2.25, 1.0, 0.25};
    rng r1{19};
    rng r2{19};
    for (int i = 0; i < 1000; ++i)
    {
        REQUIRE(table.pick_index(r1) == d(r2));
    }
    REQUIRE(r1 == r2);
    REQUIRE(table.total_weight() == 4.0);
    REQUIRE(table.size() == 5);
}

TEST_CASE("weighted_table: an identity adjustment picks as pick_index, small and large", "[weighted_table]")
{
    // try_pick_index keeps up to 64 sums on the stack and the rest on the heap.
    for (const std::size_t size : {1U, 10U, 64U, 65U, 200U})
    {
        CAPTURE(size);
        core::weighted_table<std::size_t> table;
        for (std::size_t i = 0; i < size; ++i) table.add(i, i % 7 == 3 ? 0.0 : 0.5 + static_cast<double>(i % 5));
        rng r1{size};
        rng r2{size};
        for (int i = 0; i < 2000; ++i)
        {
            REQUIRE(table.try_pick_index(r1, [](std::size_t, double w) { return w; }) == table.pick_index(r2));
        }
        REQUIRE(r1 == r2);
    }
}

TEST_CASE("weighted_table adjusts weights per call and excludes", "[weighted_table]")
{
    core::weighted_table<std::string> jobs;
    jobs.add("farmer", 3.0).add("nurse", 2.0).add("pilot", 1.0);
    rng r{5};
    const int age = 16;
    for (int i = 0; i < 2000; ++i)
    {
        const auto& job = jobs.pick(r, [&](const std::string& j, double w) {
            return (j == "pilot" && age < 21) ? 0.0 : w;
        });
        REQUIRE(job != "pilot");
    }

    // Doubling a weight shifts the frequencies accordingly.
    std::size_t nurses = 0;
    constexpr int n = 200000;
    for (int i = 0; i < n; ++i)
    {
        const auto& job = jobs.pick(r, [](const std::string& j, double w) {
            return j == "nurse" ? w * 2.0 : w;
        });
        nurses += job == "nurse" ? 1U : 0U;
    }
    REQUIRE(static_cast<double>(nurses) / n == Catch::Approx(4.0 / 8.0).margin(0.005));
}

TEST_CASE("weighted_table with nothing eligible", "[weighted_table][errors]")
{
    const core::weighted_table<int> table{{1, 1.0}, {2, 1.0}};
    auto none = [](int, double) { return 0.0; };
    rng r{3};
    rng reference{3};
    REQUIRE(table.try_pick(r, none) == nullptr);
    (void)reference();
    REQUIRE(r == reference); // still consumed exactly one value
    REQUIRE_THROWS_AS(table.pick(r, none), std::invalid_argument);
    REQUIRE_THROWS_AS(table.pick(r, [](int, double) { return -1.0; }), std::invalid_argument);

    const core::weighted_table<int> zero{{1, 0.0}};
    REQUIRE_THROWS_AS(zero.pick(r), std::invalid_argument);
    core::weighted_table<int> bad;
    REQUIRE_THROWS_AS(bad.add(1, -1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(bad.add(1, std::numeric_limits<double>::infinity()), std::invalid_argument);
    REQUIRE(bad.empty());
}

TEST_CASE("weighted_table picks distinct entries", "[weighted_table]")
{
    const core::weighted_table<std::string> hobbies{
        {"chess", 1.0}, {"running", 3.0}, {"painting", 2.0}, {"fishing", 0.0}};
    rng r{11};
    for (int i = 0; i < 500; ++i)
    {
        const auto picked = hobbies.pick_distinct_indices(r, 2);
        REQUIRE(picked.size() == 2);
        REQUIRE(picked[0] != picked[1]);
        REQUIRE(picked[0] != 3);
        REQUIRE(picked[1] != 3);
    }
    // Only three entries have weight: asking for four gives three, and still
    // consumes four values.
    rng a{2};
    rng b{2};
    REQUIRE(hobbies.pick_distinct_indices(a, 4).size() == 3);
    for (int i = 0; i < 4; ++i) (void)b();
    REQUIRE(a == b);
}

// ---------------------------------------------------------------------------
// seed_signature
// ---------------------------------------------------------------------------

TEST_CASE("seed_signature text and JSON forms", "[seed_signature]")
{
    const core::seed_signature sig{.seed = 1234, .data = 0x9ae16a3b2f90404fULL};
    REQUIRE(sig.to_string() == "00000000000004d2-9ae16a3b2f90404f");
    REQUIRE(core::seed_signature::parse(sig.to_string()) == sig);
    REQUIRE(core::seed_signature::parse("00000000000004d2") ==
            core::seed_signature{.seed = 1234, .data = 0});
    const core::seed_signature max{.seed = std::numeric_limits<std::uint64_t>::max(), .data = 0};
    REQUIRE(core::seed_signature::parse(max.to_string()) == max);

    core::json_writer w;
    sig.to_json(w);
    REQUIRE(w.str() == R"({"seed":"00000000000004d2","data":"9ae16a3b2f90404f"})");

    for (const char* bad : {"", "4d2", "00000000000004D2", "00000000000004d2-", "00000000000004d2-1",
                            "0000000000004d2x-9ae16a3b2f90404f", "00000000000004d2-9ae16a3b2f90404f-0"})
    {
        INFO(bad);
        REQUIRE_THROWS_AS(core::seed_signature::parse(bad), std::invalid_argument);
    }
}

// ---------------------------------------------------------------------------
// layered_table
// ---------------------------------------------------------------------------

namespace
{

core::tsv_document doc(std::string_view text, std::string source = "layer.tsv")
{
    return core::tsv_document::parse(text, std::move(source));
}

std::vector<std::string> keys(const core::layered_table& t)
{
    std::vector<std::string> out;
    for (const auto& row : t.rows()) out.emplace_back(row["code"]);
    return out;
}

const core::table_schema schema{.key = {"code"}, .required = {"height"}, .optional = {"note"}};

const std::string_view base = "code\theight\n"
                              "BR\t173.6\n"
                              "JP\t172.0\n"
                              "NG\t167.0\n";

} // namespace

TEST_CASE("layered_table loads a base and overrides in place", "[table]")
{
    core::layered_table t{schema};
    REQUIRE(t.empty());
    REQUIRE(t.data_hash() == 0);
    t.extend(doc(base, "base.tsv"), core::layer_mode::replace);
    REQUIRE(keys(t) == std::vector<std::string>{"BR", "JP", "NG"});

    const auto& info = t.extend(doc("code\theight\tnote\n"
                                    "JP\t175\ttaller\n"
                                    "SE\t181.5\t\n"));
    REQUIRE(info.replaced == 1);
    REQUIRE(info.added == 1);
    REQUIRE(info.warnings.empty());
    REQUIRE(keys(t) == std::vector<std::string>{"BR", "JP", "NG", "SE"});
    REQUIRE(t.find({"JP"})->get<double>("height") == 175.0);
    REQUIRE(t.find({"JP"})->source() == "layer.tsv");
    REQUIRE(t.find({"BR"})->source() == "base.tsv");
    REQUIRE_FALSE(t.find({"US"}).has_value());
    REQUIRE(t.layers().size() == 2);
}

TEST_CASE("layered_table removes rows marked disabled", "[table]")
{
    core::layered_table t{schema};
    t.extend(doc(base), core::layer_mode::replace);
    const auto& info = t.extend(doc("code\theight\tdisabled\n"
                                    "JP\t\t1\n"
                                    "US\t\t1\n"
                                    "NG\t170\t0\n"));
    REQUIRE(info.removed == 1);
    REQUIRE(info.replaced == 1);
    REQUIRE(info.warnings.size() == 1); // US was not loaded
    REQUIRE(keys(t) == std::vector<std::string>{"BR", "NG"});
    REQUIRE(t.find({"NG"})->get<double>("height") == 170.0);
    REQUIRE(t.find({"BR"})->get<double>("height") == 173.6); // index rebuilt after removal
    REQUIRE_FALSE(t.find({"JP"}).has_value());

    // A key removed by one layer can come back in the next.
    t.extend(doc("code\theight\nJP\t171\n"));
    REQUIRE(keys(t) == std::vector<std::string>{"BR", "NG", "JP"});
    REQUIRE(t.find({"JP"})->get<double>("height") == 171.0);
}

TEST_CASE("layered_table add and replace modes", "[table]")
{
    core::layered_table t{schema};
    t.extend(doc(base), core::layer_mode::replace);
    t.extend(doc("code\theight\nSE\t181\n"), core::layer_mode::add);
    REQUIRE(t.size() == 4);
    t.extend(doc("code\theight\nUS\t176\n"), core::layer_mode::replace);
    REQUIRE(keys(t) == std::vector<std::string>{"US"});
    REQUIRE(t.layers().size() == 1); // replace starts the history again
    REQUIRE(t.layers().front().source == "layer.tsv");

    // Reloading the same base after other layers gives the original hash.
    core::layered_table u{schema};
    u.extend(doc(base), core::layer_mode::replace);
    const auto base_hash = u.data_hash();
    u.extend(doc("code\theight\nSE\t181\n"));
    REQUIRE(u.data_hash() != base_hash);
    u.extend(doc(base), core::layer_mode::replace);
    REQUIRE(u.data_hash() == base_hash);
    t.clear();
    REQUIRE(t.empty());
    REQUIRE(t.layers().empty());
}

TEST_CASE("layered_table leaves the table unchanged when a layer fails", "[table][errors]")
{
    core::layered_table t{schema};
    t.extend(doc(base), core::layer_mode::replace);
    const auto hash = t.data_hash();
    auto unchanged = [&] {
        REQUIRE(keys(t) == std::vector<std::string>{"BR", "JP", "NG"});
        REQUIRE(t.find({"JP"})->get<double>("height") == 172.0);
        REQUIRE(t.layers().size() == 1);
        REQUIRE(t.data_hash() == hash);
    };

    // Clash in add mode, after a row that would have been added.
    REQUIRE_THROWS_WITH(t.extend(doc("code\theight\nSE\t181\nJP\t1\n"), core::layer_mode::add),
                        "layer.tsv:3: key already loaded (layer is in add mode)");
    unchanged();
    // Key repeated within the layer: a loaded key, a new key, and keys that
    // the layer disabled, loaded or not.
    REQUIRE_THROWS_WITH(t.extend(doc("code\theight\nJP\t1\nJP\t2\n")),
                        "layer.tsv:3: key repeated (first on line 2)");
    REQUIRE_THROWS_WITH(t.extend(doc("code\theight\nSE\t1\nBR\t2\nSE\t3\n")),
                        "layer.tsv:4: key repeated (first on line 2)");
    REQUIRE_THROWS_WITH(t.extend(doc("code\theight\tdisabled\nJP\t\t1\nJP\t180\t0\n")),
                        "layer.tsv:3: key repeated (first on line 2)");
    REQUIRE_THROWS_WITH(t.extend(doc("code\theight\tdisabled\nJP\t180\t0\nJP\t\t1\n")),
                        "layer.tsv:3: key repeated (first on line 2)");
    REQUIRE_THROWS_WITH(t.extend(doc("code\theight\tdisabled\nUS\t\t1\nUS\t176\t0\n")),
                        "layer.tsv:3: key repeated (first on line 2)");
    REQUIRE_THROWS_WITH(t.extend(doc("code\theight\tdisabled\nUS\t\t1\nUS\t\t1\n")),
                        "layer.tsv:3: key repeated (first on line 2)");
    unchanged();
    // Missing required column, on a row with data; and a missing key column.
    REQUIRE_THROWS_WITH(t.extend(doc("code\tnote\nJP\tx\n")),
                        "layer.tsv:2: column 'height': required column missing");
    REQUIRE_THROWS_WITH(t.extend(doc("height\n180\n")), "layer.tsv: column 'code': key column missing");
    unchanged();
    // Empty key.
    REQUIRE_THROWS_AS(t.extend(doc("code\theight\n\t1\n")), core::tsv_error);
    unchanged();
    // Bad disabled value, and disabling in add mode.
    REQUIRE_THROWS_AS(t.extend(doc("code\theight\tdisabled\nJP\t1\tyes\n")), core::tsv_error);
    REQUIRE_THROWS_AS(t.extend(doc("code\theight\tdisabled\nJP\t1\t1\n"), core::layer_mode::add),
                      core::tsv_error);
    unchanged();
    // The library's own row check fails on the second row.
    auto validate = [](const core::tsv_row& row) { (void)row.get<double>("height"); };
    REQUIRE_THROWS_WITH(t.extend(doc("code\theight\nJP\t180\nSE\ttall\n"), core::layer_mode::override, validate),
                        "layer.tsv:3: column 'height': expected a number, got 'tall'");
    unchanged();
}

TEST_CASE("layered_table accepts a removal-only layer without data columns", "[table]")
{
    core::layered_table t{schema};
    t.extend(doc(base), core::layer_mode::replace);
    const auto& info = t.extend(doc("code\tdisabled\nJP\t1\n"));
    REQUIRE(info.removed == 1);
    REQUIRE(keys(t) == std::vector<std::string>{"BR", "NG"});
    // ...but a row that is not disabled still needs them.
    REQUIRE_THROWS_AS(t.extend(doc("code\tdisabled\nNG\t1\nSE\t0\n")), core::tsv_error);
    REQUIRE(keys(t) == std::vector<std::string>{"BR", "NG"});
}

TEST_CASE("layered_table warns about unknown columns", "[table]")
{
    core::layered_table t{schema};
    const auto& info = t.extend(doc("code\theight\tcolour\nBR\t1\tgreen\n"));
    REQUIRE(info.warnings.size() == 1);
    REQUIRE(info.warnings[0] == "layer.tsv: column 'colour' is not used and was ignored");
}

TEST_CASE("layered_table data hash follows content, order and mode", "[table]")
{
    auto build = [](std::string_view first, core::layer_mode mode) {
        core::layered_table t{schema};
        t.extend(doc(first), core::layer_mode::replace);
        t.extend(doc("code\theight\nSE\t181\n"), mode);
        return t.data_hash();
    };
    const auto a = build(base, core::layer_mode::override);
    REQUIRE(a != 0);
    REQUIRE(a == build(base, core::layer_mode::override));
    REQUIRE(a != build(base, core::layer_mode::add));
    REQUIRE(a != build("code\theight\nBR\t173.6\n", core::layer_mode::override));

    // The same data with CRLF line ends hashes the same.
    std::string crlf;
    for (const char c : base)
    {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    REQUIRE(a == build(crlf, core::layer_mode::override));
}

TEST_CASE("layered_table with a composite key", "[table]")
{
    core::layered_table t{{.key = {"country", "sex"}, .required = {"mean"}, .optional = {}}};
    t.extend(doc("country\tsex\tmean\nBR\tm\t173\nBR\tf\t160\n"), core::layer_mode::replace);
    t.extend(doc("country\tsex\tmean\nBR\tf\t161\n"));
    REQUIRE(t.size() == 2);
    REQUIRE(t.find({"BR", "f"})->get<int>("mean") == 161);
    REQUIRE(t.find({"BR", "m"})->get<int>("mean") == 173);
    REQUIRE_THROWS_AS(t.find({"BR"}), std::invalid_argument);
    REQUIRE_THROWS_AS(core::layered_table{core::table_schema{}}, std::invalid_argument);
}

TEST_CASE("layered_table finds every key through growth, overrides and removals", "[table]")
{
    // 5,000 keys grow the index several times; the keys include ones that
    // are prefixes of each other and a composite pair that joins ambiguously.
    std::string text = "code\theight\n";
    for (int i = 0; i < 5000; ++i) text += "K" + std::to_string(i) + "\t" + std::to_string(i) + "\n";
    core::layered_table t{schema};
    t.extend(doc(text), core::layer_mode::replace);
    REQUIRE(t.size() == 5000);
    for (int i = 0; i < 5000; i += 7)
    {
        REQUIRE(t.find({"K" + std::to_string(i)})->get<int>("height") == i);
    }
    REQUIRE_FALSE(t.find({"K5000"}).has_value());
    REQUIRE_FALSE(t.find({"K"}).has_value());

    // Override some, remove some, add some, then look everything up again.
    std::string layer = "code\theight\tdisabled\n";
    for (int i = 0; i < 5000; i += 10) layer += "K" + std::to_string(i) + "\t-1\t\n";
    for (int i = 5; i < 5000; i += 10) layer += "K" + std::to_string(i) + "\t\t1\n";
    layer += "NEW\t9\t\n";
    const auto& info = t.extend(doc(layer));
    REQUIRE(info.replaced == 500);
    REQUIRE(info.removed == 500);
    REQUIRE(info.added == 1);
    REQUIRE(t.size() == 4501);
    for (int i = 0; i < 5000; ++i)
    {
        const auto row = t.find({"K" + std::to_string(i)});
        if (i % 10 == 5) REQUIRE_FALSE(row.has_value());
        else REQUIRE(row->get<int>("height") == (i % 10 == 0 ? -1 : i));
    }
    REQUIRE(t.find({"NEW"})->get<int>("height") == 9);

    core::layered_table pairs{{.key = {"a", "b"}, .required = {}, .optional = {}}};
    pairs.extend(doc("a\tb\nx\tyz\nxy\tz\n"), core::layer_mode::replace);
    REQUIRE(pairs.size() == 2);
    REQUIRE(pairs.find({"x", "yz"})->get<std::string>("b") == "yz");
    REQUIRE(pairs.find({"xy", "z"})->get<std::string>("b") == "z");
    REQUIRE_FALSE(pairs.find({"xyz", ""}).has_value());
}

// ---------------------------------------------------------------------------
// resources
// ---------------------------------------------------------------------------

namespace
{

// tests/fixtures/resources, found the way libraries find theirs; works from
// the repository root and from build/.
std::filesystem::path fixture_root()
{
    const auto root = core::find_resources("tests/fixtures", "lite/sample.tsv");
    REQUIRE(root.has_value());
    return *root;
}

void require_same_content(const core::tsv_document& a, const core::tsv_document& b)
{
    REQUIRE(a.columns() == b.columns());
    REQUIRE(a.size() == b.size());
    for (std::size_t r = 0; r < a.size(); ++r)
    {
        for (std::size_t c = 0; c < a.columns().size(); ++c)
        {
            REQUIRE(a.row(r).cell(c) == b.row(r).cell(c));
        }
    }
    REQUIRE(a.content_hash() == b.content_hash());
}

} // namespace

TEST_CASE("tier names", "[resources]")
{
    STATIC_REQUIRE(core::tier_name(dasmig::dataset::lite) == "lite");
    STATIC_REQUIRE(core::tier_name(dasmig::dataset::full) == "full");
}

TEST_CASE("embedded files decode to exactly the files on disk", "[resources]")
{
    const auto& files = dasmig::embedded::fixture::files;
    REQUIRE(files.size() == 2);
    const auto* sample = core::find_embedded(files, "lite/sample.tsv");
    REQUIRE(sample != nullptr);
    REQUIRE(sample->chunks.size() > 1); // the fixture is generated in small chunks
    REQUIRE(core::find_embedded(files, "lite/missing.tsv") == nullptr);
    REQUIRE(core::find_embedded({}, "lite/sample.tsv") == nullptr);

    const auto root = fixture_root();
    const auto from_disk = core::tsv_document::load(root / "lite" / "sample.tsv");
    const auto from_embedded = core::tsv_document::parse(sample->text(), "embedded");
    require_same_content(from_disk, from_embedded);

    // Quotes, backslashes, multi-byte text and digits after accents survive.
    REQUIRE(from_embedded.row(0)["note"] == R"("quoted" \ back\slash)");
    REQUIRE(from_embedded.row(1)["name"] == "\xe6\x97\xa5\xe6\x9c\xac");
    REQUIRE(from_embedded.row(1)["note"] == "\xc3\xa9" "1" "\xc3\xa9" "2 digits after accents");
    REQUIRE(from_embedded.row(2)["note"].size() == 1200);
}

TEST_CASE("load_resource prefers embedded data and falls back to disk", "[resources]")
{
    const auto root = fixture_root();
    const auto& files = dasmig::embedded::fixture::files;

    const auto embedded = core::load_resource("lite/sample.tsv", files, root);
    REQUIRE(embedded.has_value());
    REQUIRE(embedded->source() == "embedded:lite/sample.tsv");

    const auto disk = core::load_resource("lite/sample.tsv", {}, root);
    REQUIRE(disk.has_value());
    REQUIRE(disk->source() == (root / "lite/sample.tsv").generic_string());
    require_same_content(*embedded, *disk);

    // Headerless word list, both ways.
    const auto words = core::load_resource("lite/animals.words", files, root, {"word"});
    REQUIRE(words.has_value());
    REQUIRE(words->row(2)["word"] == "Caf\xc3\xa9");
    require_same_content(*words, *core::load_resource("lite/animals.words", {}, root, {"word"}));

    REQUIRE_FALSE(core::load_resource("lite/missing.tsv", files, root).has_value());
    REQUIRE_FALSE(core::load_resource("lite/sample.tsv", {}, std::nullopt).has_value());
}

TEST_CASE("find_resources searches the standard candidates", "[resources]")
{
    const auto probe_base = std::filesystem::temp_directory_path() / "dasmig_core_probe";
    std::filesystem::remove_all(probe_base);
    const auto lib_root = probe_base / "parent" / "x-generator" / "resources";
    std::filesystem::create_directories(lib_root / "lite");
    std::ofstream{lib_root / "lite" / "marker.tsv"} << "a\n";

    // From the parent directory: <library>/resources.
    REQUIRE(core::find_resources("x-generator", "lite/marker.tsv", probe_base / "parent") ==
            probe_base / "parent" / "x-generator" / "resources");
    // From the repository: resources.
    REQUIRE(core::find_resources("x-generator", "lite/marker.tsv",
                                 probe_base / "parent" / "x-generator") ==
            probe_base / "parent" / "x-generator" / "resources");
    // From a build directory inside it: ../resources.
    std::filesystem::create_directories(probe_base / "parent" / "x-generator" / "build");
    REQUIRE(core::find_resources("x-generator", "lite/marker.tsv",
                                 probe_base / "parent" / "x-generator" / "build")
                .has_value());
    // The marker must match: another library's resources are skipped.
    REQUIRE_FALSE(core::find_resources("x-generator", "lite/other.tsv", probe_base / "parent"));
    REQUIRE_FALSE(core::find_resources("y-generator", "lite/marker.tsv", probe_base));
    REQUIRE(core::resource_candidates("x-generator", probe_base).size() == 4);
    std::filesystem::remove_all(probe_base);
}
