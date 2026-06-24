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
