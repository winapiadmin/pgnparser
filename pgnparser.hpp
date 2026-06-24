#pragma once

#include "mio.hpp"
#include <cassert>
#include <cstring>
#include <string_view>
namespace pgn {

class PGNVisitor {
  public:
    virtual ~PGNVisitor() = default;

    virtual bool wantsTags() const { return true; }
    virtual void onTag(std::string_view key, std::string_view value) {}
    virtual void onMove(std::string_view san) {}
    virtual void onComment(std::string_view text) {}
    virtual void onVariationStart() { /*track variations*/ }
    virtual void onVariationEnd() {}
    virtual void onGameEnd(std::string_view result) = 0;
    virtual void onNAG(int nag) {}
    virtual bool onGameStart() { return true; }
};

constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }

constexpr bool isWhitespace(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

class PGNInput {
  public:
    explicit PGNInput(std::string_view data) : begin_(data.data()), cur_(data.data()), end_(data.data() + data.size()) {}

    PGNInput(const PGNInput &) = default;
    PGNInput &operator=(const PGNInput &) = default;
    template <typename StringT> explicit PGNInput(StringT filename) {
        std::error_code ec;
        mmap_ = mio::make_mmap_source(filename, ec);
        if (ec)
            throw std::runtime_error(ec.message());

        begin_ = mmap_.data();
        cur_ = begin_;
        end_ = begin_ + mmap_.size();
    }
    char peek() const { return cur_ == end_ ? '\0' : *cur_; }

    bool eof() const { return cur_ == end_; }

    char read() {
        assert(cur_ <= end_ && "Invalid cursor position");
        return cur_ == end_ ? '\0' : *cur_++;
    }

    void skip(size_t n = 1) { cur_ += (n < remaining() ? n : remaining()); }

    void consume(size_t n) { skip(n); }

    bool startsWith(std::string_view text) const {
        return remaining() >= text.size() && std::memcmp(cur_, text.data(), text.size()) == 0;
    }

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

    const char *data() const { return cur_; }

    size_t remaining() const { return static_cast<size_t>(end_ - cur_); }

    size_t offset() const { return static_cast<size_t>(cur_ - begin_); }

    std::string_view remainingView() const { return std::string_view(cur_, remaining()); }

  private:
    mio::mmap_source mmap_;

    const char *begin_;
    const char *cur_;
    const char *end_;
};

class PGNParser {
  public:
    explicit PGNParser(PGNVisitor &visitor);

    void parse(PGNInput &input);
    void parse(std::string_view data) {
        PGNInput in(data);
        parse(in);
    }
    /**
     * Parses all games in the input. Not thread-safe.
     * @param input PGN input to parse.
     */
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
