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
#pragma once

#include "pgnparser.hpp"
#include <memory>
#include <moves_io.h>
#include <ostream>
#include <position.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pgn {

/// A single node in the move tree (main line plus variations).
struct MoveNode {
    std::string raw;                                   ///< Original unparsed SAN string from file.
    std::string san;                                   ///< Canonical SAN produced by chesslib.
    chess::Move move;                                  ///< Parsed chesslib Move object.
    std::string comment;                               ///< Brace/line comment attached to this move.
    std::unique_ptr<MoveNode> next;                    ///< Next move in the main line (nullptr for last).
    std::vector<std::unique_ptr<MoveNode>> variations; ///< Alternative branches at this node.
};

/// Bookkeeping for variation nesting during PGNBuilder callbacks.
struct Branch {
    MoveNode *branchPoint; ///< The node where the variation splits off.
    MoveNode *varTail;     ///< Last node in the current variation line.
};

/// A complete parsed PGN game with tags, move tree, and result.
struct Game {
    std::vector<std::pair<std::string, std::string>> tags; ///< All tag pairs (ordered).
    std::unordered_map<std::string, std::string> tagMap;   ///< Tag lookup by key.
    std::unique_ptr<MoveNode> root;                        ///< First move node (nullptr for empty game).
    std::string result;                                    ///< Game result ("1-0", "1/2-1/2", "*", …).
    chess::Position startPos;                              ///< Starting position (default or from FEN).

    Game();
    explicit Game(const chess::Position &pos);

    void setTag(std::string_view k, std::string_view v);

    /// Reconstruct a Game from a chess::Position by unwinding its history
    /// via undoMove<true>.
    static Game fromPosition(chess::Position &pos);
};

/// Builds a `Game` object from parser callbacks.
///
/// Pass an instance to `PGNParser::parseAll()` — it accumulates tags,
/// moves, comments, and variations into a `Game` accessible via `release()`.
class PGNBuilder : public PGNVisitor {
  public:
    /// Take ownership of the built game (move semantics).
    Game release();

    void onTag(std::string_view k, std::string_view v) override;
    void onMove(std::string_view san) override;
    void onComment(std::string_view text) override;
    void onVariationStart() override;
    void onVariationEnd() override;
    void onGameEnd(std::string_view result) override;
    void onNAG(int nag) override;

  private:
    Game game_;
    MoveNode *tail_ = nullptr;        ///< Pointer to the last move on the current line.
    MoveNode *lastNode_ = nullptr;    ///< Pointer to the most recent move (for comment attachment).
    std::vector<Branch> branchStack_; ///< Variation nesting stack.
};

/// Serialise a `Game` back to standard PGN text.
///
/// Emits the seven STR tags in canonical order, then remaining tags,
/// then the movetext with move numbers, comments, and variations.
class PGNPrinter {
  public:
    explicit PGNPrinter(std::ostream &os);

    /// Write game in PGN format to the wrapped stream.
    void print(const Game &game);

  private:
    /// Recursively print a move node and its variations from the given position.
    void printNode(MoveNode *node, chess::Position &pos);
    /// Try to interpret `raw` as a chesslib Move.  First attempts UCI parse,
    /// then falls back to legal-move disambiguation by SAN.
    static chess::Move parseMove(chess::Position &pos, const std::string &raw);
    /// Look up a tag value, falling back to `fallback` when missing.
    static std::string tagValue(const Game &game, const std::string &key, const std::string &fallback);

    static constexpr const char *STR_ORDER[7] = { "Event", "Site", "Date", "Round", "White", "Black", "Result" };
    static const std::unordered_set<std::string> STR_TAGS;

    std::ostream &os_;
};

} // namespace pgn
