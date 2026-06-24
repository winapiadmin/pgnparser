#include "../pgnexport.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <cassert>
#include <cstdio>

using namespace chess;

#ifndef TEST_DIR
#define TEST_DIR "../tests/"
#endif

static std::string read_file(const std::string& path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file) return "";
    return std::string((std::istreambuf_iterator<char>(file)),
                       std::istreambuf_iterator<char>());
}

static pgn::Game parse_game(const std::string& path) {
    pgn::PGNBuilder builder;
    pgn::PGNParser parser(builder);
    parser.parse(read_file(path));
    return builder.release();
}

static std::string print_game(const pgn::Game& game) {
    std::ostringstream oss;
    pgn::PGNPrinter printer(oss);
    printer.print(game);
    return oss.str();
}

#define CHECK(cond) do { \
    if (!(cond)) { std::cerr << "FAIL: " #cond << " (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; failures++; } \
    else { std::cout << "  PASS: " #cond << std::endl; } \
} while(0)

#define CHECK_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a != _b) { std::cerr << "FAIL: " #a " == " #b "  got \"" << _a << "\" expected \"" << _b << "\" (" << __FILE__ << ":" << __LINE__ << ")" << std::endl; failures++; } \
    else { std::cout << "  PASS: " #a " == " #b << std::endl; } \
} while(0)

#define TEST_CASE(name) std::cout << "\n--- " name " ---" << std::endl; int failures_##__LINE__ = 0
#define FAIL(failures) do { failures += failures_##__LINE__; } while(0)

// -------------------------------------------------------------------
// Helpers
// -------------------------------------------------------------------

/// Extract the final line containing the termination marker.
static std::string last_line(const std::string& s) {
    auto pos = s.rfind('\n');
    if (pos == std::string::npos) return s;
    // skip blank lines before the result
    auto end = s.size();
    while (pos > 0 && s[pos-1] == '\n') pos--;
    return s.substr(pos + 1, end - pos - 2); // strip trailing \n
}

/// Check if a line (or part of it) appears in the output.
static bool contains_line(const std::string& output, const std::string& line) {
    return output.find(line) != std::string::npos;
}

// -------------------------------------------------------------------
// Tests
// -------------------------------------------------------------------

int main() {
    int failures = 0;

    // ---------------------------------------------------------------
    {
        TEST_CASE("STR tags are emitted in standard order");

        pgn::Game game;
        pgn::PGNPrinter printer(std::cout);
        std::ostringstream oss;
        pgn::PGNPrinter p(oss);
        p.print(game);
        std::string out = oss.str();

        // The first 7 lines must be the STR tags in order
        CHECK_EQ(out.substr(0, 7), "[Event ");
        CHECK(contains_line(out, "[Event \"?\"]"));
        CHECK(contains_line(out, "[Site \"?\"]"));
        CHECK(contains_line(out, "[Date \"?\"]"));
        CHECK(contains_line(out, "[Round \"?\"]"));
        CHECK(contains_line(out, "[White \"?\"]"));
        CHECK(contains_line(out, "[Black \"?\"]"));
        CHECK(contains_line(out, "[Result \"*\"]"));

        // STR tags appear before any non-STR tags
        auto posEvent = out.find("[Event ");
        auto posSite  = out.find("[Site ", posEvent + 1);
        auto posDate  = out.find("[Date ", posSite + 1);
        auto posRound = out.find("[Round ", posDate + 1);
        auto posWhite = out.find("[White ", posRound + 1);
        auto posBlack = out.find("[Black ", posWhite + 1);
        auto posResult= out.find("[Result ", posBlack + 1);
        CHECK(posEvent != std::string::npos);
        CHECK(posSite > posEvent);
        CHECK(posDate > posSite);
        CHECK(posRound > posDate);
        CHECK(posWhite > posRound);
        CHECK(posBlack > posWhite);
        CHECK(posResult > posBlack);

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Parse and print test1.pgn preserves all STR tags");

        auto game = parse_game(TEST_DIR "test1.pgn");
        std::string out = print_game(game);

        // STR tags with actual values
        CHECK(contains_line(out, "[Event \"?\"]"));
        CHECK(contains_line(out, "[Date \"2026.06.20\"]"));
        CHECK(contains_line(out, "[White \"prev_internal_engine\"]"));
        CHECK(contains_line(out, "[Black \"internal_engine\"]"));
        CHECK(contains_line(out, "[Result \"0-1\"]"));

        // Non-STR tags follow
        CHECK(contains_line(out, "[GameId \"UqWasVld\"]"));
        CHECK(contains_line(out, "[Variant \"Standard\"]"));

        // Moves present
        CHECK(contains_line(out, "1. Nc3 g6"));
        CHECK(contains_line(out, "20. bxa3 Rxa3"));
        CHECK(contains_line(out, "41. h5 Bxd4# 0-1"));

        // Exactly 41 move numbers (pattern: digit(s) followed by ". ")
        int dots = 0;
        for (size_t i = 1; i+1 < out.size(); i++) {
            if (out[i] == '.' && out[i+1] == ' ' && out[i-1] >= '0' && out[i-1] <= '9')
                dots++;
        }
        CHECK_EQ(dots, 41);

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Coordinate notation normalizes to SAN");

        auto game = parse_game(TEST_DIR "test4.pgn");
        std::string out = print_game(game);

        pgn::PGNBuilder builder;
        pgn::PGNParser parser(builder);
        parser.parse("e2e4 e7e5 g1f3 b8c6 *");
        auto g = builder.release();
        std::string o = print_game(g);
        CHECK(o.find("1. e4 e5 2. Nf3 Nc6 *") != std::string::npos);

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Comments are preserved");

        auto game = parse_game(TEST_DIR "test3.pgn");
        // test3.pgn: 1. Nc3 {???} Nc6 *
        // Our parser attaches comments to the previous move node
        CHECK(game.root != nullptr);
        CHECK_EQ(game.root->comment, "???");
        std::string out = print_game(game);
        CHECK(contains_line(out, "1. Nc3 {???} Nc6 *"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("FEN start position is handled");

        auto game = parse_game(TEST_DIR "test2.pgn");
        CHECK(game.startPos.fen().find("r2qkb1r") == 0);
        // The game starts at move 8
        std::string out = print_game(game);
        CHECK(contains_line(out, "8. Nf3"));
        CHECK(contains_line(out, "[FEN \"r2qkb1r/pppb1ppp/2n5/3np3/8/2PP4/PPQ2PPP/RNB1KBNR w KQkq - 1 8\"]"));
        // SetUp tag should follow STR tags
        CHECK(contains_line(out, "[SetUp \"1\"]"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Engine comments in test2.pgn are preserved");

        auto game = parse_game(TEST_DIR "test2.pgn");
        std::string out = print_game(game);
        CHECK(contains_line(out, "{-0.65/10 0.325s, n=179998}"));
        CHECK(contains_line(out, "{+0.61/8 0.323s, n=207718}"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Game::fromPosition reconstructs moves");

        Position pos;
        // Play 1. e4 e5 2. Nf3 Nc6
        auto m1 = uci::uciToMove(pos, "e2e4"); CHECK(m1.is_ok()); pos.doMove(m1);
        auto m2 = uci::uciToMove(pos, "e7e5"); CHECK(m2.is_ok()); pos.doMove(m2);
        auto m3 = uci::uciToMove(pos, "g1f3"); CHECK(m3.is_ok()); pos.doMove(m3);
        auto m4 = uci::uciToMove(pos, "b8c6"); CHECK(m4.is_ok()); pos.doMove(m4);

        auto game = pgn::Game::fromPosition(pos);
        CHECK(game.root != nullptr);
        std::string out = print_game(game);
        CHECK(contains_line(out, "1. e4 e5 2. Nf3 Nc6"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Game::fromPosition with non-standard start");

        Position pos("r1bqkb1r/pppppppp/2n5/4P3/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 3");
        auto m = uci::uciToMove(pos, "d2d4"); CHECK(m.is_ok()); pos.doMove(m);
        m = uci::uciToMove(pos, "e7e6"); CHECK(m.is_ok()); pos.doMove(m);

        auto game = pgn::Game::fromPosition(pos);
        std::string out = print_game(game);
        // Should include FEN tag since start position is non-standard
        CHECK(contains_line(out, "[FEN \"r1bqkb1r/pppppppp/2n5/4P3/8/8/PPPP1PPP/RNBQKBNR w KQkq - 0 3\"]"));
        CHECK(contains_line(out, "3. d4 e6"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Non-STR tags appear after STR tags");

        auto game = parse_game(TEST_DIR "test1.pgn");
        std::string out = print_game(game);
        auto posResult = out.find("[Result \"0-1\"]");
        auto posGameId = out.find("[GameId \"UqWasVld\"]");
        CHECK(posResult != std::string::npos);
        CHECK(posGameId != std::string::npos);
        CHECK(posGameId > posResult); // GameId comes after Result

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Result tag matches game result");

        auto game = parse_game(TEST_DIR "test1.pgn");
        CHECK_EQ(game.result, "0-1");
        std::string out = print_game(game);
        // Result tag should be "0-1" and the termination marker should match
        CHECK(contains_line(out, "[Result \"0-1\"]"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("No-move game with termination marker");

        pgn::PGNBuilder builder;
        pgn::PGNParser parser(builder);
        parser.parse("[Event \"Test\"]\n[Result \"1-0\"]\n\n1-0\n");
        auto game = builder.release();
        CHECK(game.root == nullptr);
        CHECK_EQ(game.result, "1-0");
        std::string out = print_game(game);
        CHECK(contains_line(out, "[Result \"1-0\"]"));
        CHECK(contains_line(out, "1-0"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Validate moves from test1.pgn via chesslib");

        auto game = parse_game(TEST_DIR "test1.pgn");

        // Walk the tree, apply moves, verify final position
        Position pos = game.startPos;
        int moveCount = 0;
        for (auto* node = game.root.get(); node; node = node->next.get()) {
            // Try to find the move from the saved SAN
            Movelist moves;
            pos.legals(moves);
            bool found = false;
            for (Move m : moves) {
                if (uci::moveToSan(pos, m, false, true) == node->san) {
                    pos.doMove(m);
                    found = true;
                    break;
                }
            }
            CHECK(found);
            moveCount++;
        }
        CHECK_EQ(moveCount, 82);
        // Final position should be checkmate for white (black delivered mate)
        CHECK(pos.is_checkmate());
        CHECK(pos.side_to_move() == WHITE);

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Comment after white move (test5.pgn: 1. e4 {....} 1.. e5 *)");

        auto game = parse_game(TEST_DIR "test5.pgn");
        CHECK(game.root != nullptr);
        CHECK_EQ(game.root->san, "e4");
        CHECK_EQ(game.root->comment, "....");
        std::string out = print_game(game);
        CHECK(contains_line(out, "1. e4 {....} e5 *"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("bxa3 pawn capture from FEN (test_bxa3.pgn)");

        auto game = parse_game(TEST_DIR "test_bxa3.pgn");
        std::string out = print_game(game);
        CHECK(contains_line(out, "1. bxa3 e5 *"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("bxc3 pawn capture from FEN (test_bxc3.pgn)");

        auto game = parse_game(TEST_DIR "test_bxc3.pgn");
        std::string out = print_game(game);
        CHECK(contains_line(out, "4. bxc3 dxc3 *"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("NAGs are silently dropped (test_nags.pgn)");

        auto game = parse_game(TEST_DIR "test_nags.pgn");
        std::string out = print_game(game);
        CHECK(contains_line(out, "1. e4 e5"));
        // No NAGs should appear in output
        CHECK(out.find('$') == std::string::npos);
        CHECK(contains_line(out, "1-0"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Variation on white move prints correct side (test_variation_simple.pgn)");

        auto game = parse_game(TEST_DIR "test_variation_simple.pgn");
        std::string out = print_game(game);
        // Variation should show 1. d4 (white move) not 1... d4 (black move)
        CHECK(contains_line(out, "1. e4 (1. d4 d5) e5 *"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Variation on black move prints correct side (test_variation_black.pgn)");

        auto game = parse_game(TEST_DIR "test_variation_black.pgn");
        std::string out = print_game(game);
        CHECK(contains_line(out, "1. e4 e5 (1... d5 2. exd5) 2. Nf3 *"));

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    {
        TEST_CASE("Game constructor from Position sets FEN tag for non-standard");

        Position stdPos;
        pgn::Game g1(stdPos);
        CHECK(g1.tags.empty()); // No FEN tag for standard start

        Position customPos("4k3/8/8/8/8/8/8/4K3 w - - 0 1");
        pgn::Game g2(customPos);
        bool hasFen = false;
        for (auto& [k, v] : g2.tags) {
            if (k == "FEN") hasFen = true;
        }
        CHECK(hasFen);
        CHECK_EQ(g2.startPos.fen(), "4k3/8/8/8/8/8/8/4K3 w - - 0 1");

        FAIL(failures);
    }

    // ---------------------------------------------------------------
    std::cout << "\n==============================" << std::endl;
    if (failures == 0) {
        std::cout << "All tests passed!" << std::endl;
        return 0;
    } else {
        std::cout << failures << " test(s) FAILED!" << std::endl;
        return 1;
    }
}
