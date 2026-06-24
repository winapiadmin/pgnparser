#include "pgnparser.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
struct TestVisitor : pgn::PGNVisitor {
    int games = 0, sans = 0, nags = 0, outcomes = 0, variations = 0, comments = 0, tags = 0;
    void onTag(std::string_view key, std::string_view value) override { tags++; }
    void onMove(std::string_view san) override { sans++; }
    void onComment(std::string_view text) override { comments++; }
    void onVariationEnd() override { variations++; }
    void onGameEnd(std::string_view result) override { outcomes++; }
    void onNAG(int nag) override { nags++; }
    bool onGameStart() override {
        games++;
        return true;
    }
};

std::string read_file(std::string path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file)
        return "";

    // Construct the string using stream iterator boundaries
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv) {
    if (argc == 1) {
        std::cerr << argv[0] << " counts games\n";
        std::cerr << argv[0] << " <pgn file>";
        return 1;
    }
    TestVisitor printer;
    pgn::PGNParser parser(printer);
    auto start = std::chrono::steady_clock::now();
    pgn::PGNInput input((argv[1]));
    parser.parseAll(input);
    auto end = std::chrono::steady_clock::now();
    std::chrono::duration<double> elapsed = end - start;
    printf("Stats: { games: %d, tags: %d, sans: %d, nags: %d, "
           "comments: %d, variations: %d, outcomes: %d}\n",
           printer.games,
           printer.tags,
           printer.sans,
           printer.nags,
           printer.comments,
           printer.variations,
           printer.outcomes);
    std::cout << "Time = " << elapsed.count() << " s\n";
    std::cout << "Speed = " << (printer.games / elapsed.count()) << " games/s\n";
    return 0;
}