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
#include <algorithm>
#include <climits>
#include <cstring>

namespace pgn {

/// Count trailing zeros in a 64-bit word.
///
/// Wraps platform intrinsics (`_BitScanForward64` on MSVC,
/// `__builtin_ctzll` on GCC/Clang) with a De Bruijn fallback.
/// Used by the SWAR delimiter scanner to find the byte offset
/// of the first matching delimiter.
static inline int ctz(uint64_t x) {
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanForward64(&index, x);
    return static_cast<int>(index);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(x);
#else
    static const int MultiplyDeBruijnBitPosition[64] = {
        0,  1,  2,  53, 3,  7,  54, 27, 4,  38, 41, 8,  34, 55, 48, 28, 62, 5,  39, 46, 44, 42,
        22, 9,  24, 35, 59, 56, 49, 18, 29, 11, 63, 52, 6,  26, 37, 40, 33, 47, 61, 45, 43, 21,
        23, 58, 17, 10, 51, 25, 36, 32, 60, 20, 57, 16, 50, 31, 19, 15, 30, 14, 13, 12,
    };

    if (x == 0)
        return 64;

    return MultiplyDeBruijnBitPosition[((uint64_t)((x & -x) * 0x03F79D71B4CB0A89ULL)) >> 58];
#endif
}

/// Convert 1–2 character "!?" annotation to NAG code.
///
///   "!"  → 1,  "?"  → 2,  "!!" → 3,  "?!" → 6,
///   "!?" → 5,  "??" → 4,  else → 0.
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

/// Parse one game: tag section then movetext.
/// Calls `onTag()`, `onMove()`, `onComment()`, … via the visitor.
void PGNParser::parse(PGNInput &input) {
    input_ = &input;
    parseTagSection();
    parseMovetext(false);
}

/// Parse all games in the input until EOF.
///
/// When `visitor_.onGameStart()` returns false, the movetext is skipped
/// using the fast `skipMovetext()` path (no per-move callbacks).  This
/// enables counting games at ~1.2M games/sec on typical hardware.
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

/// Parse (or skip) the tag section at the current input position.
///
/// Two code paths:
///   - **Skip** (`wantsTags() == false`): batch-scan for `[` … `]` fences
///     with a single `memchr` per tag — no key/value parsing, zero callbacks.
///   - **Full parse**: calls `parseTag()` for each tag, which invokes
///     `visitor_.onTag()`.
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

/// Parse a NAG (Numerical Annotation Glyph) — either as `$nnn` or
/// as the inline symbols `!`/`?`/`!!`/`!?`/`?!`/`??`.
///
/// Standard NAG values (1–6) for inline symbols are hard-coded;
/// numeric NAGs are parsed via `readNumber()`.
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

/// Parse a single `[Key "Value"]` tag pair.
///
/// Handles quoted values with backslash escaping.  When no escape
/// sequences are present, the value is passed as a `std::string_view`
/// directly into the mapped buffer (zero copy).  Escaped values
/// are unescaped into the internal `tagValue_` string.
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

/// Check if the byte at `p` begins a game-termination marker and, if so,
/// consume the input up to and including it.
///
/// Recognised results: `*`, `1-0`, `0-1`, `1/2-1/2`.
/// Uses a char dispatch table (`case '*'`, `case '1'`, `case '0'`) with
/// direct memcmp-length checks to avoid branching on the full string.
/// @return true when a result marker was found and consumed.
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
/// Parse the movetext section, dispatching each token to the visitor.
///
/// Handles: SAN moves, NAGs (`$` + `!?`), brace comments `{…}`,
/// semicolon line comments `;…`, variations `(…)`, move numbers,
/// and game-termination markers.  Recurses for nested variations.
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

/*
 * Skip movetext section to find the next game boundary.
 *
 * Uses SWAR (Sub-Word Parallelism) to scan 8 bytes at a time for three
 * delimiter bytes (\n, {, ;) using only scalar integer arithmetic:
 *
 *   For each delimiter D, XOR the 8-byte word with DDDDDDDD so that
 *   matching bytes become 0x00.  The "has-zero-byte" formula:
 *
 *       mask = ((x - 0x0101010101010101) & ~x) & 0x8080808080808080
 *
 *   sets the MSB of each byte position where x == 0x00.  OR the masks
 *   for all three delimiters, then use ctz() to find the first matching
 *   byte offset.
 *
 * This avoids 3 comparisons and 1 branch per byte (original loop) at the
 * cost of ~15 ALU ops per 8 bytes — about 4× fewer ops overall.
 */
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
            p += 8;
        }
        while (p < end) {
            if (*p == '\n' || *p == '{' || *p == ';')
                break;
            ++p;
        }
        if (p >= end)
            break;

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
                p += 8;
            }
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

/// Parse a brace comment `{…}` and pass the trimmed text to the visitor.
/// Leading/trailing whitespace inside the braces is stripped.
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
/// Parse a semicolon line comment `;…` until the next newline.
/// Leading/trailing whitespace is stripped from the comment text
/// before it is passed to the visitor.
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

/// Parse a parenthesised variation `(…)`.  Calls the visitor's
/// onVariationStart/onVariationEnd and recursively invokes
/// parseMovetext with `inVariation = true`.
void PGNParser::parseVariation() {
    consume('(');
    visitor_.onVariationStart();
    parseMovetext(true);
    if (!input_->eof() && input_->peek() == ')')
        input_->read(); // consume ')'
    visitor_.onVariationEnd();
}

/// Consume the game-termination marker at the current position.
/// Delegates to `checkEndOfGame()`.
void PGNParser::parseResult() {
    checkEndOfGame(input_->data(), input_->data() + input_->remaining(), input_->data(), input_->peek());
}

/// Read a SAN (Standard Algebraic Notation) move string from the input.
///
/// Stops at whitespace, comment/variation delimiters, NAG markers,
/// or after `MAX_MOVE_LENGTH` characters (15).  Returns a view into
/// the underlying buffer — no copying.
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

/// Parse a non-negative decimal integer from the current input position.
/// Advances the cursor past all consecutive digit characters.
int PGNParser::readNumber() {
    int num = 0;
    const char *p = input_->data();
    const char *end = p + input_->remaining();

    while (p < end) {
        unsigned d = static_cast<unsigned>(*p) - '0';
        if (d > 9)
            break;
        if (num > (INT_MAX - static_cast<int>(d)) / 10)
            break;
        num = num * 10 + static_cast<int>(d);
        ++p;
    }

    input_->consume(static_cast<size_t>(p - input_->data()));
    return num;
}

} // namespace pgn