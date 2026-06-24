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

#include "mio.hpp"
#include <cassert>
#include <cstring>
#include <string_view>
namespace pgn {

/// Callback interface for PGN parsing events.
///
/// Override the virtual methods to receive parsed PGN data.  The callbacks
/// are invoked in document order during `PGNParser::parseAll()`.
class PGNVisitor {
  public:
    virtual ~PGNVisitor() = default;

    /// Override and return false to skip tag parsing entirely (count-only mode).
    /// When false, `onTag()` is never called and tag sections are batch-skipped.
    virtual bool wantsTags() const { return true; }
    /// Called for each tag pair, e.g. onTag("Event", "World Championship").
    virtual void onTag(std::string_view key, std::string_view value) {}
    /// Called for each SAN move string, e.g. onMove("e4").
    virtual void onMove(std::string_view san) {}
    /// Called for brace and semicolon comments (leading/trailing whitespace trimmed).
    virtual void onComment(std::string_view text) {}
    /// Called when a variation (parenthesised line) begins.
    virtual void onVariationStart() {}
    /// Called when the current variation ends.
    virtual void onVariationEnd() {}
    /// Called at game end with the result string ("1-0", "1/2-1/2", "*", …)
    virtual void onGameEnd(std::string_view result) = 0;
    /// Called for NAG (Numerical Annotation Glyph) values.
    virtual void onNAG(int nag) {}
    /// Called before each game starts.  Return false to skip parsing its
    /// movetext (tag section is still parsed when wantsTags() is true).
    virtual bool onGameStart() { return true; }
};

/// @return true when `c` is an ASCII digit ('0'–'9').
constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

/// @return true when `c` is a PGN whitespace character (space, tab, newline, CR).
constexpr bool isWhitespace(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/// Memory-mapped or buffer-backed input stream for PGN data.
///
/// Construct with a `std::string_view` for in-memory data or with a filename
/// to mmap a file.  Provides forward-only cursor access (read / consume) with
/// no allocation in the hot path.
class PGNInput {
  public:
    /// Construct from an in-memory buffer (no copy, no mmap).
    explicit PGNInput(std::string_view data) : begin_(data.data()), cur_(data.data()), end_(data.data() + data.size()) {}

    PGNInput(const PGNInput &) = default;
    PGNInput &operator=(const PGNInput &) = default;

    /// Memory-map the given file and position the cursor at the beginning.
    template <typename StringT> explicit PGNInput(StringT filename) {
        std::error_code ec;
        mmap_ = mio::make_mmap_source(filename, ec);
        if (ec)
            throw std::runtime_error(ec.message());
        begin_ = mmap_.data();
        cur_ = begin_;
        end_ = begin_ + mmap_.size();
    }

    /// Return the current byte without advancing, or '\0' at EOF.
    char peek() const { return cur_ == end_ ? '\0' : *cur_; }

    /// @return true when the cursor is at the end of input.
    bool eof() const { return cur_ == end_; }

    /// Read and advance by one byte.  Returns '\0' at EOF.
    char read() {
        assert(cur_ <= end_ && "Invalid cursor position");
        return cur_ == end_ ? '\0' : *cur_++;
    }

    /// Advance the cursor by `n` bytes (clamped to remaining input).
    void skip(size_t n = 1) { cur_ += (n < remaining() ? n : remaining()); }

    /// Alias for skip().
    void consume(size_t n) { skip(n); }

    /// @return true when the remaining input begins with the given text.
    bool startsWith(std::string_view text) const {
        return remaining() >= text.size() && std::memcmp(cur_, text.data(), text.size()) == 0;
    }

    /// Skip ASCII whitespace bytes (space, tab, \\n, \\r) at the cursor.
    void skipWhitespace() {
        while (cur_ != end_) {
            unsigned char c = static_cast<unsigned char>(*cur_);
            if (c > 0x20)
                break;
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++cur_;
        }
    }

    /// @return pointer to the current cursor position into the underlying buffer.
    const char *data() const { return cur_; }

    /// @return number of bytes remaining from cursor to end.
    size_t remaining() const { return static_cast<size_t>(end_ - cur_); }

    /// @return absolute byte offset of the cursor from the start of input.
    size_t offset() const { return static_cast<size_t>(cur_ - begin_); }

    /// @return a view of the remaining input from cursor to end.
    std::string_view remainingView() const { return std::string_view(cur_, remaining()); }

  private:
    mio::mmap_source mmap_;

    const char *begin_;
    const char *cur_;
    const char *end_;
};

/// Zero-allocation PGN parser that drives a `PGNVisitor`.
///
/// Parsing is done via `parse()` (single game) or `parseAll()` (all games
/// in the input).  The visitor receives callbacks in document order.
class PGNParser {
  public:
    explicit PGNParser(PGNVisitor &visitor);

    /// Parse a single game (tag section + movetext).  Not thread-safe.
    void parse(PGNInput &input);
    void parse(std::string_view data) {
        PGNInput in(data);
        parse(in);
    }

    /// Parse all games in the input.  Calls `onGameStart()` before each game
    /// so the visitor can skip movetext parsing (count-only mode).
    /// Not thread-safe.
    void parseAll(PGNInput &input);
    void parseAll(std::string_view data) {
        PGNInput in(data);
        parseAll(in);
    }

  private:
    std::string tagValue_;

    void parseLineComment();
    void parseTagSection();
    void parseTag();
    void parseMovetext(bool inVariation = false);
    void skipMovetext();
    void parseComment();
    void parseVariation();
    void parseResult();
    void parseNAG();
    bool checkEndOfGame(const char *base, const char *end, const char *p, char c);
    std::string_view readMove();
    int readNumber();

    inline void skipWhitespace() { input_->skipWhitespace(); }

    inline void consume(char expected) {
        if (!input_->eof() && input_->peek() == expected)
            input_->read();
    }

    inline void consume(std::string_view text) {
        if (input_->startsWith(text))
            input_->skip(text.size());
    }

    PGNVisitor &visitor_;
    PGNInput *input_ = nullptr;
};

} // namespace pgn
