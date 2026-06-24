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
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector> // Added missing include

struct Event {
    std::string type;
    std::string a, b;
    int nag; // Added for NAG values
};

struct TestVisitor : pgn::PGNVisitor {
    std::vector<Event> events;

    void onTag(std::string_view k, std::string_view v) override {
        events.push_back({ "tag", std::string(k), std::string(v), 0 });
    }

    void onMove(std::string_view san) override { events.push_back({ "move", std::string(san), "", 0 }); }

    void onComment(std::string_view c) override { events.push_back({ "comment", std::string(c), "", 0 }); }

    void onVariationStart() override { events.push_back({ "var_start", "", "", 0 }); }

    void onVariationEnd() override { events.push_back({ "var_end", "", "", 0 }); }

    void onGameEnd(std::string_view r) override { events.push_back({ "result", std::string(r), "", 0 }); }

    void onNAG(int nag) override { events.push_back({ "nag", "", "", nag }); }
};

std::string read_file(std::string path) {
    std::ifstream file(path, std::ios::in | std::ios::binary);
    if (!file)
        return "";

    // Construct the string using stream iterator boundaries
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::string escape_json(std::string_view s) {
    std::string out;

    for (char c : s) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c;
        }
    }

    return out;
}

// Helper to convert NAG number to symbolic string
std::string nag_symbol(int nag) {
    switch (nag) {
    case 1:
        return "!";
    case 2:
        return "?";
    case 3:
        return "!!";
    case 4:
        return "??";
    case 5:
        return "!?";
    case 6:
        return "?!";
    case 7:
        return "\u25A1"; // □
    case 10:
        return "=";
    case 13:
        return "\u2A72"; // ⩲
    case 14:
        return "\u00B1"; // ±
    case 15:
        return "\u2213"; // ∓
    // Add more symbolic representations as needed
    default:
        return "";
    }
}

int main(int argc, char **argv) {
    if (argc == 1) {
        std::cerr << argv[0] << " dumps the PGN file as JSON (see testgen.py for python-chess example)\n";
        std::cerr << argv[0] << " <pgn file>";
        return 1;
    }
    TestVisitor printer;
    pgn::PGNParser parser(printer);
    pgn::PGNInput input((argv[1]));
    parser.parse(input);

    // Constructing JSON
    std::cout << '[';
    bool first = true;
    for (auto &v : printer.events) {
        if (!first)
            std::cout << ',';

        first = false;

        if (v.type == "tag") {
            std::cout << "{\"type\": \"tag\",\"key\": \"" << escape_json(v.a) << "\",\"value\":\"" << escape_json(v.b) << "\"}";
        } else if (v.type == "move") {
            std::cout << "{\"type\": \"move\",\"san\":\"" << escape_json(v.a) << "\"}";
        } else if (v.type == "result") {
            std::cout << "{\"type\": \"result\",\"value\": \"" << escape_json(v.a) << "\"}";
        } else if (v.type == "comment") {
            std::cout << "{\"type\": \"comment\",\"text\": \"" << escape_json(v.a) << "\"}";
        } else if (v.type == "nag") {
            std::string sym = nag_symbol(v.nag);
            std::cout << "{\"type\": \"nag\",\"nag\":" << v.nag;
            if (!sym.empty()) {
                std::cout << ",\"symbol\":\"" << escape_json(sym) << "\"";
            }
            std::cout << "}";
        } else if (v.type == "var_start") {
            std::cout << "{\"type\": \"var_start\"}";
        } else if (v.type == "var_end") {
            std::cout << "{\"type\": \"var_end\"}";
        }
    }
    std::cout << ']' << std::endl; // Added newline for better output
    return 0;
}