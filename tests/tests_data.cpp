// Tests for the data parts of the core: utf8, json, tsv, weighted_table,
// seed_signature and layered_table.

#include "catch_amalgamated.hpp"

#include "../dasmig/core.hpp"

#include <cstdint>
#include <filesystem>
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
}

TEST_CASE("layered_table add and replace modes", "[table]")
{
    core::layered_table t{schema};
    t.extend(doc(base), core::layer_mode::replace);
    t.extend(doc("code\theight\nSE\t181\n"), core::layer_mode::add);
    REQUIRE(t.size() == 4);
    t.extend(doc("code\theight\nUS\t176\n"), core::layer_mode::replace);
    REQUIRE(keys(t) == std::vector<std::string>{"US"});
    REQUIRE(t.layers().size() == 3);
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
    // Key repeated within the layer.
    REQUIRE_THROWS_WITH(t.extend(doc("code\theight\nJP\t1\nJP\t2\n")),
                        "layer.tsv:3: key repeated (first on line 2)");
    unchanged();
    // Missing required column.
    REQUIRE_THROWS_WITH(t.extend(doc("code\tnote\nJP\tx\n")),
                        "layer.tsv: column 'height': required column missing");
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
