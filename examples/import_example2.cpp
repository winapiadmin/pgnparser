/*
    An extraordinary fast PGN parser with mmap-based file loading for
    large files and from existing buffer.
    Copyright (C) 2026  winapiadmin

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published
    by the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/
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
    void onNAG(int nag) override { nags++; }
    void onGameEnd(std::string_view result) override { outcomes++; }
    bool onGameStart() override {
        games++;
        return true;
    }
};


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
    printf("Time = %f\n", elapsed.count());
    printf("Speed = %f games/s, or %f MB/s", printer.games / elapsed.count(), (input.end() - input.begin()) / elapsed.count()/1048576);

    return 0;
}