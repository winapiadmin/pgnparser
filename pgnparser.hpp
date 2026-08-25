#pragma once

#include "mio.hpp"
#include <cassert>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
namespace pgn {

class PGNVisitor {
  public:
    virtual ~PGNVisitor() = default;
    virtual bool wantsTags() const { return true; }
    virtual void onTag(std::string_view key, std::string_view value) {}
    virtual void onMove(std::string_view san) {}
    virtual void onComment(std::string_view text) {}
    virtual void onVariationStart() {}
    virtual void onVariationEnd() {}
    virtual void onGameEnd(std::string_view result) {}
    virtual void onNAG(int nag) {}
    virtual bool onGameStart() { return true; }
    /// Called when the parser recovers from malformed or pathological
    /// input instead of failing silently — e.g. an unterminated quoted
    /// tag value, or `(...)` variation nesting deep enough that the
    /// parser stops descending and discards the remainder. `raw` is a
    /// short, implementation-defined excerpt describing what was
    /// discarded. Purely informational; parsing continues either way.
    virtual void onError(std::string_view raw) {}
};

constexpr bool isDigit(char c) noexcept { return c >= '0' && c <= '9'; }
constexpr bool isWhitespace(char c) noexcept { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

class PGNInput {
  public:
    explicit PGNInput(std::string_view data)
        : begin_(data.empty() ? &s_empty_ : data.data()), cur_(begin_), end_(begin_ + data.size()) {}

    PGNInput(const PGNInput &) = delete;
    PGNInput &operator=(const PGNInput &) = delete;

    template <typename StringT> explicit PGNInput(StringT filename) {
        std::error_code ec;
        mmap_ = mio::make_mmap_source(filename, ec);
        if (ec)
            throw std::runtime_error(ec.message());
        begin_ = mmap_.data();
        cur_ = begin_;
        end_ = begin_ + mmap_.size();
    }

    inline char peek() const { return cur_ == end_ ? '\0' : *cur_; }
    inline bool eof() const { return cur_ == end_; }

    /// Reset the cursor to the start of the input so the same buffer
    /// (e.g. an mmapped file) can be parsed again without remapping.
    inline void rewind() { cur_ = begin_; }

    inline char read() {
        assert(cur_ <= end_ && "Invalid cursor position");
        return cur_ == end_ ? '\0' : *cur_++;
    }

    inline void skip(size_t n = 1) { cur_ += (n < remaining() ? n : remaining()); }
    inline void consume(size_t n) { skip(n); }

    inline bool startsWith(std::string_view text) const {
        return remaining() >= text.size() && std::memcmp(cur_, text.data(), text.size()) == 0;
    }

    inline void skipWhitespace() {
        while (cur_ != end_) {
            unsigned char c = static_cast<unsigned char>(*cur_);
            if (c > 0x20)
                break;
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                break;
            ++cur_;
        }
    }

    inline const char *data() const { return cur_; }
    inline const char *end() const { return end_; }
    inline const char *begin() const { return begin_; }
    inline size_t remaining() const { return static_cast<size_t>(end_ - cur_); }
    inline size_t offset() const { return static_cast<size_t>(cur_ - begin_); }
    inline std::string_view remainingView() const { return std::string_view(cur_, remaining()); }

  private:
    inline static const char s_empty_ = '\0';
    mio::mmap_source mmap_;
    const char *begin_;
    const char *cur_;
    const char *end_;
};

class PGNParser {
  public:
    PGNParser(PGNVisitor &visitor) : visitor_(visitor) {}

    void parse(PGNInput &input) {
        input_ = &input;
        parseTagSection();
        if (input_->eof())
            return;
        if (visitor_.onGameStart()) {
            parseMovetext();
        } else {
            skipMovetext();
            visitor_.onGameEnd({});
        }
    }
    void parse(std::string_view data) {
        PGNInput in(data);
        parse(in);
    }

    void parseAll(PGNInput &input);
    void parseAll(std::string_view data) {
        PGNInput in(data);
        parseAll(in);
    }

  public:
    /// Maximum nesting depth for `(...)` variations before the parser
    /// stops recursing and iteratively flattens the remainder instead.
    /// Real annotated games rarely nest more than a handful of levels;
    /// this exists to bound stack usage against pathological/adversarial
    /// input (e.g. tens of thousands of nested parens), not to limit
    /// normal use.
    static constexpr size_t kMaxVariationDepth = 64;

  private:
    std::string tagValue_;
    size_t variationDepth_ = 0;
    void parseLineComment();
    void parseTagSection();
    void parseTag();
    template<bool inVariation=false>
    void parseMovetext();
    void skipMovetext();
    void parseComment();
    /// Iteratively consumes the body of a variation whose matching `)`
    /// hasn't been found yet, without recursing. Used once
    /// `kMaxVariationDepth` is reached so stack usage stays bounded
    /// regardless of how much deeper the input actually nests — the
    /// content in that region is discarded (no onMove/onComment calls)
    /// rather than risking a stack overflow to preserve it.
    void skipVariationBody();
    void parseNAG();
    bool checkEndOfGame(const char *base, const char *end, const char *p, char c);
    std::string_view readMove();
    int readNumber();

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
