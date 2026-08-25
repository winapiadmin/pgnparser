#include "../pgnparser.hpp"
#include <iostream>
#include <string>
#include <string_view>

using namespace pgn;

#ifndef TEST_DIR
#define TEST_DIR "../tests/"
#endif

struct CountVisitor : PGNVisitor {
    int count = 0;
    bool wantsTags() const override { return false; }
    bool onGameStart() override { return false; }
    void onGameEnd(std::string_view) override { count++; }
};

struct FullParseVisitor : PGNVisitor {
    int count = 0;
    bool onGameStart() override { return true; }
    void onGameEnd(std::string_view) override { count++; }
};

#define CHECK(cond) do { \
    if (!(cond)) { std::cerr << "FAIL: " #cond << std::endl; failures++; } \
    else { std::cout << "  PASS: " #cond << std::endl; } \
} while(0)

#define CHECK_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { std::cerr << "FAIL: " #a " == " #b "  got " << _a << " expected " << _b << std::endl; failures++; } \
    else { std::cout << "  PASS: " #a " == " #b << std::endl; } \
} while(0)

#define TEST_CASE(name) std::cout << "\n--- " name " ---" << std::endl

// Both parse paths run over one mmapped view of the input: files are no
// longer read into heap strings at all, and are loaded once instead of
// twice (once per parse path).
static int count_skip(PGNInput &in) {
    CountVisitor v;
    PGNParser parser(v);
    parser.parseAll(in);
    return v.count;
}

static int count_full(PGNInput &in) {
    FullParseVisitor v;
    PGNParser parser(v);
    parser.parseAll(in);
    return v.count;
}

static int count_skip(std::string_view data) {
    PGNInput in(data);
    return count_skip(in);
}

static int count_full(std::string_view data) {
    PGNInput in(data);
    return count_full(in);
}

// Maps the file once and runs both parse paths over it via rewind().
static void check_file_counts(const char *name, int expected, int &failures) {
    const std::string path = TEST_DIR + std::string(name);
    PGNInput in(path);
    const int skipCount = count_skip(in);
    in.rewind();
    const int fullCount = count_full(in);
    CHECK_EQ(skipCount, expected);
    CHECK_EQ(fullCount, expected);
}

int main() {
    int failures = 0;

    {
        TEST_CASE("Skip path and full parse agree on all test files");
        static const struct FileCount {
            const char* name;
            int expected;
        } files[] = {
            {                                              "test1.pgn", 1 },
            {                                              "test2.pgn", 1 },
            {                                              "test3.pgn", 1 },
            {                                              "test4.pgn", 3 },
            {                                              "test5.pgn", 1 },
            {                                              "basic.pgn", 1 },
            {                                               "book.pgn", 1 },
            {                                                "nag.pgn", 1 }, // movetext only, no tag section
            {                                            "non_ascii.pgn", 1 },
            {                                     "square_bracket_in_header.pgn", 1 },
            {                                  "unescaped_quote_header.pgn", 1 },
            {                                    "backslash_header.pgn", 1 },
            {                                      "multiple.pgn", 4 },
            {                                     "test_edge.pgn", 3 },
            {                                   "no_result.pgn", 1 },
            {                                   "empty_body.pgn", 1 }, // game 1 has no result marker
            {                                       "skip.pgn", 2 },
            {                                   "variations.pgn", 1 }, // nested + stacked variations
            {                               "test_variations.pgn", 1 },
            {                           "test_variation_valid.pgn", 1 },
            {                         "test_variation_simple.pgn", 1 },
            {                          "test_variation_black.pgn", 1 },
            {                                             "test_nags.pgn", 1 },
            {                                            "test_bxa3.pgn", 1 },
            {                                            "test_bxc3.pgn", 1 },
            {                        "comment_before_moves.pgn", 1 },
            {   "lichess_pgn_2026.06.20_prev_internal_engine_vs_internal_engine.UqWasVld.pgn", 1 },
            {                     "no_moves_but_game_termination.pgn", 1 },
            {                   "no_moves_but_game_termination_2.pgn", 1 },
            {                   "no_moves_but_game_termination_3.pgn", 1 },
            {                                "no_moves_two_games.pgn", 2 },
            {  "no_moves_but_comment_followed_by_termination_marker.pgn", 2 },
            {          "no_moves_but_game_termination_multiple.pgn", 2 },
          { "no_moves_but_game_termination_multiple_2.pgn", 3 },
            {                                              "no_moves.pgn", 0 }, // no body at all (tags only)
        };
        for (auto& f : files)
            check_file_counts(f.name, f.expected, failures);
    }

    {
        TEST_CASE("Movetext without termination marker: skip counts structure, full needs marker");
        static const char* noMarkerFiles[] = {
            "black2move.pgn",
            "castling.pgn",
            "newline.pgn",
            "threefold_repetition.pgn",
        };
        for (const char* name : noMarkerFiles) {
            std::cout << "  " << name << std::endl;
            PGNInput in(TEST_DIR + std::string(name));
            CHECK_EQ(count_skip(in), 1); // skip path counts game structure
            in.rewind();
            CHECK_EQ(count_full(in), 0); // full path needs a result marker
        }
    }

    {
        TEST_CASE("corrupted.pgn: truncated file differs between paths");
        PGNInput in(TEST_DIR "corrupted.pgn");
        CHECK_EQ(count_skip(in), 1); // skip path counts structure
        in.rewind();
        CHECK_EQ(count_full(in), 0); // full path needs result marker
    }

    {
        TEST_CASE("Inline data: single game with moves");
        CHECK_EQ(count_skip("[Event \"?\"]\n[Result \"1-0\"]\n\n1. e4 e5 2. Nf3 Nc6 *\n"), 1);
        CHECK_EQ(count_full("[Event \"?\"]\n[Result \"1-0\"]\n\n1. e4 e5 2. Nf3 Nc6 *\n"), 1);
    }

    {
        TEST_CASE("Inline data: two games with \\n line endings");
        std::string data =
            "[Event \"1\"]\n[Result \"1-0\"]\n\n1. e4 e5 *\n\n"
            "[Event \"2\"]\n[Result \"0-1\"]\n\n1. d4 d5 *\n";
        CHECK_EQ(count_skip(data), 2);
        CHECK_EQ(count_full(data), 2);
    }

    {
        TEST_CASE("Inline data: two games with \\r\\n line endings");
        std::string data =
            "[Event \"1\"]\r\n[Result \"1-0\"]\r\n\r\n1. e4 e5 *\r\n\r\n"
            "[Event \"2\"]\r\n[Result \"0-1\"]\r\n\r\n1. d4 d5 *\r\n";
        CHECK_EQ(count_skip(data), 2);
        CHECK_EQ(count_full(data), 2);
    }

    {
        TEST_CASE("Inline data: two games with mixed \\n\\r\\n line endings");
        std::string data =
            "[Event \"1\"]\n[Result \"1-0\"]\n\n1. e4 e5 *\n\n"
            "[Event \"2\"]\n[Result \"0-1\"]\n\n1. d4 d5 *\n";
        CHECK_EQ(count_skip(data), 2);
        CHECK_EQ(count_full(data), 2);
    }

    {
        TEST_CASE("Inline data: multiple whitespace between games");
        std::string data =
            "[Event \"1\"]\n[Result \"1-0\"]\n\n1. e4 e5 *\n\n\n\n"
            "[Event \"2\"]\n[Result \"0-1\"]\n\n1. d4 d5 *\n";
        CHECK_EQ(count_skip(data), 2);
        CHECK_EQ(count_full(data), 2);
    }

    {
        TEST_CASE("Inline data: no blank line between games (back-to-back tags)");
        std::string data =
            "[Event \"1\"]\n[Result \"1-0\"]\n\n1. e4 e5 *\n"
            "[Event \"2\"]\n[Result \"0-1\"]\n\n1. d4 d5 *\n";
        CHECK_EQ(count_skip(data), 2);
        CHECK_EQ(count_full(data), 2);
    }

    {
        TEST_CASE("Inline data: three games with comments and variations");
        std::string data =
            "[Event \"1\"]\n[Result \"1-0\"]\n\n1. e4 {good} e5 2. Nf3 *\n\n"
            "[Event \"2\"]\n[Result \"0-1\"]\n\n1. d4 d5 2. c4 *\n\n"
            "[Event \"3\"]\n[Result \"1/2-1/2\"]\n\n1. e4 e5 (1... c5) 2. Nf3 *\n";
        CHECK_EQ(count_skip(data), 3);
        CHECK_EQ(count_full(data), 3);
    }

    {
        TEST_CASE("Inline data: no-move games with result markers in body");
        CHECK_EQ(count_skip("[Event \"1\"]\n[Result \"1-0\"]\n\n1-0\n"), 1);
        CHECK_EQ(count_skip("[Event \"1\"]\n[Result \"0-1\"]\n\n0-1\n"), 1);
        CHECK_EQ(count_skip("[Event \"1\"]\n[Result \"1/2-1/2\"]\n\n1/2-1/2\n"), 1);
        CHECK_EQ(count_skip("[Event \"1\"]\n[Result \"*\"]\n\n*\n"), 1);
        CHECK_EQ(count_skip("[Event \"1\"]\n[Result \"?\"]\n\n"), 0);
    }

    {
        TEST_CASE("Inline data: game with FEN and result in body counts");
        CHECK_EQ(count_skip("[FEN \"4k3/8/8/8/8/8/8/4K3 w - - 0 1\"]\n[Result \"*\"]\n\n*"), 1);
        CHECK_EQ(count_full("[FEN \"4k3/8/8/8/8/8/8/4K3 w - - 0 1\"]\n[Result \"*\"]\n\n*"), 1);
    }

    {
        TEST_CASE("Inline data: comment before movetext (no_result.pgn pattern)");
        std::string data = "[Event \"1\"]\n[Result \"*\"]\n\n{No result} *\n";
        CHECK_EQ(count_skip(data), 1);
        CHECK_EQ(count_full(data), 1);
    }

    {
        TEST_CASE("Inline data: semicolon comment");
        std::string data =
            "[Event \"1\"]\n[Result \"1-0\"]\n\n1. e4 e5 ; a comment\n2. Nf3 *\n\n"
            "[Event \"2\"]\n[Result \"0-1\"]\n\n1. d4 *\n";
        CHECK_EQ(count_skip(data), 2);
        CHECK_EQ(count_full(data), 2);
    }

    {
        TEST_CASE("Inline data: empty input yields 0 games");
        CHECK_EQ(count_skip(""), 0);
        CHECK_EQ(count_full(""), 0);
    }

    {
        TEST_CASE("Inline data: tags-only with no body yields 0 games");
        CHECK_EQ(count_skip("[Event \"?\"]\n[Result \"*\"]\n"), 0);
    }

    {
        TEST_CASE("Inline data: result markers embedded in text are not false positives");
        // '0-1' appearing inside a move like B0-1 would not parse, but the marker
        // '0-1' separating two games should still work.
        std::string data =
            "[Event \"1\"]\n[Result \"0-1\"]\n\n1. e4 e5 0-1\n\n"
            "[Event \"2\"]\n[Result \"1-0\"]\n\n1. d4 d5 1-0\n";
        CHECK_EQ(count_skip(data), 2);
        CHECK_EQ(count_full(data), 2);
    }

    {
        TEST_CASE("Large multi-game files: golden counts, both paths agree");
        static const struct FileCount {
            const char* name;
            int expected;
        } files[] = {
            {  "games1345598358.pgn",    2 },
            {  "games1352081361.pgn",   20 },
            {  "games1567817165.pgn",   27 },
            {  "games2127584351.pgn",   68 },
            {  "games1537757853.pgn",  701 },
            {  "games2123827639.pgn",  748 },
            {  "games136431434.pgn",  1051 },
            {  "games1450490177.pgn", 1064 },
            {  "games1400823794.pgn", 1184 },
            {  "games1227580063.pgn", 1535 },
            {  "games1730228391.pgn", 1950 },
            {         "games.pgn",    7811 },
        };
        for (auto& f : files)
            check_file_counts(f.name, f.expected, failures);
    }

    std::cout << "\n==============================" << std::endl;
    if (failures == 0) {
        std::cout << "All tests passed!" << std::endl;
        return 0;
    } else {
        std::cout << failures << " test(s) FAILED!" << std::endl;
        return 1;
    }
}
