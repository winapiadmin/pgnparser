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

struct MoveNode {
    std::string raw;
    std::string san;
    chess::Move move;
    std::string comment;
    std::unique_ptr<MoveNode> next;
    std::vector<std::unique_ptr<MoveNode>> variations;
};

struct Branch {
    MoveNode *branchPoint;
    MoveNode *varTail;
};

struct Game {
    std::vector<std::pair<std::string, std::string>> tags;
    std::unordered_map<std::string, std::string> tagMap;
    std::unique_ptr<MoveNode> root;
    std::string result;
    chess::Position startPos;

    Game();
    explicit Game(const chess::Position &pos);

    void setTag(std::string_view k, std::string_view v);

    /// Extract game history from a Position via undoMove<true>.
    static Game fromPosition(chess::Position &pos);
};

class PGNBuilder : public PGNVisitor {
  public:
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
    MoveNode *tail_ = nullptr;
    MoveNode *lastNode_ = nullptr;

    std::vector<Branch> branchStack_;
};

class PGNPrinter {
  public:
    explicit PGNPrinter(std::ostream &os);

    void print(const Game &game);

  private:
    void printNode(MoveNode *node, chess::Position &pos);
    static chess::Move parseMove(chess::Position &pos, const std::string &raw);
    static std::string tagValue(const Game &game, const std::string &key, const std::string &fallback);

    static constexpr const char *STR_ORDER[7] = { "Event", "Site", "Date", "Round", "White", "Black", "Result" };
    static const std::unordered_set<std::string> STR_TAGS;

    std::ostream &os_;
};

} // namespace pgn
