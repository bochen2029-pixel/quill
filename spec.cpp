// quill · spec.cpp — the specification language: reading it, expanding the sugar, assembling
// the weights, rotating the basis, and building the model that the verbs work on.
#include "spec.h"

#include <float.h>

#include <cctype>
#include <charconv>
#include <cmath>
#include <unordered_map>

namespace quill {

int Spec::field(std::string_view want) const {
    for (size_t i = 0; i < fields.size(); ++i)
        if (fields[i].name == want) return (int)i;
    return -1;
}
int Spec::token(std::string_view want) const {
    for (size_t i = 0; i < tokens.size(); ++i)
        if (tokens[i] == want) return (int)i;
    return -1;
}

namespace {

constexpr int kMaxFields = 4096, kMaxTokens = 4096, kMaxStates = 4096, kMaxWidth = 4096;
constexpr unsigned long long kMaxSeed = 9007199254740991ull;      // 2^53 - 1: exact in every JSON reader
const char* const kStatements = "net tokens fields embed init tau neuron clear readout emit latch machine on rotate width";

// ---- exact decimal values, for W_ROUNDED_CONSTANT ----------------------------------------------------
// A number >= 0 as 0.DIGITS x 10^exp, DIGITS with no leading or trailing zero ("" for zero). Integers only.
struct Decimal {
    std::string digits;
    int exp = 0;
    bool operator==(const Decimal& o) const { return digits == o.digits && (digits.empty() || exp == o.exp); }
};
Decimal trimmed(std::string digits, long long exp) {            // digits read as 0.DIGITS x 10^exp
    const size_t lead = digits.find_first_not_of('0');
    if (lead == std::string::npos) return {};
    digits.erase(0, lead);
    digits.erase(digits.find_last_not_of('0') + 1);
    return { digits, (int)std::clamp(exp - (long long)lead, -100000LL, 100000LL) };
}
void big_mul(std::vector<uint32_t>& a, uint32_t m) {           // base 10^9, least significant limb first
    uint64_t carry = 0;
    for (auto& x : a) {
        const uint64_t v = (uint64_t)x * m + carry;
        x = (uint32_t)(v % 1000000000u);
        carry = v / 1000000000u;
    }
    while (carry) {
        a.push_back((uint32_t)(carry % 1000000000u));
        carry /= 1000000000u;
    }
}
// The exact decimal value of a finite double x >= 0: x = mant x 2^e, so x = mant x 5^-e / 10^-e when e < 0.
Decimal exact_decimal(double x) {
    if (!(x > 0) || !std::isfinite(x)) return {};
    int e2 = 0;
    const double f = std::frexp(x, &e2);                       // x = f x 2^e2, 0.5 <= f < 1: exact
    uint64_t mant = (uint64_t)std::ldexp(f, 53);               // f has at most 53 significant bits
    int e = e2 - 53;
    while (mant % 2 == 0) {
        mant /= 2;
        ++e;
    }
    std::vector<uint32_t> big{ (uint32_t)(mant % 1000000000u) };
    for (uint64_t r = mant / 1000000000u; r; r /= 1000000000u) big.push_back((uint32_t)(r % 1000000000u));
    long long point = 0;
    if (e >= 0)
        for (int k = e; k > 0; k -= 31) big_mul(big, k >= 31 ? 2147483648u : (1u << k));
    else {
        for (int k = -e; k > 0; k -= 13) {
            uint32_t m = 1;
            for (int j = 0; j < std::min(k, 13); ++j) m *= 5;  // 5^13 = 1220703125 < 2^31
            big_mul(big, m);
        }
        point = -e;
    }
    std::string s = std::to_string(big.back());
    for (size_t i = big.size() - 1; i-- > 0;) s += ssprintf("%09u", big[i]);
    return trimmed(s, (long long)s.size() - point);
}
// The exact value of a decimal literal as written: "2.5e-1", ".5", "4503599627370496".
Decimal literal_decimal(std::string_view w) {
    std::string digits;
    long long point = -1;
    size_t i = 0;
    for (; i < w.size() && (isdigit((unsigned char)w[i]) || w[i] == '.'); ++i)
        if (w[i] == '.') point = (long long)digits.size();
        else digits += w[i];
    if (point < 0) point = (long long)digits.size();
    long long e10 = 0;
    if (i < w.size() && (w[i] == 'e' || w[i] == 'E')) {
        const std::string tail(w.substr(i + 1));
        e10 = std::clamp(std::strtoll(tail.c_str(), nullptr, 10), -100000LL, 100000LL);
    }
    return trimmed(digits, point + e10);
}
// a - b for two decimals, rounded to a double (the difference of a literal and the double that holds it).
double decimal_difference(const Decimal& a, const Decimal& b) {
    const long long sa = a.exp - (long long)a.digits.size(), sb = b.exp - (long long)b.digits.size();
    const long long s = std::min(a.digits.empty() ? sb : sa, b.digits.empty() ? sa : sb);
    std::string x = a.digits.empty() ? "0" : a.digits + std::string((size_t)(sa - s), '0');
    std::string y = b.digits.empty() ? "0" : b.digits + std::string((size_t)(sb - s), '0');
    bool neg = false;
    if (x.size() < y.size() || (x.size() == y.size() && x < y)) {
        std::swap(x, y);
        neg = true;
    }
    std::string r(x.size(), '0');
    int borrow = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        int dx = x[x.size() - 1 - i] - '0' - borrow;
        const int dy = i < y.size() ? y[y.size() - 1 - i] - '0' : 0;
        borrow = dx < dy;
        if (borrow) dx += 10;
        r[r.size() - 1 - i] = (char)('0' + dx - dy);
    }
    const size_t lead = r.find_first_not_of('0');
    if (lead == std::string::npos) return 0.0;
    r.erase(0, lead);
    const std::string head = r.substr(0, 17);
    const std::string text = head + "e" + std::to_string(s + (long long)(r.size() - head.size()));
    double v = 0;
    std::from_chars(text.data(), text.data() + text.size(), v);
    return neg ? -v : v;
}
// Whether one operation on numbers rounds: the processor's inexact flag around it (as prove reads it).
template <class Op>
double flagged(bool& inexact, Op op) {
    _clearfp();
    const double v = op();
    if (_statusfp() & _SW_INEXACT) inexact = true;
    return v;
}

bool field_name_ok(std::string_view s) {
    if (s.empty() || !(isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (isalnum((unsigned char)c) || c == '_') continue;
        if (c == '.' && i + 1 < s.size() && s[i + 1] != '.') continue;
        return false;
    }
    return true;
}
bool token_name_ok(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!(isalnum((unsigned char)c) || c == '_')) return false;
    return true;
}
bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}
bool is_spare(std::string_view s) { return s.size() > 5 && s.substr(0, 5) == "spare" && all_digits(s.substr(5)); }

struct Tok {
    enum Kind { Word, Number, Punct };
    Kind kind = Word;
    std::string text;
    double value = 0;
    int col = 0;
    bool is(const char* p) const { return kind == Punct && text == p; }
};

struct Stop {};      // thrown to abandon one statement once its problem is recorded

struct Parser {
    Spec s;
    std::vector<Issue> errors;
    std::unordered_map<std::string, int> field_at;
    int line = 0;
    std::string src, note;
    std::vector<Tok> t;
    size_t p = 0, end = 0;                       // the expression cursor
    int net_line = 0, tokens_line = 0, readout_line = 0, rotate_line = 0, width_line = 0;
    int cur_sugar = -1;
    std::vector<int> autos;                      // the `in.<token>` fields quill declared
    std::vector<std::pair<int, int>> embeds;     // (token, field) pairs seen
    int open = -1;                               // the machine whose `on` lines are being read
    std::vector<int> ex, ex_line, wild, wild_line;

    // ---- problems ----
    void add(int col, const std::string& msg, const std::string& hint = "") {
        if (errors.size() >= 50) return;
        Issue i;
        i.code = "E_SPEC";
        i.message = msg;
        i.line = line;
        i.column = col;
        i.source = src;
        i.hint = hint;
        errors.push_back(std::move(i));
    }
    [[noreturn]] void fail(int col, const std::string& msg, const std::string& hint = "") {
        add(col, msg, hint);
        throw Stop{};
    }
    int end_col() const { return t.empty() ? 1 : t.back().col + (int)t.back().text.size(); }
    std::string names(const std::vector<std::string>& v, size_t most = 12) const {
        std::string out;
        for (size_t i = 0; i < v.size() && i < most; ++i) out += (i ? " " : "") + v[i];
        if (v.size() > most) out += ssprintf(" ... (%zu in all)", v.size());
        return out;
    }
    std::vector<std::string> field_names() const {
        std::vector<std::string> v;
        for (const auto& f : s.fields) v.push_back(f.name);
        return v;
    }

    // ---- lexing one line (its comment already removed) ----
    void lex(const std::string& text) {
        t.clear();
        const auto wordch = [](char c) { return isalnum((unsigned char)c) || c == '_' || c == '.'; };
        const auto mantissa_e = [](std::string_view w) {      // "1e", "2.5E": a number waiting for its exponent
            if (w.size() < 2 || (w.back() != 'e' && w.back() != 'E')) return false;
            int digits = 0, dots = 0;
            for (char c : w.substr(0, w.size() - 1)) {
                if (c >= '0' && c <= '9') ++digits;
                else if (c == '.') ++dots;
                else return false;
            }
            return digits > 0 && dots <= 1;
        };
        size_t i = 0;
        while (i < text.size()) {
            const char c = text[i];
            if (c == ' ' || c == '\t') { ++i; continue; }
            Tok k;
            k.col = (int)i + 1;
            if (wordch(c)) {
                size_t j = i;
                while (j < text.size() && wordch(text[j])) ++j;
                if (j + 1 < text.size() && (text[j] == '+' || text[j] == '-') && isdigit((unsigned char)text[j + 1]) &&
                    mantissa_e(std::string_view(text).substr(i, j - i))) {
                    ++j;
                    while (j < text.size() && isdigit((unsigned char)text[j])) ++j;
                }
                k.text = text.substr(i, j - i);
                k.kind = ((isdigit((unsigned char)c) || c == '.') && to_double(k.text, k.value)) ? Tok::Number : Tok::Word;
                i = j;
            } else if (c == '<' && i + 1 < text.size() && text[i + 1] == '-') {
                k.kind = Tok::Punct; k.text = "<-"; i += 2;
            } else if (c == '-' && i + 1 < text.size() && text[i + 1] == '>') {
                k.kind = Tok::Punct; k.text = "->"; i += 2;
            } else if (strchr("=,:*+-/()", c)) {
                k.kind = Tok::Punct; k.text = std::string(1, c); ++i;
            } else {
                size_t n = 1;                                   // show the whole character, not one byte of it
                while (i + n < text.size() && ((unsigned char)text[i + n] & 0xC0) == 0x80) ++n;
                const std::string ch = text.substr(i, n);
                std::string hint;
                if (ch == "\xE2\x88\x92" || ch == "\xE2\x80\x93" || ch == "\xE2\x80\x94") hint = "write the ASCII minus sign -";
                else if (ch == "\xE2\x86\x90") hint = "write <-";
                else if (ch == "\xE2\x86\x92") hint = "write ->";
                else if (ch == "\xC3\x97" || ch == "\xC2\xB7" || ch == "\xE2\x8B\x85") hint = "write * for multiplication";
                else if (c == '<') hint = "the arrow of a tau is <-";
                else if (c == '>') hint = "the arrow of a neuron is ->";
                else if (c == ';') hint = "one statement per line; no semicolons";
                fail(k.col, "unexpected character '" + ch + "'", hint);
            }
            t.push_back(std::move(k));
        }
    }

    // ---- names ----
    void need_tokens(int col) {
        if (!tokens_line) fail(col, "the `tokens` statement must come before this one");
    }
    int add_field(const std::string& name, FieldDecl::Kind kind, int col) {
        if ((int)s.fields.size() >= kMaxFields) fail(col, ssprintf("more than %d fields", kMaxFields));
        FieldDecl f;
        f.name = name;
        f.kind = kind;
        f.line = line;
        s.fields.push_back(std::move(f));
        field_at[name] = (int)s.fields.size() - 1;
        return (int)s.fields.size() - 1;
    }
    Op& push(Op::Kind kind) {
        Op op;
        op.kind = kind;
        op.line = line;
        op.generated = cur_sugar >= 0;
        op.sugar = cur_sugar;
        s.ops.push_back(std::move(op));
        return s.ops.back();
    }
    int token_of(const Tok& k) {
        need_tokens(k.col);
        const int at = k.kind == Tok::Punct ? -1 : s.token(k.text);
        if (at < 0) {
            const std::string near = nearest(k.text, s.tokens);
            fail(k.col, "unknown token '" + k.text + "'. The tokens are: " + names(s.tokens),
                 near.empty() ? "" : "did you mean '" + near + "'?");
        }
        return at;
    }
    // The field that is 1 during a step exactly when this token arrives: declared, embedded and
    // cleared by quill the first time it is needed.
    int input_field(int tok) {
        const std::string name = "in." + s.tokens[(size_t)tok];
        const auto it = field_at.find(name);
        if (it != field_at.end()) return it->second;
        const int f = add_field(name, FieldDecl::Input, 1);
        s.fields[(size_t)f].token = tok;
        s.fields[(size_t)f].auto_clear = true;
        s.fields[(size_t)f].clear_line = line;
        {
            Op& a = push(Op::Fields);
            a.generated = true;
            a.fields = { f };
        }
        {
            Op& b = push(Op::Embed);
            b.generated = true;
            b.token = tok;
            b.fields = { f };
        }
        embeds.emplace_back(tok, f);
        autos.push_back(f);
        return f;
    }
    int field_of(const Tok& k) {
        if (k.kind == Tok::Punct) fail(k.col, "a field name is expected here, not '" + k.text + "'");
        const auto it = field_at.find(k.text);
        if (it != field_at.end()) return it->second;
        if (k.text.size() > 3 && k.text.compare(0, 3, "in.") == 0) {
            need_tokens(k.col);
            const int tok = s.token(std::string_view(k.text).substr(3));
            if (tok >= 0) return input_field(tok);
            fail(k.col, "unknown field '" + k.text + "': '" + k.text.substr(3) + "' is not a token. The tokens are: " + names(s.tokens),
                 "in.<token> is the field that is 1 while that token is being read");
        }
        if (k.kind == Tok::Number) fail(k.col, "a field name is expected here, not the number " + k.text);
        const std::string near = nearest(k.text, field_names());
        fail(k.col, "unknown field '" + k.text + "'. Declared so far: " + names(field_names()),
             !near.empty() ? "did you mean '" + near + "'?" : std::string("declare it first with: fields ") + k.text);
    }
    int written_field(const Tok& k, const char* verb) {      // a field something writes: never `one`
        const int f = field_of(k);
        if (f == 0) fail(k.col, std::string("`one` is the constant 1; nothing may ") + verb + " it");
        return f;
    }

    // ---- expressions: sums of weighted fields ----
    // Each coefficient also carries what is written minus what float64 holds (to first order) and whether any
    // rounding entered it, so that a constant float64 cannot hold is reported (W_ROUNDED_CONSTANT). The values
    // themselves are computed exactly as before.
    std::vector<IssueValue> rounded_now;                      // this statement's numbers that float64 cannot hold
    static void add_to(double& c, double& err, bool& rounded, bool& op_rounded, double b, double berr, bool brounded) {
        bool inexact = false;
        const double a = c, s = flagged(inexact, [&] { return a + b; });
        const double bb = s - a, e = (a - (s - bb)) + (b - bb);  // the exact error of a + b (two-sum)
        c = s;
        err += berr + e;
        rounded = rounded || brounded || inexact;
        op_rounded = op_rounded || inexact;
    }
    static void accumulate(Lin& a, const Lin& b, double sign) {
        for (const auto& tb : b.terms) {
            bool found = false;
            for (auto& ta : a.terms)
                if (ta.field == tb.field) {
                    add_to(ta.coef, ta.err, ta.rounded, ta.op_rounded, sign * tb.coef, sign * tb.err, tb.rounded);
                    ta.op_rounded = ta.op_rounded || tb.op_rounded;
                    found = true;
                    break;
                }
            if (!found) a.terms.push_back({ tb.field, sign * tb.coef, sign * tb.err, tb.rounded, tb.op_rounded });
        }
        add_to(a.constant, a.cerr, a.crounded, a.cop_rounded, sign * b.constant, sign * b.cerr, b.crounded);
        a.cop_rounded = a.cop_rounded || b.cop_rounded;
    }
    // c x (b +- berr), or c / (b +- berr): the product or quotient, and the first-order error. A coefficient that is
    // exactly 0 stays exactly 0 whatever b is, so it takes on none of b's rounding.
    static void scale(double& c, double& err, bool& rounded, bool& op_rounded, double b, double berr, bool brounded,
                      bool bop_rounded, bool divide) {
        bool inexact = false;
        const double a = c;
        if (a == 0 && err == 0 && !rounded) {
            c = divide ? a / b : a * b;                            // 0 (or -0), as before
            return;
        }
        op_rounded = op_rounded || bop_rounded;
        if (divide) {
            const double q = flagged(inexact, [&] { return a / b; });
            c = q;
            err = (err - q * berr) / b + std::fma(-q, b, a) / b;   // the remainder a - q b is exact (fma)
        } else {
            const double p = flagged(inexact, [&] { return a * b; });
            c = p;
            err = err * b + a * berr + std::fma(a, b, -p);       // the exact error of a x b (fma)
        }
        rounded = rounded || brounded || inexact;
        op_rounded = op_rounded || inexact;
    }
    Lin primary() {
        if (p >= end) fail(end_col(), "the expression stops where a field or a number should be");
        const Tok k = t[p];
        Lin v;
        if (k.kind == Tok::Number) {
            ++p;
            v.constant = k.value;
            const Decimal written = literal_decimal(k.text), stored = exact_decimal(k.value);
            if (!(written == stored)) {                        // a literal float64 cannot hold
                v.cerr = decimal_difference(written, stored);
                v.crounded = true;
                bool seen = false;
                for (const auto& r : rounded_now) seen = seen || (r.what == "number" && r.written == k.text);
                if (!seen) rounded_now.push_back({ "number", k.text, k.value, v.cerr });
            }
            return v;
        }
        if (k.kind == Tok::Word) {
            ++p;
            if (!field_name_ok(k.text))
                fail(k.col, "'" + k.text + "' is neither a number nor a field name",
                     isdigit((unsigned char)k.text[0]) ? "write 2*A for two times A" : "");
            v.terms.push_back({ field_of(k), 1.0 });
            return v;
        }
        if (k.is("(")) {
            ++p;
            v = sum();
            if (p >= end || !t[p].is(")")) fail(p < end ? t[p].col : end_col(), "a ')' is missing");
            ++p;
            return v;
        }
        fail(k.col, "unexpected '" + k.text + "' in an expression");
    }
    Lin unary() {
        if (p < end && t[p].is("-")) {
            ++p;
            Lin v = unary();
            for (auto& x : v.terms) {
                x.coef = -x.coef;
                x.err = -x.err;
            }
            v.constant = -v.constant;
            v.cerr = -v.cerr;
            return v;
        }
        if (p < end && t[p].is("+")) { ++p; return unary(); }
        return primary();
    }
    Lin product() {
        Lin a = unary();
        for (;;) {
            if (p < end && (t[p].is("*") || t[p].is("/"))) {
                const bool divide = t[p].is("/");
                const int col = t[p].col;
                ++p;
                Lin b = unary();
                if (divide) {
                    if (!b.terms.empty()) fail(col, "an expression can be divided by a number only, not by a field");
                    if (b.constant == 0) fail(col, "division by zero");
                } else {
                    if (!a.terms.empty() && !b.terms.empty())
                        fail(col, "the product of two fields is not linear",
                             "a rule's argument is a weighted sum of fields; use a threshold on a sum to combine two fields");
                    if (a.terms.empty()) std::swap(a, b);           // a carries the fields, b is the number
                }
                for (auto& x : a.terms) scale(x.coef, x.err, x.rounded, x.op_rounded, b.constant, b.cerr, b.crounded, b.cop_rounded, divide);
                scale(a.constant, a.cerr, a.crounded, a.cop_rounded, b.constant, b.cerr, b.crounded, b.cop_rounded, divide);
            } else if (p < end && (t[p].kind != Tok::Punct || t[p].is("("))) {
                fail(t[p].col, "an operator is missing before '" + t[p].text + "'", "write 2*A for two times A");
            } else {
                return a;
            }
        }
    }
    Lin sum() {
        Lin a = product();
        while (p < end && (t[p].is("+") || t[p].is("-"))) {
            const double sign = t[p].is("-") ? -1.0 : 1.0;
            ++p;
            accumulate(a, product(), sign);
        }
        return a;
    }
    Lin expression(size_t from, size_t to, const char* what) {
        p = from;
        end = to;
        if (p >= end) fail(from < t.size() ? t[from].col : end_col(), std::string("the ") + what + " is missing");
        Lin v = sum();
        if (p < end) fail(t[p].col, "unexpected '" + t[p].text + "' after the " + what);
        // A coefficient that an operation on numbers rounded (8/3, -4.5 + 2^-52): the literals were reported
        // as they were read; this reports what their combination stored. A term that rounded to 0 is dropped
        // below like any zero term, and reported here.
        for (const auto& x : v.terms)
            if (x.op_rounded || (x.coef == 0 && x.rounded && x.err != 0))
                rounded_now.push_back({ "the weight of " + s.fields[(size_t)x.field].name, "", x.coef, x.err });
        if (v.cop_rounded) rounded_now.push_back({ "the constant", "", v.constant, v.cerr });
        v.terms.erase(std::remove_if(v.terms.begin(), v.terms.end(), [](const Term& x) { return x.coef == 0; }), v.terms.end());
        return v;
    }
    size_t find(const char* punct) const {
        for (size_t i = 1; i < t.size(); ++i)
            if (t[i].is(punct)) return i;
        return std::string::npos;
    }

    // ---- statements ----
    void st_net() {
        if (net_line) fail(t[0].col, ssprintf("the network is already named at line %d", net_line));
        if (t.size() != 2 || t[1].kind == Tok::Punct || t[1].text.size() > 64)       // the lexer's words are letters, digits, '_' and '.'
            fail(t.size() > 1 ? t[1].col : end_col(), "net takes one name of letters, digits, '_' and '.' (at most 64 characters)",
                 "for example: net R1_plain");
        s.name = t[1].text;
        net_line = line;
    }
    void st_tokens() {
        if (tokens_line) fail(t[0].col, ssprintf("the tokens are already listed at line %d", tokens_line));
        if (t.size() < 2) fail(end_col(), "tokens needs at least one token name", "for example: tokens zero one");
        std::vector<std::string> list;
        for (size_t i = 1; i < t.size(); ++i) {
            const Tok& k = t[i];
            if (k.is(",")) continue;
            if (k.kind == Tok::Punct || !token_name_ok(k.text))
                fail(k.col, "'" + k.text + "' is not a token name: use letters, digits and '_', separated by spaces");
            if (std::find(list.begin(), list.end(), k.text) != list.end()) fail(k.col, "the token '" + k.text + "' is listed twice");
            unsigned long long as_index = 0;
            if (all_digits(k.text) && (!to_u64(k.text, as_index) || as_index != list.size() || (k.text.size() > 1 && k.text[0] == '0')))
                fail(k.col, ssprintf("the token named '%s' would be token number %zu", k.text.c_str(), list.size()),
                     "a token whose name is a number must be named by its own position (tokens 0 1 2), because "
                     "`run --tokens` accepts names and positions alike");
            list.push_back(k.text);
        }
        if (list.empty()) fail(end_col(), "tokens needs at least one token name", "for example: tokens zero one");
        if ((int)list.size() > kMaxTokens) fail(t[0].col, ssprintf("more than %d tokens", kMaxTokens));
        s.tokens = std::move(list);
        tokens_line = line;
    }
    void check_new_field(const Tok& k) {
        if (k.kind == Tok::Punct || !field_name_ok(k.text))
            fail(k.col, "'" + k.text + "' is not a field name: start with a letter or '_', then letters, digits, '_' and '.'");
        if (k.text == "one") fail(k.col, "`one` is always there (it holds the constant 1); do not declare it");
        if (is_spare(k.text)) fail(k.col, "'" + k.text + "' is reserved: quill names the unused coordinates spare0, spare1, ...");
        if (k.text.compare(0, 3, "in.") == 0)
            fail(k.col, "'" + k.text + "' is reserved: a name that starts with 'in.' is the input field of a token (in.<token>)",
                 "choose another name");
        const auto it = field_at.find(k.text);
        if (it != field_at.end())
            fail(k.col, ssprintf("the field '%s' is already declared at line %d", k.text.c_str(), s.fields[(size_t)it->second].line));
    }
    void st_fields() {
        if (t.size() < 2) fail(end_col(), "fields needs at least one field name", "for example: fields A B");
        std::vector<int> made;
        const auto flush = [&] {
            if (!made.empty()) push(Op::Fields).fields = made;
            made.clear();
        };
        size_t named = 0;
        for (size_t i = 1; i < t.size(); ++i) {
            const Tok& k = t[i];
            if (k.is(",")) continue;
            ++named;
            if (k.kind != Tok::Punct && k.text.size() > 3 && k.text.compare(0, 3, "in.") == 0) {
                // A token's input field, declared here so that its coordinate comes here; quill still embeds and clears it.
                need_tokens(k.col);
                const int tok = s.token(std::string_view(k.text).substr(3));
                if (tok < 0)
                    fail(k.col, "'" + k.text + "' cannot be declared: '" + k.text.substr(3) + "' is not a token. The tokens are: " + names(s.tokens),
                         "in.<token> is the field that is 1 while that token is being read");
                if (const auto it = field_at.find(k.text); it != field_at.end())
                    fail(k.col, ssprintf("the field '%s' is already declared at line %d", k.text.c_str(), s.fields[(size_t)it->second].line));
                flush();
                input_field(tok);
                continue;
            }
            check_new_field(k);
            made.push_back(add_field(k.text, FieldDecl::Declared, k.col));
        }
        if (!named) fail(end_col(), "fields needs at least one field name", "for example: fields A B");
        flush();
    }
    void st_embed() {
        if (t.size() < 3) fail(end_col(), "embed takes a token and the field it adds 1 to", "for example: embed one in1");
        const int tok = token_of(t[1]);
        std::vector<int> into;
        for (size_t i = 2; i < t.size(); ++i) {
            if (t[i].is(",")) continue;
            const int f = written_field(t[i], "write");
            if (std::find(embeds.begin(), embeds.end(), std::make_pair(tok, f)) != embeds.end())
                fail(t[i].col, "the token '" + s.tokens[(size_t)tok] + "' already adds 1 to '" + s.fields[(size_t)f].name +
                                   "'" + (s.fields[(size_t)f].auto_clear ? " (quill embeds in.<token> fields itself)" : ""));
            embeds.emplace_back(tok, f);
            into.push_back(f);
        }
        if (into.empty()) fail(end_col(), "embed takes a token and the field it adds 1 to", "for example: embed one in1");
        Op& op = push(Op::Embed);
        op.token = tok;
        op.fields = into;
    }
    void set_init(int f, double v, int col) {
        FieldDecl& fd = s.fields[(size_t)f];
        if (fd.init_line) fail(col, ssprintf("the field '%s' already has a start value (line %d)", fd.name.c_str(), fd.init_line));
        fd.init = v;
        fd.init_line = line;
    }
    void st_init() {
        if (t.size() < 2) fail(end_col(), "init takes FIELD=VALUE pairs", "for example: init A=1");
        std::vector<int> fs;
        std::vector<double> vs;
        size_t i = 1;
        while (i < t.size()) {
            const Tok name = t[i];
            if (name.kind == Tok::Punct) fail(name.col, "init takes FIELD=VALUE pairs", "for example: init A=1");
            if (i + 1 >= t.size() || !t[i + 1].is("="))
                fail(i + 1 < t.size() ? t[i + 1].col : end_col(), "an '=' is expected after '" + name.text + "'", "for example: init A=1");
            size_t v = i + 2;
            double sign = 1;
            if (v < t.size() && (t[v].is("-") || t[v].is("+"))) { sign = t[v].is("-") ? -1 : 1; ++v; }
            if (v >= t.size() || t[v].kind != Tok::Number)
                fail(v < t.size() ? t[v].col : end_col(), "a number is expected after '" + name.text + "='");
            const int f = written_field(name, "set");
            {
                const Decimal written = literal_decimal(t[v].text), stored = exact_decimal(t[v].value);
                if (!(written == stored))
                    rounded_now.push_back({ "the start value of " + name.text, (sign < 0 ? "-" : "") + t[v].text, sign * t[v].value,
                                            sign * decimal_difference(written, stored) });
            }
            set_init(f, sign * t[v].value, name.col);
            fs.push_back(f);
            vs.push_back(sign * t[v].value);
            i = v + 1;
        }
        Op& op = push(Op::Init);
        op.fields = fs;
        op.values = vs;
    }
    std::vector<int> targets_of(size_t from, size_t to) {
        std::vector<int> out;
        for (size_t i = from; i < to; ++i) {
            if (t[i].is(",")) continue;
            const int f = written_field(t[i], "write");
            if (std::find(out.begin(), out.end(), f) != out.end())
                fail(t[i].col, "the field '" + s.fields[(size_t)f].name + "' is listed twice");
            out.push_back(f);
        }
        return out;
    }
    void st_tau() {
        const size_t arrow = find("<-");
        if (arrow == std::string::npos)
            fail(t[0].col, "a tau has the form: tau TARGET <- EXPRESSION", "for example: tau A <- 2*in1 - 2*A - 0.5");
        if (arrow == 1) fail(t[arrow].col, "the tau names no target field before '<-'");
        const std::vector<int> targets = targets_of(1, arrow);
        const Lin in = expression(arrow + 1, t.size(), "expression after '<-'");
        Op& op = push(Op::Tau);
        op.fields = targets;
        op.in = in;
        op.note = note;
    }
    void st_neuron() {
        const size_t arrow = find("->");
        if (arrow == std::string::npos)
            fail(t[0].col, "a neuron has the form: neuron EXPRESSION -> WEIGHTED FIELDS", "for example: neuron 2*in1 - 2*A - 0.5 -> A");
        const Lin in = expression(1, arrow, "expression before '->'");
        const Lin out = expression(arrow + 1, t.size(), "list of written fields after '->'");
        if (out.constant != 0) fail(t[arrow + 1].col, "a neuron writes fields only: the part after '->' cannot hold a constant");
        if (out.terms.empty()) fail(t[arrow + 1].col, "the neuron writes nothing");
        for (const auto& x : out.terms)
            if (x.field == 0) fail(t[arrow + 1].col, "`one` is the constant 1; nothing may write it");
        Op& op = push(Op::Neuron);
        op.in = in;
        op.out = out;
        op.note = note;
    }
    void mark_cleared(int f, int col) {
        FieldDecl& fd = s.fields[(size_t)f];
        if (fd.clear_line)
            fail(col, fd.auto_clear ? "'" + fd.name + "' is cleared by quill itself (every in.<token> field is)"
                                    : ssprintf("the field '%s' is already cleared at line %d", fd.name.c_str(), fd.clear_line),
                 "clearing a field twice would subtract it twice");
        fd.clear_line = line;
    }
    void st_clear() {
        if (t.size() < 2) fail(end_col(), "clear takes the fields to rewrite from zero", "for example: clear A in0 in1");
        std::vector<int> fs = targets_of(1, t.size());
        for (size_t i = 0; i < fs.size(); ++i) mark_cleared(fs[i], t[1].col);
        Op& op = push(Op::Clear);
        op.fields = fs;
        op.note = note;
    }
    void need_no_readout() {
        if (readout_line)
            fail(t[0].col, ssprintf("the readout is already defined at line %d (a specification has one readout or one emit)", readout_line));
    }
    void st_readout() {
        need_no_readout();
        const Lin in = expression(1, t.size(), "margin expression");
        Op& op = push(Op::Readout);
        op.in = in;
        op.note = note;
        readout_line = line;
    }
    int begin_sugar() {
        Sugar g;
        g.line = line;
        g.text = trim(src.substr(0, src.find('#')));
        s.sugar.push_back(std::move(g));
        return (int)s.sugar.size() - 1;
    }
    void st_emit() {
        need_no_readout();
        if (t.size() < 2) fail(end_col(), "emit takes the fields in which the decision is emit", "for example: emit S.1 S.2");
        std::vector<int> fs;
        for (size_t i = 1; i < t.size(); ++i) {
            if (t[i].is(",")) continue;
            const int f = field_of(t[i]);
            if (f == 0) fail(t[i].col, "`one` is the constant 1: emit on it would always emit");
            if (std::find(fs.begin(), fs.end(), f) != fs.end()) fail(t[i].col, "the field '" + s.fields[(size_t)f].name + "' is listed twice");
            fs.push_back(f);
        }
        cur_sugar = begin_sugar();
        Op& op = push(Op::Readout);
        for (int f : fs) op.in.terms.push_back({ f, 8.0 });
        op.in.constant = -4.0;
        op.note = "margin +4 when one of these fields is 1, -4 when none is";
        cur_sugar = -1;
        readout_line = line;
    }
    void st_rotate() {
        if (rotate_line) fail(t[0].col, ssprintf("rotate is already given at line %d", rotate_line));
        unsigned long long seed = 0;
        if (t.size() != 2 || !all_digits(t[1].text) || !to_u64(t[1].text, seed) || seed > kMaxSeed)
            fail(t.size() > 1 ? t[1].col : end_col(), "rotate takes one seed: a whole number from 0 to 9007199254740991", "for example: rotate 101");
        s.rotate = true;
        s.seed = seed;
        rotate_line = line;
    }
    void st_width() {
        if (width_line) fail(t[0].col, ssprintf("width is already given at line %d", width_line));
        unsigned long long w = 0;
        if (t.size() != 2 || !all_digits(t[1].text) || !to_u64(t[1].text, w) || w < 1 || w > (unsigned long long)kMaxWidth)
            fail(t.size() > 1 ? t[1].col : end_col(), ssprintf("width takes one whole number from 1 to %d", kMaxWidth), "for example: width 32");
        s.width = (int)w;
        s.width_explicit = true;
        width_line = line;
    }
    // KEY=a,b,c groups after position `from`. Returns (key token, value tokens).
    std::vector<std::pair<Tok, std::vector<Tok>>> key_values(size_t from) {
        std::vector<std::pair<Tok, std::vector<Tok>>> out;
        size_t i = from;
        while (i < t.size()) {
            const Tok key = t[i];
            if (key.kind != Tok::Word || i + 1 >= t.size() || !t[i + 1].is("="))
                fail(key.col, "expected KEY=VALUE here, found '" + key.text + "'");
            i += 2;
            std::vector<Tok> vals;
            for (;;) {
                if (i >= t.size() || t[i].kind == Tok::Punct) fail(i < t.size() ? t[i].col : end_col(), "a value is missing after '" + key.text + "='");
                vals.push_back(t[i++]);
                if (i < t.size() && t[i].is(",")) { ++i; continue; }
                break;
            }
            for (const auto& done : out)
                if (done.first.text == key.text) fail(key.col, "'" + key.text + "=' is given twice");
            out.emplace_back(key, std::move(vals));
        }
        return out;
    }
    std::string any_of(const std::vector<int>& toks) const {      // "a", "a or b", "a, b or c"
        std::string out;
        for (size_t i = 0; i < toks.size(); ++i)
            out += (i == 0 ? "" : i + 1 == toks.size() ? " or " : ", ") + s.tokens[(size_t)toks[i]];
        return out;
    }
    Lin lin(std::initializer_list<Term> terms, double constant) {
        Lin l;
        l.terms.assign(terms);
        l.constant = constant;
        return l;
    }
    void st_latch() {
        need_tokens(t[0].col);
        if (t.size() < 2 || t[1].kind == Tok::Punct)
            fail(end_col(), "latch takes a field name, then set=, reset= and toggle= token lists", "for example: latch A toggle=one reset=zero");
        const Tok name = t[1];
        const auto kv = key_values(2);
        std::vector<int> set, reset, toggle;
        int init = -1, init_col = 0;
        std::vector<int> role(s.tokens.size(), 0);
        for (const auto& g : kv) {
            const std::string& key = g.first.text;
            if (key == "init") {
                if (g.second.size() != 1 || (g.second[0].text != "0" && g.second[0].text != "1"))
                    fail(g.second[0].col, "a latch starts at init=0 or init=1");
                init = g.second[0].text == "1";
                init_col = g.first.col;
                continue;
            }
            std::vector<int>* into = key == "set" ? &set : key == "reset" ? &reset : key == "toggle" ? &toggle : nullptr;
            if (!into) fail(g.first.col, "latch does not know '" + key + "='; it takes set=, reset=, toggle= and init=");
            const int code = key == "set" ? 1 : key == "reset" ? 2 : 3;
            for (const auto& v : g.second) {
                const int tok = token_of(v);
                if (role[(size_t)tok])
                    fail(v.col, "the token '" + s.tokens[(size_t)tok] + "' has two roles in this latch",
                         "a token may set, reset or toggle a latch, not two of them");
                role[(size_t)tok] = code;
                into->push_back(tok);
            }
            std::sort(into->begin(), into->end());
        }
        if (set.empty() && reset.empty() && toggle.empty())
            fail(end_col(), "the latch names no token: give set=, reset= or toggle=", "for example: latch A toggle=one reset=zero");
        cur_sugar = begin_sugar();
        int f;
        const auto have = field_at.find(name.text);
        if (have != field_at.end()) {
            f = have->second;
            if (s.fields[(size_t)f].kind != FieldDecl::Declared) fail(name.col, "'" + name.text + "' cannot be a latch: it is " +
                                                                                    (f == 0 ? "the constant" : "an input or a machine state"));
        } else {
            check_new_field(name);
            f = add_field(name.text, FieldDecl::Declared, name.col);
            push(Op::Fields).fields = { f };
        }
        if (init >= 0) {
            set_init(f, init, init_col);
            Op& op = push(Op::Init);
            op.fields = { f };
            op.values = { (double)init };
        }
        const std::string who = "latch " + name.text + ": ";
        const auto listed = [&](const std::vector<int>& v) { return any_of(v); };
        if (!set.empty()) {
            Lin in;
            for (int tok : set) in.terms.push_back({ input_field(tok), 2.0 });
            in.constant = -0.5;
            Op& op = push(Op::Tau);
            op.fields = { f };
            op.in = in;
            op.note = who + "set by " + listed(set);
        }
        if (!toggle.empty()) {
            Lin in;
            for (int tok : toggle) in.terms.push_back({ input_field(tok), 2.0 });
            in.terms.push_back({ f, -2.0 });
            in.constant = -0.5;
            Op& op = push(Op::Tau);
            op.fields = { f };
            op.in = in;
            op.note = who + "toggled on by " + listed(toggle) + " while it is 0";
        }
        {
            Lin in;
            in.terms.push_back({ f, 2.0 });
            std::vector<int> events = set;
            events.insert(events.end(), reset.begin(), reset.end());
            events.insert(events.end(), toggle.begin(), toggle.end());
            for (int tok : events) in.terms.push_back({ input_field(tok), -2.0 });
            in.constant = -0.5;
            Op& op = push(Op::Tau);
            op.fields = { f };
            op.in = in;
            op.note = who + "stays 1 unless " + listed(events) + " arrives";
        }
        mark_cleared(f, name.col);
        Op& op = push(Op::Clear);
        op.fields = { f };
        op.note = who + "rewritten at every step";
        cur_sugar = -1;
    }
    void st_machine() {
        need_tokens(t[0].col);
        if (t.size() < 2 || t[1].kind == Tok::Punct)
            fail(end_col(), "machine takes a name and states=N", "for example: machine S states=3 init=0");
        const Tok name = t[1];
        if (!field_name_ok(name.text)) fail(name.col, "'" + name.text + "' is not a machine name: start with a letter or '_'");
        if (name.text == "in")
            fail(name.col, "'in' cannot name a machine: its state fields in.0, in.1, ... would take names reserved for the in.<token> fields",
                 "choose another name, for example: machine S states=3");
        for (const auto& m : s.machines)
            if (m.name == name.text) fail(name.col, ssprintf("the machine '%s' is already declared at line %d", m.name.c_str(), m.line));
        unsigned long long states = 0, init = 0;
        bool have_states = false;
        for (const auto& g : key_values(2)) {
            const std::string& key = g.first.text;
            if (key != "states" && key != "init") fail(g.first.col, "machine does not know '" + key + "='; it takes states= and init=");
            unsigned long long v = 0;
            if (g.second.size() != 1 || !all_digits(g.second[0].text) || !to_u64(g.second[0].text, v))
                fail(g.second[0].col, "'" + key + "=' takes one whole number");
            if (key == "states") { states = v; have_states = true; }
            else init = v;
        }
        if (!have_states || states < 1 || states > (unsigned long long)kMaxStates)
            fail(end_col(), ssprintf("machine needs states=N with N from 1 to %d", kMaxStates), "for example: machine S states=3 init=0");
        if (init >= states) fail(end_col(), ssprintf("init=%llu is not a state: the states are 0 to %llu", init, states - 1));
        for (unsigned long long k = 0; k < states; ++k) {
            Tok fk = name;
            fk.text = name.text + "." + std::to_string(k);
            check_new_field(fk);
        }
        cur_sugar = begin_sugar();
        Machine m;
        m.name = name.text;
        m.line = line;
        m.states = (int)states;
        m.init = (int)init;
        m.first_field = (int)s.fields.size();
        std::vector<int> made;
        for (int k = 0; k < m.states; ++k) {
            const int f = add_field(m.name + "." + std::to_string(k), FieldDecl::State, name.col);
            s.fields[(size_t)f].machine = (int)s.machines.size();
            made.push_back(f);
        }
        push(Op::Fields).fields = made;
        set_init(m.first_field + m.init, 1.0, name.col);
        {
            Op& op = push(Op::Init);
            op.fields = { m.first_field + m.init };
            op.values = { 1.0 };
        }
        s.machines.push_back(std::move(m));
        open = (int)s.machines.size() - 1;
        const size_t cells = (size_t)states * s.tokens.size();
        ex.assign(cells, -1);
        ex_line.assign(cells, 0);
        wild.assign(s.tokens.size(), -1);
        wild_line.assign(s.tokens.size(), 0);
        cur_sugar = -1;
    }
    void st_on() {
        if (open < 0) fail(t[0].col, "`on` belongs to a machine: it must follow a `machine` statement or another `on` line");
        const Machine& m = s.machines[(size_t)open];
        const size_t colon = find(":");
        if (colon == std::string::npos || colon < 2)
            fail(end_col(), "an on line has the form: on TOKEN: STATE->STATE ...", "for example: on one: 0->1 1->2 2->0");
        std::vector<int> toks;
        for (size_t i = 1; i < colon; ++i) {
            if (t[i].is(",")) continue;
            const int tok = token_of(t[i]);
            if (std::find(toks.begin(), toks.end(), tok) != toks.end()) fail(t[i].col, "the token '" + t[i].text + "' is listed twice");
            toks.push_back(tok);
        }
        const auto state = [&](size_t i) -> int {
            unsigned long long v = 0;
            if (i >= t.size() || !all_digits(t[i].text) || !to_u64(t[i].text, v) || v >= (unsigned long long)m.states)
                fail(i < t.size() ? t[i].col : end_col(),
                     ssprintf("a state of machine %s is expected here: a number from 0 to %d", m.name.c_str(), m.states - 1));
            return (int)v;
        };
        size_t i = colon + 1;
        if (i >= t.size()) fail(end_col(), "the on line lists no transition", "for example: on one: 0->1 1->2 2->0");
        const size_t T = s.tokens.size();
        while (i < t.size()) {
            if (t[i].is(",")) { ++i; continue; }
            const int col = t[i].col;
            const bool any = t[i].is("*");
            const int from = any ? -1 : state(i);
            if (i + 1 >= t.size() || !t[i + 1].is("->")) fail(i + 1 < t.size() ? t[i + 1].col : end_col(), "a transition is written STATE->STATE");
            const int to = state(i + 2);
            for (int tok : toks) {
                if (any) {
                    if (wild[(size_t)tok] >= 0 && wild[(size_t)tok] != to)
                        fail(col, ssprintf("on %s, '*' already goes to state %d (line %d)", s.tokens[(size_t)tok].c_str(), wild[(size_t)tok],
                                           wild_line[(size_t)tok]));
                    wild[(size_t)tok] = to;
                    wild_line[(size_t)tok] = line;
                } else {
                    const size_t cell = (size_t)from * T + (size_t)tok;
                    if (ex[cell] >= 0 && ex[cell] != to)
                        fail(col, ssprintf("on %s, state %d already goes to state %d (line %d)", s.tokens[(size_t)tok].c_str(), from, ex[cell],
                                           ex_line[cell]));
                    ex[cell] = to;
                    ex_line[cell] = line;
                }
            }
            i += 3;
        }
        s.sugar[(size_t)sugar_of_machine(open)].text += "\n  " + trim(src.substr(0, src.find('#')));
    }
    int sugar_of_machine(int mi) const {
        for (size_t g = 0; g < s.sugar.size(); ++g)
            if (s.sugar[g].line == s.machines[(size_t)mi].line) return (int)g;
        return 0;
    }
    // The end of a machine block: write its rules. One threshold per listed move, one per state that
    // can hold, one for a token that sends every state to the same place; then every state is cleared.
    void close_machine() {
        const int mi = open;
        open = -1;
        Machine& m = s.machines[(size_t)mi];
        const int keep_line = line;
        const size_t T = s.tokens.size();
        const int S = m.states;
        std::vector<char> pure(T, 0);
        m.delta.assign((size_t)S * T, 0);
        for (size_t e = 0; e < T; ++e) {
            bool any_explicit = false;
            for (int st = 0; st < S; ++st)
                if (ex[(size_t)st * T + e] >= 0) any_explicit = true;
            pure[e] = wild[e] >= 0 && !any_explicit;
            for (int st = 0; st < S; ++st) {
                const int x = ex[(size_t)st * T + e];
                m.delta[(size_t)st * T + e] = x >= 0 ? x : wild[e] >= 0 ? wild[e] : st;
            }
        }
        cur_sugar = sugar_of_machine(mi);
        const std::string who = "machine " + m.name + ": ";
        const auto sf = [&](int st) { return m.first_field + st; };
        for (size_t e = 0; e < T; ++e) {
            if (pure[e]) {
                line = wild_line[e];
                const int in_e = input_field((int)e);
                Op& op = push(Op::Tau);
                op.fields = { sf(wild[e]) };
                op.in = lin({ { in_e, 2.0 } }, -0.5);
                op.note = who + "on " + s.tokens[e] + ", every state goes to " + std::to_string(wild[e]);
                continue;
            }
            for (int st = 0; st < S; ++st) {
                const int to = m.delta[(size_t)st * T + e];
                if (to == st) continue;
                line = ex[(size_t)st * T + e] >= 0 ? ex_line[(size_t)st * T + e] : wild_line[e];
                const int in_e = input_field((int)e);
                Op& op = push(Op::Tau);
                op.fields = { sf(to) };
                op.in = lin({ { in_e, 2.0 }, { sf(st), 2.0 } }, -2.5);
                op.note = who + "on " + s.tokens[e] + ", state " + std::to_string(st) + " goes to " + std::to_string(to);
            }
        }
        line = m.line;
        for (int st = 0; st < S; ++st) {
            std::vector<int> leaves;                              // the tokens on which this state does not hold
            for (size_t e = 0; e < T; ++e)
                if (pure[e] || m.delta[(size_t)st * T + e] != st) leaves.push_back((int)e);
            if (leaves.size() == T) continue;                     // it never holds: no rule needed
            Lin in;
            in.terms.push_back({ sf(st), 2.0 });
            for (int e : leaves) in.terms.push_back({ input_field(e), -2.0 });
            in.constant = -0.5;
            Op& op = push(Op::Tau);
            op.fields = { sf(st) };
            op.in = in;
            op.note = who + "state " + std::to_string(st) + (leaves.empty() ? " holds on every token" : " holds unless " + any_of(leaves) + " arrives");
        }
        std::vector<int> all;
        for (int st = 0; st < S; ++st) {
            s.fields[(size_t)sf(st)].clear_line = m.line;
            all.push_back(sf(st));
        }
        Op& op = push(Op::Clear);
        op.fields = all;
        op.note = who + "every state is rewritten at each step";
        cur_sugar = -1;
        line = keep_line;
    }

    void statement() {
        const Tok& k = t[0];
        const std::string w = k.text;
        if (open >= 0 && w != "on") close_machine();
        if (k.kind != Tok::Word) fail(k.col, std::string("a statement starts with one of: ") + kStatements);
        if (w == "net") st_net();
        else if (w == "tokens") st_tokens();
        else if (w == "fields") st_fields();
        else if (w == "embed") st_embed();
        else if (w == "init") st_init();
        else if (w == "tau") st_tau();
        else if (w == "neuron") st_neuron();
        else if (w == "clear") st_clear();
        else if (w == "readout") st_readout();
        else if (w == "emit") st_emit();
        else if (w == "latch") st_latch();
        else if (w == "machine") st_machine();
        else if (w == "on") st_on();
        else if (w == "rotate") st_rotate();
        else if (w == "width") st_width();
        else {
            std::vector<std::string> words;
            for (size_t a = 0, b; a < strlen(kStatements); a = b + 1) {
                b = std::string_view(kStatements).find(' ', a);
                if (b == std::string::npos) b = strlen(kStatements);
                words.emplace_back(kStatements + a, b - a);
            }
            std::string lower = w;
            for (auto& c : lower) c = (char)tolower((unsigned char)c);
            const std::string near = std::find(words.begin(), words.end(), lower) != words.end() ? lower : nearest(w, words);
            fail(k.col, "unknown statement '" + w + "'. The statements are: " + kStatements,
                 near.empty() ? "" : near == lower ? "statements are lowercase: write '" + near + "'" : "did you mean '" + near + "'?");
        }
    }

    void warn(int at_line, const char* code, const std::string& msg, const std::string& hint) {
        Issue i;
        i.code = code;
        i.message = msg;
        i.line = at_line;
        i.source = at_line >= 1 && at_line <= (int)s.lines.size() ? s.lines[(size_t)at_line - 1] : std::string();
        i.hint = hint;
        s.warnings.push_back(std::move(i));
    }

    void finish() {
        line = (int)s.lines.size();
        src.clear();
        if (open >= 0) {
            try { close_machine(); } catch (const Stop&) {}
        }
        if (!autos.empty()) {
            line = 0;
            Op& op = push(Op::Clear);
            op.generated = true;
            op.fields = autos;
            op.note = "an input: back to 0 at the end of every step";
        }
        line = 0;
        if (!net_line) add(0, "the specification has no `net NAME` statement", "for example: net R1_plain");
        if (!tokens_line) add(0, "the specification has no `tokens` statement", "for example: tokens zero one");
        if (!readout_line) add(0, "the specification has no readout: add `readout EXPRESSION` or `emit FIELDS`", "for example: readout 8*A - 4");
        const int nf = (int)s.fields.size();
        if (!s.width_explicit) s.width = std::max(32, 8 * ((nf + 4 + 7) / 8));
        else if (s.width < nf) {
            line = width_line;
            src = s.lines[(size_t)width_line - 1];
            add(1, ssprintf("width %d is too small: the specification has %d fields, the constant `one` included", s.width, nf),
                ssprintf("use width %d or more, or leave the line out for the default", nf));
        }
        if (s.width > kMaxWidth) add(0, ssprintf("the network needs width %d; the limit is %d", s.width, kMaxWidth));
        if (!errors.empty()) return;
        // warnings: precise ones only
        std::vector<int> tau_line(s.fields.size(), 0), embed_line(s.fields.size(), 0);
        std::vector<char> read(s.fields.size(), 0), written(s.fields.size(), 0);
        for (const auto& op : s.ops) {
            if (op.kind == Op::Tau)
                for (int f : op.fields) { written[(size_t)f] = 1; if (!tau_line[(size_t)f]) tau_line[(size_t)f] = op.line ? op.line : -1; }
            if (op.kind == Op::Embed)
                for (int f : op.fields) { written[(size_t)f] = 1; if (!embed_line[(size_t)f]) embed_line[(size_t)f] = op.line ? op.line : -1; }
            if (op.kind == Op::Neuron)
                for (const auto& x : op.out.terms) written[(size_t)x.field] = 1;
            if (op.kind == Op::Tau || op.kind == Op::Neuron || op.kind == Op::Readout)
                for (const auto& x : op.in.terms) read[(size_t)x.field] = 1;
        }
        for (size_t f = 1; f < s.fields.size(); ++f) {
            const FieldDecl& fd = s.fields[f];
            if (!fd.clear_line && tau_line[f])
                warn(fd.line, "W_NOT_CLEARED", "the field '" + fd.name + "' is written by a tau (line " + std::to_string(std::max(tau_line[f], 0)) +
                                                   ") but never cleared: every write adds to its old value",
                     "add: clear " + fd.name);
            else if (!fd.clear_line && embed_line[f])
                warn(fd.line, "W_NOT_CLEARED", "a token adds 1 to the field '" + fd.name + "' (line " + std::to_string(std::max(embed_line[f], 0)) +
                                                   ") and nothing clears it: it grows by 1 at every arrival",
                     "add: clear " + fd.name);
            if (!read[f] && !written[f] && fd.init == 0)
                warn(fd.line, "W_UNUSED_FIELD", "the field '" + fd.name + "' is declared but nothing reads or writes it", "");
        }
    }

    void run(const std::string& text) {
        s.text = text;
        s.sha256 = sha256_hex(text);
        {
            FieldDecl one;
            one.name = "one";
            one.kind = FieldDecl::One;
            one.init = 1;
            s.fields.push_back(one);
            field_at["one"] = 0;
        }
        if (!valid_utf8(text)) {
            add(0, "the specification is not valid UTF-8 text");
            return;
        }
        size_t at = text.size() >= 3 && text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
        while (at <= text.size()) {
            size_t nl = text.find('\n', at);
            if (nl == std::string::npos) nl = text.size();
            std::string ln = text.substr(at, nl - at);
            if (!ln.empty() && ln.back() == '\r') ln.pop_back();
            s.lines.push_back(ln);
            at = nl + 1;
        }
        if (!s.lines.empty() && s.lines.back().empty()) s.lines.pop_back();
        for (size_t i = 0; i < s.lines.size(); ++i) {
            line = (int)i + 1;
            src = s.lines[i];
            const size_t hash = src.find('#');
            note = hash == std::string::npos ? std::string() : trim(std::string_view(src).substr(hash + 1));
            cur_sugar = -1;
            rounded_now.clear();
            try {
                lex(hash == std::string::npos ? src : src.substr(0, hash));
                if (t.empty()) continue;
                statement();
                if (!rounded_now.empty()) warn_rounded();
            } catch (const Stop&) {
            }
        }
        finish();
    }
    // W_ROUNDED_CONSTANT: the numbers of this statement that float64 cannot hold, each with what is stored
    void warn_rounded() {
        std::string msg = "float64 cannot hold every number on this line, so the network keeps the nearest value: ";
        for (size_t i = 0; i < rounded_now.size(); ++i) {
            const IssueValue& r = rounded_now[i];
            msg += (i ? "; " : "") + (r.what == "number" ? "`" + r.written + "`" : r.what + (r.written.empty() ? "" : " `" + r.written + "`")) +
                   " is stored as " + ssprintf("%.17g", r.stored) + " (written minus stored: " + ssprintf("%.3g", r.difference) + ")";
        }
        msg += ". The proof is about the stored network";
        warn(line, "W_ROUNDED_CONSTANT", msg,
             "numbers that are exact in binary (2, 0.5, 2.5, 0.375, 1/4) are stored as written");
        s.warnings.back().values = rounded_now;
    }
};

}  // namespace

Spec parse_spec(const std::string& text) {
    Parser p;
    p.run(text);
    if (!p.errors.empty()) {
        Fail f;
        f.issues = std::move(p.errors);
        throw f;
    }
    return std::move(p.s);
}

// ======================================================================
// printing statements back
// ======================================================================
static std::string lin_names(const std::vector<std::string>& names, const Lin& l) {
    std::string out;
    for (const auto& x : l.terms) {
        const double a = std::fabs(x.coef);
        const std::string body = (a == 1 ? std::string() : num(a) + "*") + names[(size_t)x.field];
        if (out.empty()) out = (x.coef < 0 ? "-" : "") + body;
        else out += (x.coef < 0 ? " - " : " + ") + body;
    }
    if (out.empty()) return num(l.constant);
    if (l.constant != 0) out += (l.constant < 0 ? " - " : " + ") + num(std::fabs(l.constant));
    return out;
}

std::string lin_text(const Spec& s, const Lin& l) {
    std::vector<std::string> names;
    for (const auto& f : s.fields) names.push_back(f.name);
    return lin_names(names, l);
}

std::string op_text(const Spec& s, const Op& op) {
    const auto list = [&](const std::vector<int>& fs) {
        std::string out;
        for (int f : fs) out += " " + s.fields[(size_t)f].name;
        return out;
    };
    switch (op.kind) {
        case Op::Fields: return "fields" + list(op.fields);
        case Op::Embed: return "embed " + s.tokens[(size_t)op.token] + list(op.fields);
        case Op::Init: {
            std::string out = "init";
            for (size_t i = 0; i < op.fields.size(); ++i) out += " " + s.fields[(size_t)op.fields[i]].name + "=" + num(op.values[i]);
            return out;
        }
        case Op::Tau: return "tau" + list(op.fields) + " <- " + lin_text(s, op.in);
        case Op::Neuron: return "neuron " + lin_text(s, op.in) + " -> " + lin_text(s, op.out);
        case Op::Clear: return "clear" + list(op.fields);
        case Op::Readout: return "readout " + lin_text(s, op.in);
    }
    return std::string();
}

bool implied_by_input(const Spec& s, const Op& op) {
    if (!op.generated || (op.kind != Op::Embed && op.kind != Op::Clear) || op.fields.empty()) return false;
    for (int f : op.fields)
        if (!s.fields[(size_t)f].auto_clear) return false;
    return true;
}

std::string expanded_text(const Spec& s) {
    std::string out = "# " + s.name + ": the same network in primitive statements only (what quill assembles)\n";
    std::vector<std::string> inputs;
    for (const auto& f : s.fields)
        if (f.auto_clear) inputs.push_back(f.name);
    if (!inputs.empty())
        out += "# " + join(inputs, ", ") +
               (inputs.size() == 1 ? " is the input field of its token: quill embeds the token into it and clears it after the last rule\n"
                                   : " are the input fields of their tokens: quill embeds each token into its field and clears the field after the last rule\n");
    out += "net " + s.name + "\ntokens " + join(s.tokens, " ") + "\n";
    for (const auto& op : s.ops) {
        if (implied_by_input(s, op)) continue;
        std::string ln = op_text(s, op);
        if (!op.note.empty()) {
            std::string n = op.note;
            for (auto& c : n)
                if (c == '\n' || c == '\r') c = ' ';
            ln += "    # " + n;
        }
        out += ln + "\n";
    }
    if (s.rotate) out += "rotate " + std::to_string(s.seed) + "\n";
    out += "width " + std::to_string(s.width) + "\n";
    return out;
}

// ======================================================================
// the assembler
// ======================================================================
Built assemble(const Spec& s) {
    Built b;
    Network& net = b.net;
    net.name = s.name;
    net.tokens = s.tokens;
    net.d = s.width;
    size_t n = 0;
    for (const auto& op : s.ops) {
        if (op.kind == Op::Tau) n += 2;
        else if (op.kind == Op::Neuron) n += 1;
        else if (op.kind == Op::Clear) n += 2 * op.fields.size();
    }
    const size_t d = (size_t)s.width, T = s.tokens.size();
    if (n > 1000000 || n * d > 8000000)
        throw Fail("E_LIMIT", ssprintf("the network would have %zu neurons at width %zu: more than quill builds (1,000,000 neurons, "
                                       "8,000,000 weights in W1)", n, d),
                   "a machine statement costs about two neurons per listed (state, token) pair");
    net.n = (int)n;
    net.E.assign(T * d, 0.0);
    net.W1.assign(n * d, 0.0);
    net.b1.assign(n, 0.0);
    net.W2.assign(d * n, 0.0);
    net.r0.assign(d, 0.0);
    net.Wout.assign(2 * d, 0.0);
    net.bout.assign(2, 0.0);
    for (size_t f = 0; f < s.fields.size(); ++f) net.r0[f] = s.fields[f].init;
    net.r0[0] = 1.0;
    b.neuron_rule.assign(n, -1);
    size_t i = 0;
    const auto row = [&](size_t neuron, const Lin& l, double bias) {
        for (const auto& x : l.terms) net.W1[neuron * d + (size_t)x.field] += x.coef;
        net.b1[neuron] = bias;
    };
    const auto rule = [&](Rule::Kind kind, const Op& op, size_t n0, bool pair, std::string text) -> Rule& {
        Rule r;
        r.kind = kind;
        r.n0 = (int)n0;
        r.n1 = pair ? (int)n0 + 1 : -1;
        r.line = op.line;
        r.text = std::move(text);
        r.note = op.note;
        if (op.sugar >= 0) r.from = s.sugar[(size_t)op.sugar].text;
        else if (op.generated) r.from = "automatic";
        b.neuron_rule[n0] = (int)b.rules.size();
        if (pair) b.neuron_rule[n0 + 1] = (int)b.rules.size();
        b.rules.push_back(std::move(r));
        return b.rules.back();
    };
    for (const auto& op : s.ops) {
        switch (op.kind) {
            case Op::Embed:
                for (int f : op.fields) net.E[(size_t)op.token * d + (size_t)f] += 1.0;
                break;
            case Op::Tau: {
                row(i, op.in, op.in.constant);
                row(i + 1, op.in, op.in.constant - 1.0);
                for (int f : op.fields) {
                    net.W2[(size_t)f * n + i] += 1.0;
                    net.W2[(size_t)f * n + i + 1] += -1.0;
                }
                rule(Rule::Tau, op, i, true, op_text(s, op)).targets = op.fields;
                i += 2;
                break;
            }
            case Op::Neuron: {
                row(i, op.in, op.in.constant);
                Rule& r = rule(Rule::Neuron, op, i, false, op_text(s, op));
                for (const auto& x : op.out.terms) {
                    net.W2[(size_t)x.field * n + i] += x.coef;
                    r.targets.push_back(x.field);
                }
                i += 1;
                break;
            }
            case Op::Clear:
                for (int f : op.fields) {
                    net.W1[i * d + (size_t)f] = 1.0;
                    net.W2[(size_t)f * n + i] = -1.0;
                    net.W1[(i + 1) * d + (size_t)f] = -1.0;
                    net.W2[(size_t)f * n + i + 1] = 1.0;
                    rule(Rule::Clear, op, i, true, "clear " + s.fields[(size_t)f].name).targets = { f };
                    i += 2;
                }
                break;
            case Op::Readout: {
                std::vector<double> c(d, 0.0);
                for (const auto& x : op.in.terms) c[(size_t)x.field] += x.coef;
                c[0] += op.in.constant;                            // the constant rides on `one`: no readout bias
                for (size_t j = 0; j < d; ++j) {
                    const double hold = -0.5 * c[j];
                    net.Wout[j] = hold == 0 ? 0.0 : hold;          // +0, never -0: the bytes the kit writes axis-aligned
                    net.Wout[d + j] = 0.5 * c[j];
                }
                break;
            }
            case Op::Fields:
            case Op::Init:
                break;
        }
    }
    return b;
}

// ======================================================================
// rotation
// ======================================================================
namespace {

// The natural logarithm with +, -, *, / only, so that it rounds the same way on every machine
// (the C library's log may not). Accurate to a few units in the last place; x > 0.
double det_log(double x) {
    int e = 0;
    while (x >= 2.0) { x *= 0.5; ++e; }
    while (x < 1.0) { x *= 2.0; --e; }
    if (x > 1.4142135623730951) { x *= 0.5; ++e; }            // now x is in (0.707, 1.414]
    const double u = (x - 1.0) / (x + 1.0), u2 = u * u;
    double sum = 0.0;
    for (int k = 12; k >= 0; --k) sum = sum * u2 + 1.0 / (double)(2 * k + 1);   // atanh series, Horner
    return (double)e * 0.6931471805599453 + 2.0 * u * sum;
}

}  // namespace

std::vector<double> rotation(int width, unsigned long long seed, int cols) {
    // Only the first `cols` columns of Q are computed (all of them when cols >= width). Column j of Gram-Schmidt
    // depends only on columns 0 to j-1 and on column j of the Gaussian matrix, so these columns are bit for bit the
    // full rotation's. The Gaussian matrix is still drawn whole and in the same order, row by row, so that a seed
    // means the same matrix; only the entries of the kept columns are stored, and the logarithm and square root are
    // skipped for a pair of draws that lands in no kept column (the acceptance test needs u and v only).
    // (Until 0.1.2 every column was computed: O(width^3), minutes at width 4096, for columns nothing used.)
    const size_t d = (size_t)width, m = std::min(d, (size_t)std::max(cols, 0)), total = d * d;
    Rng rng(seed);
    std::vector<double> g(d * m);
    for (size_t k = 0; k < total;) {                           // Gaussians by the polar method, in pairs
        const double u = 2.0 * rng.uniform() - 1.0, v = 2.0 * rng.uniform() - 1.0;
        const double sq = u * u + v * v;
        if (sq >= 1.0 || sq == 0.0) continue;
        const bool second = k + 1 < total;
        const bool keep0 = k % d < m, keep1 = second && (k + 1) % d < m;
        if (keep0 || keep1) {
            const double f = std::sqrt(-2.0 * det_log(sq) / sq);
            if (keep0) g[(k / d) * m + k % d] = u * f;
            if (keep1) g[((k + 1) / d) * m + (k + 1) % d] = v * f;
        }
        k += second ? 2 : 1;
    }
    // Gram-Schmidt on the columns, twice per column: the Q of the QR factorisation whose R has a
    // positive diagonal, orthogonal to rounding error. Row-major, row stride m.
    std::vector<double> q(d * m, 0.0), v(d);
    for (size_t j = 0; j < m; ++j) {
        for (size_t i = 0; i < d; ++i) v[i] = g[i * m + j];
        for (int pass = 0; pass < 2; ++pass)
            for (size_t k = 0; k < j; ++k) {
                double c = 0.0;
                for (size_t i = 0; i < d; ++i) c += q[i * m + k] * v[i];
                for (size_t i = 0; i < d; ++i) v[i] -= c * q[i * m + k];
            }
        double nn = 0.0;
        for (size_t i = 0; i < d; ++i) nn += v[i] * v[i];
        const double norm = std::sqrt(nn);
        if (!(norm > 1e-9)) throw Fail("E_INTERNAL", "the rotation is degenerate for this seed; choose another seed");
        for (size_t i = 0; i < d; ++i) q[i * m + j] = v[i] / norm;
    }
    return q;
}

Network rotated(const Network& a, const std::vector<double>& q, size_t m) {
    Network r = a;
    const size_t d = (size_t)a.d, n = (size_t)a.n, T = (size_t)a.T();
    const auto column = [&](size_t f) {                       // q holds the first m columns of Q only
        if (f >= m) throw Fail("E_INTERNAL", "a weight lies outside the columns of the rotation that were computed");
    };
    // rows that multiply the state on the right: X' = X Q^T, summed over the field index in order
    const auto right = [&](const std::vector<double>& x, size_t rows) {
        std::vector<double> out(rows * d, 0.0);
        for (size_t k = 0; k < rows; ++k)
            for (size_t f = 0; f < d; ++f) {
                const double c = x[k * d + f];
                if (c == 0) continue;
                column(f);
                for (size_t i = 0; i < d; ++i) out[k * d + i] += c * q[i * m + f];
            }
        return out;
    };
    r.E = right(a.E, T);
    r.W1 = right(a.W1, n);
    r.Wout = right(a.Wout, 2);
    r.r0 = right(a.r0, 1);                                    // Q r0 is r0^T Q^T
    r.W2.assign(d * n, 0.0);                                  // W2' = Q W2
    for (size_t f = 0; f < d; ++f)
        for (size_t k = 0; k < n; ++k) {
            const double c = a.W2[f * n + k];
            if (c == 0) continue;
            column(f);
            for (size_t i = 0; i < d; ++i) r.W2[i * n + k] += q[i * m + f] * c;
        }
    return r;
}

// How many leading columns of Q a rotated build needs: every coordinate any weight or the start state touches
// (the fields'), and up to four spares beyond the fields for the manifest's true shams.
size_t rotation_columns(const Network& a, size_t fields) {
    const size_t d = (size_t)a.d, n = (size_t)a.n;
    size_t used = std::min(d, fields + 4);
    const auto scan = [&](const std::vector<double>& x) {
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i] != 0) used = std::max(used, i % d + 1);
    };
    scan(a.E);
    scan(a.W1);
    scan(a.Wout);
    scan(a.r0);
    for (size_t f = 0; f < d; ++f)
        for (size_t k = 0; k < n; ++k)
            if (a.W2[f * n + k] != 0) used = std::max(used, f + 1);
    return used;
}

// ======================================================================
// the model
// ======================================================================
int Model::field(std::string_view name) const {
    for (size_t i = 0; i < fields.size(); ++i)
        if (fields[i] == name) return (int)i;
    return -1;
}

double Model::value(const double* state, int f) const {
    if (!rotated) return state[coord[(size_t)f]];
    const double* q = dirs.data() + (size_t)f * (size_t)net.d;
    double acc = 0;
    for (int j = 0; j < net.d; ++j) acc += q[j] * state[j];
    return acc;
}

std::string Model::lin_text(const Lin& l) const { return lin_names(fields, l); }

Lin Model::rule_lin(int rule) const {
    Lin l;
    const size_t i = (size_t)rules[(size_t)rule].n0, Fz = (size_t)F();
    for (size_t f = 0; f < Fz; ++f) {
        const double c = rd[i * Fz + f];
        if (std::fabs(c) > kTol) l.terms.push_back({ (int)f, c });
    }
    l.constant = net.b1[i];
    return l;
}

static double snap(double v) {
    const double s = std::round(v * 1e9) / 1e9;
    return std::fabs(s - v) <= kTol && std::fabs(v) < 1e6 ? (s == 0 ? 0.0 : s) : v;
}

void Model::finish() {
    const size_t Fz = (size_t)F(), d = (size_t)net.d, n = (size_t)net.n, T = (size_t)net.T();
    one = field("one");
    const auto project = [&](const double* x, size_t stride, double* out, double* outside) {
        double total = 0, named = 0;
        for (size_t j = 0; j < d; ++j) total += x[j * stride] * x[j * stride];
        for (size_t f = 0; f < Fz; ++f) {
            double c = 0;
            if (!rotated) c = x[(size_t)coord[f] * stride];
            else
                for (size_t j = 0; j < d; ++j) c += dirs[f * d + j] * x[j * stride];
            out[f] = c;
            named += c * c;
        }
        if (outside) *outside = std::sqrt(std::max(0.0, total - named));
    };
    rd.assign(n * Fz, 0.0);
    wr.assign(n * Fz, 0.0);
    em.assign(T * Fz, 0.0);
    r0f.assign(Fz, 0.0);
    ro.assign(Fz, 0.0);
    rd_out.assign(n, 0.0);
    wr_out.assign(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        project(net.W1.data() + i * d, 1, rd.data() + i * Fz, &rd_out[i]);
        project(net.W2.data() + i, n, wr.data() + i * Fz, &wr_out[i]);
        if (rd_out[i] < 1e-6) rd_out[i] = 0;                    // the square root of a rounding residue
        if (wr_out[i] < 1e-6) wr_out[i] = 0;
    }
    for (size_t t = 0; t < T; ++t) project(net.E.data() + t * d, 1, em.data() + t * Fz, nullptr);
    project(net.r0.data(), 1, r0f.data(), nullptr);
    std::vector<double> wm(d);
    for (size_t j = 0; j < d; ++j) wm[j] = net.Wout[d + j] - net.Wout[j];
    project(wm.data(), 1, ro.data(), nullptr);
    // name the rules that were recovered from weights
    for (size_t k = 0; k < rules.size(); ++k) {
        Rule& r = rules[k];
        if (!r.text.empty()) continue;
        if (Fz == 0) {
            r.text = r.kind == Rule::Neuron ? ssprintf("neuron %d", r.n0) : ssprintf("%s: neurons %d and %d", r.kind == Rule::Tau ? "tau" : "clear", r.n0, r.n1);
            continue;
        }
        const size_t i = (size_t)r.n0;
        Lin in = rule_lin((int)k), out;
        for (auto& x : in.terms) x.coef = snap(x.coef);
        in.constant = snap(in.constant);
        bool unit = true;
        for (size_t f = 0; f < Fz; ++f) {
            const double c = wr[i * Fz + f];
            if (std::fabs(c) <= kTol) continue;
            out.terms.push_back({ (int)f, snap(c) });
            r.targets.push_back((int)f);
            if (std::fabs(c - 1.0) > kTol) unit = false;
        }
        const std::string outside = (rd_out[i] > 0 ? " (also reads outside the named fields)" : "") +
                                    std::string(wr_out[i] > 0 ? " (also writes outside the named fields)" : "");
        if (r.kind == Rule::Clear && r.targets.size() == 1) r.text = "clear " + fields[(size_t)r.targets[0]];
        else if (r.kind == Rule::Tau && unit && !out.terms.empty()) {
            r.text = "tau";
            for (int f : r.targets) r.text += " " + fields[(size_t)f];
            r.text += " <- " + lin_text(in) + outside;
        } else {
            r.text = std::string(r.kind == Rule::Tau ? "threshold " : "neuron ") + lin_text(in) + " -> " +
                     (out.terms.empty() ? std::string("nothing named") : lin_text(out)) + outside;
        }
    }
}

Model model_from_spec(const Spec& s, bool allow_rotate) {
    Built b = assemble(s);
    Model m;
    m.origin = "specification";
    m.rules = std::move(b.rules);
    m.neuron_rule = std::move(b.neuron_rule);
    m.machines = s.machines;
    m.warnings = s.warnings;
    m.spec_text = s.text;
    for (const auto& f : s.fields) m.fields.push_back(f.name);
    m.input = JV::object();
    m.input.set("kind", "specification").set("sha256", s.sha256);
    const size_t d = (size_t)s.width, Fz = s.fields.size();
    if (s.rotate && allow_rotate) {
        const size_t cols = rotation_columns(b.net, Fz);
        const std::vector<double> q = rotation(s.width, s.seed, (int)cols);
        m.net = rotated(b.net, q, cols);
        m.rotated = true;
        m.seed = s.seed;
        m.seed_known = true;
        m.dirs.assign(Fz * d, 0.0);
        for (size_t f = 0; f < Fz; ++f)
            for (size_t j = 0; j < d; ++j) m.dirs[f * d + j] = q[j * cols + f];
        m.notes.push_back("rotated by quill with seed " + std::to_string(s.seed) + ": every field lies along a column of Q");
        for (size_t f = Fz; f < d && f < Fz + 4; ++f) {            // unused directions, for the manifest's true shams
            m.sham_names.push_back("spare" + std::to_string(f - Fz));
            for (size_t j = 0; j < d; ++j) m.sham_dirs.push_back(q[j * cols + f]);
        }
    } else {
        m.net = std::move(b.net);
        for (size_t f = 0; f < Fz; ++f) m.coord.push_back((int)f);
        for (size_t f = Fz; f < d && f < Fz + 4; ++f) {
            m.sham_names.push_back("spare" + std::to_string(f - Fz));
            for (size_t j = 0; j < d; ++j) m.sham_dirs.push_back(j == f ? 1.0 : 0.0);
        }
    }
    m.finish();
    return m;
}

Model model_from_text(const std::string& spec_text, int rotate_mode, unsigned long long seed) {
    Spec s = parse_spec(spec_text);
    if (rotate_mode == 1) s.rotate = false;
    if (rotate_mode == 2) {
        if (seed > kMaxSeed) throw Fail("E_ARGS", "a rotation seed is a whole number from 0 to 9007199254740991");
        s.rotate = true;
        s.seed = seed;
    }
    Model m = model_from_spec(s, true);
    if (rotate_mode) m.input.set("rotate", rotate_mode == 1 ? JV("none") : JV((unsigned long long)seed));
    return m;
}

Model model_from_folder(const Folder& f) {
    JV input = JV::object();
    input.set("kind", "folder").set("weights_sha256", f.weights_sha256).set("network_sha256", f.network_sha256);
    input.set("manifest_sha256", f.manifest_sha256.empty() ? JV() : JV(f.manifest_sha256));
    const size_t d = (size_t)f.net.d, n = (size_t)f.net.n;
    std::vector<Issue> warnings;
    // A folder quill built carries its specification: rebuild it and compare bit for bit.
    const JV* qj = f.manifest.t == JV::Obj ? f.manifest.get("quill") : nullptr;
    if (qj && qj->t == JV::Obj && qj->get("spec") && qj->get("spec")->is_str()) {
        bool same = false;
        std::string why;
        try {
            const JV* rs = qj->get("rotate_seed");
            const bool has_seed = rs && rs->t == JV::Int;
            Model m = model_from_text(qj->get("spec")->s, has_seed ? 2 : 1, has_seed ? (unsigned long long)rs->i : 0);
            same = m.net.E == f.net.E && m.net.W1 == f.net.W1 && m.net.b1 == f.net.b1 && m.net.W2 == f.net.W2 && m.net.r0 == f.net.r0 &&
                   m.net.Wout == f.net.Wout && m.net.bout == f.net.bout && m.net.tokens == f.net.tokens;
            if (same) {
                m.origin = "quill folder";
                m.input = input;
                m.net.name = f.net.name;
                m.notes.push_back("the specification kept in manifest.json rebuilds weights.npz bit for bit");
                return m;
            }
            why = "it builds different weights";
        } catch (const Fail& e) {
            why = e.first().message;
        }
        Issue w;
        w.code = "W_SPEC_MISMATCH";
        w.message = "manifest.json carries a specification that does not rebuild these weights (" + why +
                    "); working from the weights and the field directions alone";
        warnings.push_back(std::move(w));
    }
    Model m;
    m.net = f.net;
    m.input = input;
    m.warnings = std::move(warnings);
    const JV* kit = f.network.get("kit_version");
    m.origin = (kit && !f.network.get("quill")) ? "kit folder" : "folder";
    // the named fields and their directions
    const JV* fj = f.manifest.t == JV::Obj ? f.manifest.get("fields") : nullptr;
    if (fj && fj->t == JV::Obj) {
        bool axis = true;
        std::vector<int> coord;
        for (const auto& kv : fj->obj) {
            if (kv.second.t != JV::Arr || kv.second.arr.size() != d)
                throw Fail("E_FORMAT", ssprintf("manifest.json: the direction of field '%s' must be a list of %zu numbers", kv.first.c_str(), d));
            int at = -1, nonzero = 0;
            for (size_t j = 0; j < d; ++j) {
                if (!kv.second.arr[j].is_num())
                    throw Fail("E_FORMAT", "manifest.json: the direction of field '" + kv.first + "' holds something that is not a number");
                const double v = kv.second.arr[j].d;
                m.dirs.push_back(v);
                if (v != 0) { ++nonzero; at = v == 1.0 ? (int)j : -1; }
            }
            if (nonzero != 1 || at < 0) axis = false;
            coord.push_back(at);
            m.fields.push_back(kv.first);
        }
        m.rotated = !axis;
        if (axis) { m.coord = coord; m.dirs.clear(); }
        else {
            double worst = 0;
            const size_t Fz = m.fields.size();
            for (size_t a = 0; a < Fz; ++a)
                for (size_t b = a; b < Fz; ++b) {
                    double dot = 0;
                    for (size_t j = 0; j < d; ++j) dot += m.dirs[a * d + j] * m.dirs[b * d + j];
                    worst = std::max(worst, std::fabs(dot - (a == b ? 1.0 : 0.0)));
                }
            if (worst > 1e-9) {
                Issue w;
                w.code = "W_DIRECTIONS";
                w.message = "the field directions in manifest.json are not orthonormal (largest error " + sci(worst) + "): field values read along them are approximate";
                m.warnings.push_back(std::move(w));
            }
            m.notes.push_back("field values are read along the directions in manifest.json");
        }
    } else {
        m.notes.push_back("no field names: manifest.json is missing or lists no fields, so states are raw coordinates");
    }
    // recover the rules from the weights: threshold pairs, clear pairs, lone neurons
    const auto row = [&](size_t i, size_t j) { return f.net.W1[i * d + j]; };
    const auto col = [&](size_t i, size_t j) { return f.net.W2[j * n + i]; };
    const auto same_pair = [&](size_t a, size_t b, double row_sign, double bias_shift) {
        if (std::fabs(f.net.b1[b] - (row_sign * f.net.b1[a] + bias_shift)) > kTol) return false;
        for (size_t j = 0; j < d; ++j)
            if (std::fabs(row(b, j) - row_sign * row(a, j)) > kTol || std::fabs(col(b, j) + col(a, j)) > kTol) return false;
        return true;
    };
    const auto is_clear = [&](size_t a, size_t b) {
        if (f.net.b1[a] != 0 || !same_pair(a, b, -1.0, 0.0)) return false;
        double norm2 = 0;
        for (size_t j = 0; j < d; ++j) {
            if (std::fabs(col(a, j) + row(a, j)) > kTol) return false;
            norm2 += row(a, j) * row(a, j);
        }
        return std::fabs(norm2 - 1.0) <= 1e-9;
    };
    std::vector<char> used(n, 0);
    m.neuron_rule.assign(n, -1);
    for (size_t i = 0; i < n; ++i) {
        if (used[i]) continue;
        Rule r;
        r.kind = Rule::Neuron;
        r.n0 = (int)i;
        for (int tries = 0; tries < 2 && r.n1 < 0; ++tries)        // the neighbour first, then anyone
            for (size_t j = i + 1; j < (tries == 0 ? std::min(n, i + 2) : n); ++j) {
                if (used[j]) continue;
                if (same_pair(i, j, 1.0, -1.0)) { r.kind = Rule::Tau; r.n1 = (int)j; break; }
                if (is_clear(i, j)) { r.kind = Rule::Clear; r.n1 = (int)j; break; }
            }
        used[i] = 1;
        m.neuron_rule[i] = (int)m.rules.size();
        if (r.n1 >= 0) {
            used[(size_t)r.n1] = 1;
            m.neuron_rule[(size_t)r.n1] = (int)m.rules.size();
        }
        m.rules.push_back(std::move(r));
    }
    m.notes.push_back("the rules were recovered from the weights: pairs of neurons that form a threshold or a clear");
    m.finish();
    return m;
}

}  // namespace quill
