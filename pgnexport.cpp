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
#include <cstring>
#include <iostream>
#include <unordered_set>

namespace pgn {

const std::unordered_set<std::string> PGNPrinter::STR_TAGS = { "Event", "Site", "Date", "Round", "White", "Black", "Result" };

Game::Game() = default;

Game::Game(const chess::Position &pos) : startPos(pos) {
    std::string fen = pos.fen();
    if (fen != chess::Position::START_FEN) {
        setTag("FEN", fen);
        setTag("SetUp", "1");
    }
}

/// Set a tag pair, updating both the ordered `tags` vector and the
/// `tagMap` hash table.  If the key already exists its value is replaced
/// in place; otherwise a new entry is appended.
void Game::setTag(std::string_view k, std::string_view v) {
    std::string keyStr(k);
    tagMap[keyStr] = v;

    for (auto &[key, val] : tags) {
        if (key == keyStr) {
            val = v;
            return;
        }
    }
    tags.emplace_back(keyStr, v);
}

/// Reconstruct a full `Game` (including all moves) from a chess::Position
/// by unwinding its history stack with undoMove<true>.
///
/// The moves are replayed forwards after unwinding so the returned
/// Position matches the original.
Game Game::fromPosition(chess::Position &pos) {
    std::vector<chess::Move> moves;

    while (pos.history_count() > 1) {
        auto entry = pos.template undoMove<true>();
        moves.push_back(entry.mv);
    }

    Game game(pos);

    MoveNode *tail = nullptr;
    for (auto it = moves.rbegin(); it != moves.rend(); ++it) {
        chess::Move m = *it;
        std::string san = chess::uci::moveToSan(pos, m, false, true);
        auto node = std::make_unique<MoveNode>();
        node->raw = san;
        node->san = san;
        node->move = m;
        if (tail) {
            tail->next = std::move(node);
            tail = tail->next.get();
        } else {
            game.root = std::move(node);
            tail = game.root.get();
        }
        pos.doMove(m);
    }

    return game;
}

Game PGNBuilder::release() { return std::move(game_); }

/// Stores the tag in the game and, for "FEN" tags, initialises the
/// starting position.
void PGNBuilder::onTag(std::string_view k, std::string_view v) {
    game_.setTag(k, v);
    if (k == "FEN") {
        game_.startPos.setFEN(std::string(v));
    }
}

/// Append a move to the current line.  If inside a variation
/// (branchStack_ non-empty), the move is added to the current variation
/// branch rather than the main line.
void PGNBuilder::onMove(std::string_view san) {
    auto node = std::make_unique<MoveNode>();
    node->raw = std::string(san);
    node->san = node->raw;

    if (branchStack_.empty()) {
        if (tail_) {
            tail_->next = std::move(node);
            tail_ = tail_->next.get();
        } else {
            game_.root = std::move(node);
            tail_ = game_.root.get();
        }
    } else {
        if (branchStack_.empty()) {
            std::cerr << "Error: Variation stack is empty in onMove." << std::endl;
            return;
        }
        auto &branch = branchStack_.back();
        if (branch.varTail) {
            branch.varTail->next = std::move(node);
            branch.varTail = branch.varTail->next.get();
        } else {
            branch.branchPoint->variations.push_back(std::move(node));
            branch.varTail = branch.branchPoint->variations.back().get();
        }
        tail_ = branch.varTail;
    }

    lastNode_ = tail_;
}

/// Attach a comment to the most recently parsed move node.
void PGNBuilder::onComment(std::string_view text) {
    if (lastNode_) {
        lastNode_->comment = text;
    }
}

/// Push a branch marker so subsequent moves go into a variation.
void PGNBuilder::onVariationStart() { branchStack_.push_back({ tail_, nullptr }); }

/// Pop the branch stack and restore the tail to the branch point.
void PGNBuilder::onVariationEnd() {
    if (!branchStack_.empty()) {
        tail_ = branchStack_.back().branchPoint;
        branchStack_.pop_back();
    }
}

/// Store the game result string.
void PGNBuilder::onGameEnd(std::string_view r) { game_.result = r; }

/// NAGs are ignored during export (moves are re-parsed via chesslib).
void PGNBuilder::onNAG(int) {}

PGNPrinter::PGNPrinter(std::ostream &os) : os_(os) {}

/// Parse a move string into a chesslib Move.
///
/// First attempts UCI parsing (fast path for compatible formats).
/// On failure, strips suffix `+`/`#`, normalises `0`→`O` for castling,
/// and disambiguates by comparing SAN against all legal moves.
chess::Move PGNPrinter::parseMove(chess::Position &pos, const std::string &raw) {
    if (raw.size() >= 4) {
        try {
            chess::Move m = chess::uci::uciToMove(pos, raw);
            if (m.is_ok())
                return m;
        } catch (...) {
        }
    }

    std::string clean = raw;
    while (!clean.empty() && (clean.back() == '+' || clean.back() == '#')) {
        clean.pop_back();
    }
    // Normalize 0-0/0-0-0 to O-O/O-O-O (chesslib's moveToSan outputs O)
    for (auto &c : clean) {
        if (c == '0')
            c = 'O';
    }

    chess::Movelist moves;
    pos.legals(moves);
    for (chess::Move m : moves) {
        if (chess::uci::moveToSan(pos, m, false, false) == clean) {
            return m;
        }
    }

    return chess::Move::none();
}

/// Recursively print a move line including move numbers, comments,
/// and variations.  Advances the Position as moves are applied so
/// subsequent SANs are computed from the correct board state.
void PGNPrinter::printNode(MoveNode *node, chess::Position &pos) {
    if (!node)
        return;

    bool first = true;
    while (node) {
        bool white = pos.side_to_move() == chess::WHITE;
        int mn = pos.fullmove_number();

        chess::Position posBefore = pos;
        bool moveOk = node->move.is_ok();
        if (!moveOk) {
            node->move = parseMove(pos, node->raw);
            if (node->move.is_ok()) {
                moveOk = true;
                node->san = chess::uci::moveToSan(pos, node->move, false, true);
                pos.doMove(node->move);
            } else {
                pos = posBefore;
            }
        } else {
            pos.doMove(node->move);
        }

        if (first) {
            if (white)
                os_ << mn << ". " << node->san;
            else
                os_ << mn << "... " << node->san;
            first = false;
        } else if (white) {
            os_ << " " << mn << ". " << node->san;
        } else {
            os_ << " " << node->san;
        }

        if (!node->comment.empty()) {
            os_ << " {" << node->comment << "}";
        }

        for (auto &var : node->variations) {
            os_ << " (";
            chess::Position varPos = moveOk ? posBefore : pos;
            printNode(var.get(), varPos);
            os_ << ")";
        }

        node = node->next.get();
    }
}

std::string PGNPrinter::tagValue(const Game &game, const std::string &key, const std::string &fallback) {
    auto it = game.tagMap.find(key);
    if (it != game.tagMap.end())
        return it->second;
    return fallback;
}

/// Serialise the game to PGN.  Output order:
///   1. Seven STR tag pairs in canonical order (with "Result" from game.result).
///   2. Remaining tags (non-STR) in insertion order.
///   3. Movetext with move numbers, comments, and variations.
///   4. Game result string.
void PGNPrinter::print(const Game &game) {
    // always emit the seven STR tags in standard order
    for (const char *key : STR_ORDER) {
        std::string val;
        if (strcmp(key, "Result") == 0) {
            std::string r = game.result.empty() ? tagValue(game, "Result", "") : game.result;
            val = r.empty() ? "*" : r;
        } else {
            val = tagValue(game, key, "?");
        }
        os_ << "[" << key << " \"" << val << "\"]" << std::endl;
    }

    // remaining tags (non-STR) in insertion order
    for (auto &[k, v] : game.tags) {
        if (STR_TAGS.find(k) == STR_TAGS.end()) {
            os_ << "[" << k << " \"" << v << "\"]" << std::endl;
        }
    }

    chess::Position pos = game.startPos;
    printNode(game.root.get(), pos);

    if (!game.result.empty()) {
        os_ << " " << game.result << std::endl;
    }
}

} // namespace pgn
