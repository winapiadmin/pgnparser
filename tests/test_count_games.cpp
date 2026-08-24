#include "../pgnparser.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

using namespace pgn;

#ifndef TEST_DIR
#define TEST_DIR "../tests/"
#endif

static std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) return "";
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

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

static int count_skip(const std::string& data) {
    CountVisitor v;
    PGNParser parser(v);
    parser.parseAll(data);
    return v.count;
}

static int count_full(const std::string& data) {
    FullParseVisitor v;
    PGNParser parser(v);
    parser.parseAll(data);
    return v.count;
}

static int count_skip_file(const std::string& path) {
    return count_skip(read_file(path));
}

static int count_full_file(const std::string& path) {
    return count_full(read_file(path));
}

int main() {
    int failures = 0;

    {
        TEST_CASE("Skip path and full parse agree on all test files");
        struct FileCount {
            const char* name;
            int expected;
        } files[] = {
            {                                    "test1.pgn", 1 },
            {                                    "test2.pgn", 1 },
            {                                    "test3.pgn", 1 },
            {                                    "test4.pgn", 3 },
            {                                    "test5.pgn", 1 },
            {                                 "no_moves.pgn", 0 }, // no body at all (tags only)
            {            "no_moves_but_game_termination.pgn", 1 },
            {          "no_moves_but_game_termination_2.pgn", 1 },
            {          "no_moves_but_game_termination_3.pgn", 1 },
            {                       "no_moves_two_games.pgn", 2 },
            {   "no_moves_but_game_termination_multiple.pgn", 2 },
            { "no_moves_but_game_termination_multiple_2.pgn", 3 },
            {                                "no_result.pgn", 1 },
            {                               "empty_body.pgn", 1 }, // game 1 has no result marker
            {                                     "skip.pgn", 2 },
            {                                 "multiple.pgn", 4 },
            {                                "test_edge.pgn", 2 },
            {                         "backslash_header.pgn", 1 }
        };
        for (auto& f : files) {
            std::string path = TEST_DIR + std::string(f.name);
            int skipCount = count_skip_file(path);
            int fullCount = count_full_file(path);
            std::cerr<<path<<'\n';
            CHECK_EQ(skipCount, f.expected);
            CHECK_EQ(fullCount, f.expected);
        }
    }

    {
        TEST_CASE("corrupted.pgn: truncated file differs between paths");
        int skipCount = count_skip_file(TEST_DIR "corrupted.pgn");
        int fullCount = count_full_file(TEST_DIR "corrupted.pgn");
        CHECK_EQ(skipCount, 1);  // skip path counts structure
        CHECK_EQ(fullCount, 0);  // full path needs result marker
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
        TEST_CASE("games.pgn count matches");
        int skipCount = count_skip_file(TEST_DIR "games.pgn");
        CHECK_EQ(skipCount, 7811);
        int fullCount = count_full_file(TEST_DIR "games.pgn");
        CHECK_EQ(fullCount, 7811);
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
