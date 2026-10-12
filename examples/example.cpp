#include "../dasmig/core.hpp"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

// Libraries check that the vendored core in use is compatible.
DASMIG_CORE_REQUIRE(1, 0);

namespace core = dasmig::core;

int main()
{
    std::cout << "dasmig-core " << core::version.major << '.'
              << core::version.minor << '.' << core::version.patch << "\n\n";

    // A seeded engine gives the same sequence on every platform.
    core::rng r{42};
    std::cout << "First draws for seed 42:";
    for (int i = 0; i < 3; ++i)
    {
        std::cout << ' ' << r();
    }
    std::cout << "\n\n";

    // Child streams depend on the seed and a label, not on earlier draws.
    const core::rng person{2026};
    auto body = person.split("body");
    auto mind = person.split("mind");
    std::cout << "Height: " << core::normal(body, 170.0, 7.5) << " cm\n";
    std::cout << "Weight: " << core::lognormal(body, 4.25, 0.18) << " kg\n";
    std::cout << "Mood:   " << core::uniform_int(mind, 1, 10) << " / 10\n\n";

    // Weighted choice.
    constexpr std::array<std::string_view, 3> eyes{"brown", "intermediate", "blue"};
    const core::discrete eye_weights{0.79, 0.12, 0.09};
    std::cout << "Eyes:";
    for (int i = 0; i < 5; ++i)
    {
        std::cout << ' ' << eyes.at(eye_weights(r));
    }
    std::cout << '\n';

    // Shuffling and sampling.
    std::vector<int> deck{1, 2, 3, 4, 5, 6, 7, 8};
    core::shuffle(r, deck);
    std::cout << "Shuffled:";
    for (const int card : deck)
    {
        std::cout << ' ' << card;
    }
    std::cout << "\nPicked: " << core::pick(r, deck) << '\n';

    // Data: a base layer and an extension layer that overrides one row.
    core::layered_table heights{{.key = {"code"}, .required = {"height"}, .optional = {}}};
    heights.extend(core::tsv_document::parse("code\theight\nBR\t173.6\nJP\t172.1\n", "base"),
                   core::layer_mode::replace);
    heights.extend(core::tsv_document::parse("code\theight\nJP\t175\n", "mod"));

    // Weighted choice with a per-call condition.
    const core::weighted_table<std::string_view> jobs{{.value = "farmer", .weight = 3.0},
                                                      {.value = "nurse", .weight = 2.0},
                                                      {.value = "pilot", .weight = 0.2}};
    const int age = 17;
    const auto& job = jobs.pick(r, [&](std::string_view j, double w) {
        return (j == "pilot" && age < 21) ? 0.0 : w;
    });

    // JSON, with the replay signature (seed plus the data it used).
    core::json_writer json;
    json.begin_object().field("job", job).key("heights").begin_object();
    for (const auto& row : heights.rows())
    {
        json.field(row["code"], row.get<double>("height"));
    }
    json.end_object().key("replay");
    core::seed_signature{.seed = person.seed(), .data = heights.data_hash()}.to_json(json);
    json.end_object();
    std::cout << "\nJSON: " << json.str() << '\n';

    // A replayed seed gives the same result.
    core::rng replay{person.child_seed("body")};
    std::cout << "\nReplayed height: " << core::normal(replay, 170.0, 7.5)
              << " cm\n";
    return 0;
}
