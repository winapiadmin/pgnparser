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
#include "pgnexport.hpp"
#include <fstream>
#include <iostream>
#include <string>

static std::string read_file(const std::string &path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file)
        return "";
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int main(int argc, char **argv) {
    if (argc < 2) {
        std::cerr << "usage: pgnexport <pgn file>\n";
        return 1;
    }

    pgn::PGNBuilder builder;
    pgn::PGNParser parser(builder);
    parser.parse(read_file(argv[1]));

    pgn::Game game = builder.release();
    pgn::PGNPrinter printer(std::cout);
    printer.print(game);

    return 0;
}
