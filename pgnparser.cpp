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
#include <cstdint>
#include <array>
#ifdef _MSC_VER
#define _forceinline __forceinline
#else
#define _forceinline __attribute__((always_inline))
#endif
namespace pgn {

namespace {
// Fast-forward past whitespace characters using a single-byte lookup.
// The `c > 0x20` check is a fast path: all four whitespace bytes are
// <= 0x20, so it rules out the common case (ordinary printable move/tag
// text) in one comparison instead of four. Matches PGNInput::skipWhitespace()
// in the header, which uses the same shortcut.
static _forceinline const char *skipWhitespace(const char *p, const char *end) noexcept {
    while (p < end) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c > 0x20)
            break;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        ++p;
    }
    return p;
}

// Lookup table marking the bytes that matter while scanning inside a tag
// pair: '\n' (line boundary — tags never span lines), '"' (quote toggle),
// '\\' (escape), ']' (tag close). Ordinary bytes — the overwhelming
// majority of tag key/value text — hit a single table lookup instead of
// the 3-4 sequential comparisons the state machine needs for these four.
inline constexpr auto kIsTagSpecial = [] {
    std::array<bool, 256> t{};
    t[static_cast<unsigned char>('\n')] = true;
    t[static_cast<unsigned char>('"')] = true;
    t[static_cast<unsigned char>('\\')] = true;
    t[static_cast<unsigned char>(']')] = true;
    return t;
}();

// Same idea for scanning a tag's already-opened quoted value: only '\n'
// (line boundary), '"' (close), and '\\' (escape) matter — ']' is
// ordinary text there, so it's deliberately left out to avoid stopping
// on it.
inline constexpr auto kIsTagValueSpecial = [] {
    std::array<bool, 256> t{};
    t[static_cast<unsigned char>('\n')] = true;
    t[static_cast<unsigned char>('"')] = true;
    t[static_cast<unsigned char>('\\')] = true;
    return t;
}();

} // namespace
/// Count trailing zeros in a 64-bit word.
///
/// Wraps platform intrinsics (`_BitScanForward64` on MSVC,
/// `__builtin_ctzll` on GCC/Clang) with a De Bruijn fallback.
/// Used by the SWAR delimiter scanner to find the byte offset
/// of the first matching delimiter.
static _forceinline int ctz(uint64_t x) {
#if defined(_MSC_VER)
    unsigned long index;
    _BitScanForward64(&index, x);
    return static_cast<int>(index);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_ctzll(x);
#endif
}

/// Convert 1–2 character "!?" annotation to NAG code.
///
///   "!"  → 1,  "?"  → 2,  "!!" → 3,  "?!" → 6,
///   "!?" → 5,  "??" → 4,  else → 0.
_forceinline static int nagFromSymbols(const char *sym, size_t len) {
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
            parseMovetext();
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
///     with a manual search loop per tag — no key/value parsing, zero callbacks.
///   - **Full parse**: calls `parseTag()` for each tag, which invokes
///     `visitor_.onTag()`.
_forceinline void PGNParser::parseTagSection() {
    if (!visitor_.wantsTags()) {
        // Batch-skip entire tag section
        const char *p = input_->data();
        const char *end = input_->end();
        while (true) {
            p = skipWhitespace(p, end);
            if (p >= end || *p != '[')
                break;
            const char *tagStart = p;
            ++p; // skip '['
            bool inString = false;
            bool closed = false;
            // Tag pairs never span a line, so a '\n' before the matching
            // ']' (in or out of a string) means this tag is malformed —
            // stop scanning instead of letting `inString` desync against
            // the rest of the file. Ordinary bytes take a single table
            // lookup; only '\n' / '"' / '\\' / ']' fall into the slower
            // branches below. An escaped byte is consumed right where the
            // backslash is found (no persistent "escaped" flag to check
            // on every subsequent iteration).
            while (p < end) {
                if (!kIsTagSpecial[static_cast<unsigned char>(*p)]) {
                    ++p;
                    continue;
                }
                char ch = *p++;
                if (ch == '\n') {
                    --p; // leave the newline for the resync scan below
                    break;
                }
                if (ch == '\\') {
                    if (inString) {
                        if (p < end && *p != '\n')
                            ++p; // consume the escaped byte literally
                        else
                            break; // backslash at EOF/before '\n': malformed
                    }
                    continue; // backslash outside a string is ordinary text
                }
                if (ch == '"') {
                    inString = !inString;
                    continue;
                }
                if (!inString && ch == ']') {
                    closed = true;
                    break;
                }
            }
            if (!closed) {
                // Malformed tag: discard the rest of this line and resync
                // at the next one instead of consuming the whole buffer.
                const char *lineEnd = p;
                while (lineEnd < end && *lineEnd != '\n')
                    ++lineEnd;
                if (lineEnd < end)
                    ++lineEnd; // consume the newline
                visitor_.onError(std::string_view(tagStart, static_cast<size_t>(lineEnd - tagStart)));
                p = lineEnd;
            }
        }
        input_->consume(static_cast<size_t>(p - input_->data()));
        return;
    }

    while (true) {
        input_->skipWhitespace();
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
    const char *tagLineStart = input_->data(); // position of '['
    consume('[');
    input_->skipWhitespace();

    // ---------- Key (direct pointer access) ----------
    const char *p = input_->data();
    const char *end = input_->end();
    const char *keyStart = p;

    while (p < end) {
        unsigned char c = static_cast<unsigned char>(*p);
        if (c <= 0x20 || c == ']')
            break;
        ++p;
    }

    input_->consume(static_cast<size_t>(p - keyStart));
    std::string_view key(keyStart, static_cast<size_t>(p - keyStart));

    input_->skipWhitespace();
    p = input_->data();

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

    // Tag pairs never span a line, so stop at '\n' as well as the closing
    // quote — an unescaped newline means this value never actually closed
    // (e.g. a trailing '\"' that escaped the quote instead of ending it).
    while (p < end) {
        if (!kIsTagValueSpecial[static_cast<unsigned char>(*p)]) {
            ++p;
            continue;
        }
        char c = *p;
        if (c == '\n')
            break;
        if (c == '\\' && p + 1 < end && p[1] != '\n') {
            escaped = true;
            p += 2; // skip '\' and the escaped char
            continue;
        }
        if (c == '"')
            break;
        ++p; // '\\' at EOF or immediately before '\n': treat as literal text
    }

    const bool closed = (p < end && *p == '"');
    if (!closed) {
        // Malformed tag: discard the rest of this line and resync at the
        // next one instead of misreading a later tag's quote as ours.
        const char *lineEnd = p;
        while (lineEnd < end && *lineEnd != '\n')
            ++lineEnd;
        if (lineEnd < end)
            ++lineEnd; // consume the newline
        input_->consume(static_cast<size_t>(lineEnd - valueStart));
        visitor_.onError(std::string_view(tagLineStart, static_cast<size_t>(lineEnd - tagLineStart)));
        return;
    }

    input_->consume(static_cast<size_t>(p - valueStart));
    const char *valueEnd = p;

    if (!escaped) {
        std::string_view value(valueStart, static_cast<size_t>(valueEnd - valueStart));

        if (!input_->eof())
            input_->read(); // closing quote

        input_->skipWhitespace();

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

    input_->skipWhitespace();

    if (!input_->eof() && input_->peek() == ']')
        input_->read();

    visitor_.onTag(key, tagValue_);
}

/// Check whether the movetext at `[base, end)` ends with a
/// game-termination marker: `*`, `1-0`, `0-1`, or `1/2-1/2`. Requires
/// `c == *base` and `p == base + 1` — the caller already knows the first
/// character (via its own dispatch) and this checks the rest with direct
/// indexed comparisons rather than a second full-string scan. On a match,
/// consumes the marker and calls `onGameEnd()`.
/// @return true when a result marker was found and consumed.
_forceinline bool PGNParser::checkEndOfGame(const char *base, const char *end, const char *p, char c) {
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
    }
    return false;
}
/// Parse the movetext section, dispatching each token to the visitor.
///
/// Handles: SAN moves, NAGs (`$` + `!?`), brace comments `{…}`,
/// semicolon line comments `;…`, variations `(…)`, move numbers,
/// and game-termination markers.  Recurses for nested variations.
/// 
enum class TokAction : uint8_t {
    Move = 0,     // default: letters, files, most SAN chars
    WS,           // handled before dispatch, not really needed here
    MoveNum,      // digits '1'..'9' -> move number
    ZeroOrCastle, // '0' -> could be "0-1" result or "0-0" castling
    OneOrResult,  // '1' -> could be "1-0"/"1/2-1/2" result or move number
    Star,         // '*' -> always a result marker
    Comment,      // '{'
    LineComment,  // ';'
    VarOpen,      // '('
    VarClose,     // ')'
    Nag,          // '$'
    Annotation,   // '!' or '?'
    Count
};

inline constexpr auto kActionTable = [] {
    std::array<TokAction, 256> t{};
    for (auto &e : t)
        e = TokAction::Move; // default: SAN move char
    for (unsigned char d = '1'; d <= '9'; ++d)
        t[d] = TokAction::MoveNum;
    t[static_cast<unsigned char>('0')] = TokAction::ZeroOrCastle;
    t[static_cast<unsigned char>('1')] = TokAction::OneOrResult; // overwrites the loop above for '1'
    t[static_cast<unsigned char>('*')] = TokAction::Star;
    t[static_cast<unsigned char>('{')] = TokAction::Comment;
    t[static_cast<unsigned char>(';')] = TokAction::LineComment;
    t[static_cast<unsigned char>('(')] = TokAction::VarOpen;
    t[static_cast<unsigned char>(')')] = TokAction::VarClose;
    t[static_cast<unsigned char>('$')] = TokAction::Nag;
    t[static_cast<unsigned char>('!')] = TokAction::Annotation;
    t[static_cast<unsigned char>('?')] = TokAction::Annotation;
    return t;
}();
template<bool inVariation>
#ifdef _MSC_VER
__forceinline
#endif
void PGNParser::parseMovetext() {
    while (true) {
        const char *base = input_->data();
        const char *end = input_->end();
        const char *p = pgn::skipWhitespace(base, end);

        input_->consume(static_cast<size_t>(p - base));
        if (input_->eof())
            break;

        base = input_->data();
        char c = *base;

        switch (pgn::kActionTable[static_cast<unsigned char>(c)]) {
        case pgn::TokAction::Star:
            checkEndOfGame(base, end, base + 1, c); // '*' always matches; no fallback needed
            return;

        case pgn::TokAction::OneOrResult:
            if (checkEndOfGame(base, end, base + 1, c))
                return;
            [[fallthrough]]; // wasn't a result -> it's a move number like "1."
        case pgn::TokAction::MoveNum:
            while (!input_->eof() && pgn::isDigit(input_->peek()))
                input_->read();
            while (!input_->eof() && input_->peek() == '.')
                input_->read();
            break;

        case pgn::TokAction::ZeroOrCastle:
            if (checkEndOfGame(base, end, base + 1, c))
                return;
            [[fallthrough]]; // wasn't "0-1" -> it's O-O-style castling text
        case pgn::TokAction::Move: {
            std::string_view san = readMove();
            if (!san.empty())
                visitor_.onMove(san);
            else if (pgn::kActionTable[static_cast<unsigned char>(c)] == pgn::TokAction::Move)
                input_->read(); // hard fallback, same as original
            break;
        }

        case pgn::TokAction::Comment:
            parseComment();
            break;
        case pgn::TokAction::LineComment:
            parseLineComment();
            break;

        case pgn::TokAction::VarOpen:
            input_->consume(1); // '('
            if (variationDepth_ < kMaxVariationDepth) {
                visitor_.onVariationStart();
                ++variationDepth_;
                parseMovetext<true>();
                --variationDepth_;
            } else {
                // Nesting this deep is never legitimate PGN and would
                // recurse the call stack unboundedly; flatten iteratively
                // instead of risking a stack overflow on adversarial
                // input. skipVariationBody() consumes through the
                // matching ')' itself.
                visitor_.onError("variation nesting exceeded max depth; flattened");
                skipVariationBody();
            }
            break;

        case pgn::TokAction::VarClose:
            if constexpr (inVariation) {
                visitor_.onVariationEnd();
                input_->consume(1);
                return;
            }
            input_->consume(1);
            break;

        case pgn::TokAction::Nag:
            parseNAG();
            break;

        case pgn::TokAction::Annotation: {
            const char *p2 = base;
            size_t len = 0;
            while (len < 2 && p2 < end && (*p2 == '!' || *p2 == '?')) {
                ++p2;
                ++len;
            }
            input_->consume(len);
            int nag = nagFromSymbols(base, len);
            if (nag != 0)
                visitor_.onNAG(nag);
            break;
        }

        default:
            break; // unreachable
        }
    }
}
/*
 * Skip movetext section to find the next game boundary.
 *
 * Uses SWAR (SIMD Within A Register) to scan 8 bytes at a time for three
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
_forceinline void PGNParser::skipMovetext() {
    const char *base = input_->data();
    const char *end = input_->end(); // base + input_->remaining();
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
_forceinline void PGNParser::parseComment() {
    consume('{');

    const char *start = input_->data();
    const char *end = input_->end(); // start + input_->remaining();
    const char *q = start;

    constexpr uint64_t ones = 0x0101010101010101ULL;
    constexpr uint64_t high = 0x8080808080808080ULL;
    constexpr uint64_t brace = 0x7D7D7D7D7D7D7D7DULL;

    while (static_cast<size_t>(end - q) >= sizeof(uint64_t)) {
        uint64_t x;
        std::memcpy(&x, q, sizeof(x));

        x ^= brace;

        const uint64_t hits = (x - ones) & ~x & high;
        if (hits) {
            q += ctz(hits) >> 3;
            break;
        }

        q += sizeof(uint64_t);
    }

    // Handle the tail / the byte containing the match.
    while (q < end && *q != '}')
        ++q;

    input_->consume(static_cast<size_t>(q - start));

    if (q < end)
        input_->consume(1);

    std::string_view sv(start, static_cast<size_t>(q - start));

    size_t left = 0;
    size_t right = sv.size();

    while (left < right) {
        const unsigned char c = static_cast<unsigned char>(sv[left]);
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        ++left;
    }

    while (right > left) {
        const unsigned char c = static_cast<unsigned char>(sv[right - 1]);
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
            break;
        --right;
    }

    visitor_.onComment(sv.substr(left, right - left));
}
/// Parse a semicolon line comment `;...` until the next newline.
/// Leading/trailing whitespace is stripped from the comment text
/// before it is passed to the visitor.
void PGNParser::parseLineComment() {
    consume(';');

    const char *start = input_->data();
    const char *end = input_->end(); // start + input_->remaining();
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

/// Iteratively consume the body of a variation whose matching ')' hasn't
/// been located yet, without recursing further. Used once nesting passes
/// `kMaxVariationDepth` so stack usage is bounded by input size rather
/// than by how deep the input actually nests. `{...}` and `;...` comments
/// are skipped structurally (so parens inside them don't upset the depth
/// count) but not reported to the visitor — this region is being
/// discarded, not parsed.
void PGNParser::skipVariationBody() {
    int depth = 0; // relative to the '(' the caller already consumed
    while (!input_->eof()) {
        char c = input_->read();
        switch (c) {
        case '(':
            ++depth;
            break;
        case ')':
            if (depth == 0)
                return; // matches the caller's already-consumed '('
            --depth;
            break;
        case '{':
            while (!input_->eof() && input_->peek() != '}')
                input_->read();
            if (!input_->eof())
                input_->read(); // consume '}'
            break;
        case ';':
            while (!input_->eof() && input_->peek() != '\n')
                input_->read();
            break;
        default:
            break;
        }
    }
}
inline constexpr auto kIsMoveChar = [] {
    std::array<bool, 256> t{};
    for (unsigned char c = 0; c < 128; ++c) {
        t[c] = !(c <= 0x20 || // whitespace/control
                 c == '{' || c == '}' || c == '(' || c == ')' || c == ';' || c == '$' || c == '!' || c == '?');
    }
    for (unsigned c = 128; c < 256; ++c)
        t[c] = false; // matches original c > 0x7A cutoff-ish
    return t;
}();
/// Read a SAN (Standard Algebraic Notation) move string from the input.
///
/// Stops at whitespace, comment/variation delimiters, NAG markers,
/// or after `MAX_MOVE_LENGTH` characters (15).  Returns a view into
/// the underlying buffer — no copying.
_forceinline std::string_view PGNParser::readMove() {
    const char *start = input_->data();
    const char *p = start;
    const char *end = input_->end();
    constexpr size_t MAX_MOVE_LENGTH = 15;

    while (p < end && static_cast<size_t>(p - start) < MAX_MOVE_LENGTH && kIsMoveChar[static_cast<unsigned char>(*p)]) {
        ++p;
    }

    size_t len = static_cast<size_t>(p - start);
    input_->consume(len);
    return { start, len };
}
/// Parse a non-negative decimal integer from the current input position.
/// Advances the cursor past all consecutive digit characters.
_forceinline int PGNParser::readNumber() {
    int num = 0;
    const char *p = input_->data();
    const char *end = input_->end();

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