#include "pgnparser.hpp"
#include <algorithm>
#include <cstring>

namespace pgn {
static inline int ctz(uint64_t x) {
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanForward64(&index, x);
    return static_cast<int>(index);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(x);
#else
    static const int MultiplyDeBruijnBitPosition[32] = { 0,  1,  28, 2,  29, 14, 24, 3, 30, 22, 20, 15, 25, 17, 4,  8,
                                                         31, 27, 13, 23, 21, 19, 16, 7, 26, 12, 18, 6,  11, 5,  10, 9 };

    if (v == 0)
        return 32;

    return MultiplyDeBruijnBitPosition[((uint32_t)((v & -v) * 0x077CB531UL)) >> 27];
#endif
}
// Helper: convert '!' and '?' sequence to NAG number (no allocation)
static int nagFromSymbols(const char *sym, size_t len) {
    if (len == 2) {
        if (sym[0] == '!' && sym[1] == '!')
            return 3;
        if (sym[0] == '!' && sym[1] == '?')
            return 5;
        if (sym[0] == '?' && sym[1] == '!')
            return 6;
        if (sym[0] == '?' && sym[1] == '?')
            return 4;
    } else if (len == 1) {
        if (sym[0] == '!')
            return 1;
        if (sym[0] == '?')
            return 2;
    }
    return 0;
}

PGNParser::PGNParser(PGNVisitor &visitor) : visitor_(visitor) {}

void PGNParser::parse(PGNInput &input) {
    input_ = &input;
    parseTagSection();
    parseMovetext(false);
}

void PGNParser::parseAll(PGNInput &input) {
    input_ = &input;
    while (!input_->eof()) {
        parseTagSection();
        if (input_->eof())
            break;
        if (visitor_.onGameStart()) {
            parseMovetext(false);
        } else {
            skipMovetext();
            visitor_.onGameEnd(std::string_view());
        }
    }
}

void PGNParser::parseTagSection() {
    if (!visitor_.wantsTags()) {
        // Batch-skip entire tag section
        const char *p = input_->data();
        const char *end = p + input_->remaining();
        while (true) {
            while (p < end) {
                unsigned char c = static_cast<unsigned char>(*p);
                if (c > 0x20)
                    break;
                if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
                    break;
                ++p;
            }
            if (p >= end || *p != '[')
                break;
            ++p; // skip '['
            const void *q = std::memchr(p, ']', static_cast<size_t>(end - p));
            if (q) {
                p = static_cast<const char *>(q) + 1;
            } else {
                p = end;
                break;
            }
        }
        input_->consume(static_cast<size_t>(p - input_->data()));
        return;
    }
    while (true) {
        skipWhitespace();
        if (input_->eof() || input_->peek() != '[')
            break;
        parseTag();
    }
}

void PGNParser::parseNAG() {
    consume('$'); // consume leading '$'

    // Read first 2 chars at once to avoid repeated startsWith() calls
    char c1 = input_->peek();
    char c2 = (c1 != '\0' && input_->remaining() > 1) ? input_->data()[1] : '\0';

    if (c1 == '!' && c2 == '!') {
        visitor_.onNAG(3);
        input_->skip(2);
    } else if (c1 == '!' && c2 == '?') {
        visitor_.onNAG(5);
        input_->skip(2);
    } else if (c1 == '?' && c2 == '!') {
        visitor_.onNAG(6);
        input_->skip(2);
    } else if (c1 == '?' && c2 == '?') {
        visitor_.onNAG(4);
        input_->skip(2);
    } else if (c1 == '!') {
        visitor_.onNAG(1);
        input_->skip(1);
    } else if (c1 == '?') {
        visitor_.onNAG(2);
        input_->skip(1);
    } else {
        int nag = readNumber();
        if (nag != 0) {
            visitor_.onNAG(nag);
        }
    }
}

void PGNParser::parseTag() {
    consume('[');
    skipWhitespace();

    // ---------- Key (direct pointer access) ----------
    const char *p = input_->data();
    const char *end = p + input_->remaining();
    const char *keyStart = p;

    while (p < end) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c <= 0x20 || c == ']')
            break;
        ++p;
    }

    input_->consume(static_cast<size_t>(p - keyStart));
    std::string_view key(keyStart, static_cast<size_t>(p - keyStart));

    skipWhitespace();
    p = input_->data();
    end = p + input_->remaining();

    if (p >= end || *p != '"') {
        const char *q = p;
        while (q < end && *q != ']')
            ++q;
        if (q < end)
            ++q;
        input_->consume(static_cast<size_t>(q - p));
        return;
    }

    input_->read(); // opening quote

    // ---------- Value ----------
    const char *valueStart = input_->data();
    bool escaped = false;

    p = valueStart;
    end = p + input_->remaining();

    while (p < end) {
        if (*p == '\\') {
            escaped = true;
            p += 2; // skip '\' and the escaped char
            continue;
        }
        if (*p == '"')
            break;
        ++p;
    }

    input_->consume(static_cast<size_t>(p - valueStart));
    const char *valueEnd = p;

    if (!escaped) {
        std::string_view value(valueStart, static_cast<size_t>(valueEnd - valueStart));

        if (!input_->eof())
            input_->read(); // closing quote

        skipWhitespace();

        if (!input_->eof() && input_->peek() == ']')
            input_->read();

        visitor_.onTag(key, value);
        return;
    }

    // ---------- Optimized unescaping ----------
    tagValue_.clear();
    tagValue_.reserve(static_cast<size_t>(valueEnd - valueStart));

    for (const char *p = valueStart; p < valueEnd; ++p) {
        if (*p == '\\' && p + 1 < valueEnd) {
            ++p; // Skip escape character
        }
        tagValue_.push_back(*p);
    }

    if (!input_->eof())
        input_->read(); // closing quote

    skipWhitespace();

    if (!input_->eof() && input_->peek() == ']')
        input_->read();

    visitor_.onTag(key, tagValue_);
}
namespace {
// Fast-forward past whitespace characters using a single-byte lookup
static inline const char *skipWhitespace(const char *p, const char *end) noexcept {
    while (p < end) {
        unsigned char c = static_cast<unsigned char>(*p);
        // Fast check: all 4 whitespace chars are <= ' ' (0x20)
        if (c > 0x20)
            break;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        ++p;
    }
    return p;
}

} // namespace
bool PGNParser::checkEndOfGame(const char *base, const char *end, const char *p, char c) {
    const size_t rem = static_cast<size_t>(end - p);

    switch (c) {
    case '*': {
        size_t len = static_cast<size_t>(p - base);
        input_->consume(len);
        visitor_.onGameEnd(std::string_view(base, len));
        return true;
    }
    case '1':
        if (rem >= 2 && p[0] == '-' && p[1] == '0') {
            size_t len = static_cast<size_t>(p + 2 - base);
            input_->consume(len);
            visitor_.onGameEnd(std::string_view(base, len));
            return true;
        }
        if (rem >= 6 && p[0] == '/' && p[1] == '2' && p[2] == '-' && p[3] == '1' && p[4] == '/' && p[5] == '2') {
            size_t len = static_cast<size_t>(p + 6 - base);
            input_->consume(len);
            visitor_.onGameEnd(std::string_view(base, len));
            return true;
        }
        break;
    case '0':
        if (rem >= 2 && p[0] == '-' && p[1] == '1') {
            size_t len = static_cast<size_t>(p + 2 - base);
            input_->consume(len);
            visitor_.onGameEnd(std::string_view(base, len));
            return true;
        }
        break;
    default:
        if (rem >= 3) {
            if (p[0] == '1' && p[1] == '-' && p[2] == '0') {
                size_t len = static_cast<size_t>(p + 3 - base);
                input_->consume(len);
                visitor_.onGameEnd(std::string_view(base, len));
                return true;
            }
            if (p[0] == '0' && p[1] == '-' && p[2] == '1') {
                size_t len = static_cast<size_t>(p + 3 - base);
                input_->consume(len);
                visitor_.onGameEnd(std::string_view(base, len));
                return true;
            }
        }
        if (rem >= 7 && p[0] == '1' && p[1] == '/' && p[2] == '2' && p[3] == '-' && p[4] == '1' && p[5] == '/' && p[6] == '2') {
            size_t len = static_cast<size_t>(p + 7 - base);
            input_->consume(len);
            visitor_.onGameEnd(std::string_view(base, len));
            return true;
        }
        break;
    }
    return false;
}
void PGNParser::parseMovetext(bool inVariation) {
    while (true) {
        const char *base = input_->data();
        const char *end = base + input_->remaining();
        const char *p = pgn::skipWhitespace(base, end);

        input_->consume(static_cast<size_t>(p - base));
        if (input_->eof())
            break;

        base = input_->data();
        end = base + input_->remaining();
        char c = *base;

        if (checkEndOfGame(base, end, base + 1, c))
            return;

        switch (c) {
        case '{':
            parseComment();
            break;

        case ';':
            parseLineComment();
            break;

        case '(':
            visitor_.onVariationStart();
            input_->consume(1);
            parseMovetext(true);
            break;

        case ')':
            if (inVariation) {
                visitor_.onVariationEnd();
                input_->consume(1);
                return;
            }
            input_->consume(1);
            break;

        case '$':
            parseNAG();
            break;

        case '!':
        case '?': {
            const char *p = base;
            size_t len = 0;

            while (len < 2 && p < end) {
                char ch = *p;
                if (ch != '!' && ch != '?')
                    break;
                ++p;
                ++len;
            }

            input_->consume(len);

            int nag = nagFromSymbols(base, len);
            if (nag != 0)
                visitor_.onNAG(nag);
            break;
        }

        case 'O': // correct SAN castling
        case '0': {
            std::string_view san = readMove();
            if (!san.empty())
                visitor_.onMove(san);
            break;
        }

        default:
            if (pgn::isDigit(c)) {
                // Consume move number and trailing dots in one go.
                while (!input_->eof() && pgn::isDigit(input_->peek()))
                    input_->read();

                while (!input_->eof() && input_->peek() == '.')
                    input_->read();
            } else {
                std::string_view san = readMove();
                if (!san.empty())
                    visitor_.onMove(san);
                else
                    input_->read(); // hard fallback
            }
            break;
        }
    }
}

void PGNParser::skipMovetext() {
    const char *base = input_->data();
    const char *end = base + input_->remaining();
    const char *p = base;

    while (p < end) {
        while (end - p >= 8) {
            uint64_t w;
            std::memcpy(&w, p, sizeof(w));

            uint64_t t1 = w ^ 0x0A0A0A0A0A0A0A0AULL;
            uint64_t m1 = (t1 - 0x0101010101010101ULL) & ~t1;

            uint64_t t2 = w ^ 0x7B7B7B7B7B7B7B7BULL;
            uint64_t m2 = (t2 - 0x0101010101010101ULL) & ~t2;

            uint64_t t3 = w ^ 0x3B3B3B3B3B3B3B3BULL;
            uint64_t m3 = (t3 - 0x0101010101010101ULL) & ~t3;

            uint64_t m = (m1 | m2 | m3) & 0x8080808080808080ULL;

            if (m) {
                p += ctz(m) >> 3;
                break;
            }
            p += 4;
        }
        // does the above loop but slower, for the last few bytes
        while (p < end) {
            if (*p == '\n' || *p == '{' || *p == ';')
                break;
            ++p;
        }
        if (p >= end)
            break;

    found:
        char c = *p++;

        if (c == '{') {
            while (end - p >= 8) {
                uint64_t w;
                std::memcpy(&w, p, sizeof(w));
                uint64_t t1 = w ^ 0x7D7D7D7D7D7D7D7DULL;
                uint64_t m1 = (t1 - 0x0101010101010101ULL) & ~t1;
                uint64_t m = (m1 & 0x8080808080808080ULL);
                if (m) {
                    p += ctz(m) >> 3;
                    break;
                }
                p += 4;
            }
            // does the above loop but slower, for the last few bytes
            while (p < end && *p != '}')
                ++p;
            if (p < end)
                ++p;
        } else if (c == ';') {
            while (p < end && *p != '\n')
                ++p;
            if (p < end)
                ++p;
        } else if (c == '\n') {
            if (p < end && *p == '\r')
                ++p;
            const char *s = p;
            while (s < end && (*s == ' ' || *s == '\t'))
                ++s;
            if (s < end && (*s == '\n' || *s == '[')) {
                input_->consume(static_cast<size_t>(p - base));
                return;
            }
        }
    }

    input_->consume(static_cast<size_t>(p - base));
}

void PGNParser::parseComment() {
    consume('{');

    const char *start = input_->data();
    const char *end = start + input_->remaining();
    const char *q = start;
    while (q < end && *q != '}')
        ++q;
    input_->consume(static_cast<size_t>(q - start));
    if (!input_->eof() && input_->peek() == '}')
        input_->read();

    std::string_view sv(start, static_cast<size_t>(q - start));
    size_t left = 0;
    size_t right = sv.size();

    while (left < right) {
        unsigned char c = static_cast<unsigned char>(sv[left]);
        if (c > 0x20)
            break;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        ++left;
    }

    while (right > left) {
        unsigned char c = static_cast<unsigned char>(sv[right - 1]);
        if (c > 0x20)
            break;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        --right;
    }

    visitor_.onComment(sv.substr(left, right - left));
}
void PGNParser::parseLineComment() {
    consume(';');

    const char *start = input_->data();
    const char *end = start + input_->remaining();
    const char *q = start;
    while (q < end && *q != '\n')
        ++q;
    input_->consume(static_cast<size_t>(q - start));
    if (!input_->eof() && input_->peek() == '\n')
        input_->read();
    if (!input_->eof() && input_->peek() == '\r')
        input_->read();

    std::string_view sv(start, static_cast<size_t>(q - start));
    size_t left = 0;
    size_t right = sv.size();

    while (left < right) {
        unsigned char c = static_cast<unsigned char>(sv[left]);
        if (c > 0x20)
            break;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        ++left;
    }

    while (right > left) {
        unsigned char c = static_cast<unsigned char>(sv[right - 1]);
        if (c > 0x20)
            break;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        --right;
    }

    visitor_.onComment(sv.substr(left, right - left));
}

void PGNParser::parseVariation() {
    consume('(');
    visitor_.onVariationStart();
    parseMovetext(true);
    if (!input_->eof() && input_->peek() == ')')
        input_->read(); // consume ')'
    visitor_.onVariationEnd();
}

void PGNParser::parseResult() {
    checkEndOfGame(input_->data(), input_->data() + input_->remaining(), input_->data(), input_->peek());
}

std::string_view PGNParser::readMove() {
    const char *start = input_->data();
    const char *p = start;
    const char *end = start + input_->remaining();
    constexpr size_t MAX_MOVE_LENGTH = 15;

    while (p < end && static_cast<size_t>(p - start) < MAX_MOVE_LENGTH) {
        unsigned char c = static_cast<unsigned char>(*p);
        // Fast ASCII range check for move characters (a-z, A-Z, 0-9, '-', '+', '#', '=')
        if (c > 0x7A)
            break;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            break;
        if (c == '{' || c == '}' || c == '(' || c == ')' || c == ';')
            break;
        if (c == '$' || c == '!' || c == '?')
            break;
        ++p;
    }

    size_t len = static_cast<size_t>(p - start);
    input_->consume(len);
    return { start, len };
}

int PGNParser::readNumber() {
    int num = 0;
    const char *p = input_->data();
    const char *end = p + input_->remaining();

    while (p < end) {
        unsigned d = static_cast<unsigned>(*p) - '0';
        if (d > 9)
            break;
        num = num * 10 + static_cast<int>(d);
        ++p;
    }

    input_->consume(static_cast<size_t>(p - input_->data()));
    return num;
}

} // namespace pgn