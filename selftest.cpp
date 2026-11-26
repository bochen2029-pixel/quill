// quill · selftest.cpp — every claim of the tool, checked from the inside. It reads the specs/
// folder beside the executable when there is one, and writes no file at all.
#include "app.h"
#include "npz.h"
#include "selftest_vectors.h"

#include <cmath>

namespace quill {

namespace {

struct Suite {
    JV checks = JV::array(), notes = JV::array();
    std::string text;
    int passed = 0, failed = 0;
    void check(bool ok, const std::string& what, const std::string& detail = std::string()) {
        JV c = JV::object();
        c.set("ok", ok).set("check", what).set("detail", detail.empty() ? JV() : JV(detail));
        checks.push(c);
        (ok ? passed : failed)++;
        text += std::string("  ") + (ok ? "PASS " : "FAIL ") + what + (detail.empty() ? "" : "  [" + detail + "]") + "\n";
    }
    void note(const std::string& line) {
        notes.push(line);
        text += "  info " + line + "\n";
    }
};

Source inline_spec(const std::string& text, int mode = 0, unsigned long long seed = 0) {
    Source s;
    s.is_text = true;
    s.text = text;
    s.path = "(inline)";
    s.rotate_mode = mode;
    s.seed = seed;
    return s;
}

const JV* dig(const JV& root, std::initializer_list<const char*> path) {
    const JV* p = &root;
    for (const char* k : path) {
        if (!p || p->t != JV::Obj) return nullptr;
        p = p->get(k);
    }
    return p;
}
long long dig_int(const JV& root, std::initializer_list<const char*> path) {
    const JV* p = dig(root, path);
    return p && p->t == JV::Int ? p->i : -1;
}
double dig_num(const JV& root, std::initializer_list<const char*> path) {
    const JV* p = dig(root, path);
    return p && p->is_num() ? p->d : -1e300;
}
std::string dig_str(const JV& root, std::initializer_list<const char*> path) {
    const JV* p = dig(root, path);
    return p && p->is_str() ? p->s : std::string();
}
bool dig_true(const JV& root, std::initializer_list<const char*> path) {
    const JV* p = dig(root, path);
    return p && p->t == JV::Bool && p->b;
}

std::string unhex(const char* hex) {
    std::string out;
    const auto v = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
    for (size_t i = 0; hex[i] && hex[i + 1]; i += 2) out += (char)(v(hex[i]) * 16 + v(hex[i + 1]));
    return out;
}

bool contains(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

// ---- the base: hashes, numbers, JSON, the random stream --------------------------------------------
void test_base(Suite& t) {
    std::string million(1000000, 'a');
    Sha256 pieces;
    for (size_t at = 0; at < million.size();) {
        const size_t n = std::min<size_t>(million.size() - at, 1 + (at * 7) % 191);
        pieces.update(million.data() + at, n);
        at += n;
    }
    t.check(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" &&
                sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" &&
                pieces.hex() == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            "sha256: the empty string, \"abc\", and a million 'a' fed in uneven pieces");
    t.check(crc32("123456789") == 0xCBF43926u, "crc32 of \"123456789\" is cbf43926");
    struct { double v; const char* want; } nums[] = {
        { 0.5, "0.5" }, { 1e-9, "1e-9" }, { 1e21, "1e+21" }, { 123456789012345680000.0, "123456789012345680000" }, { -0.0, "0" },
        { 5e-324, "5e-324" }, { 1.7976931348623157e308, "1.7976931348623157e+308" }, { 0.000001, "0.000001" }, { 1e-7, "1e-7" },
        { 100.0, "100" }, { 2.0000000000000004, "2.0000000000000004" }, { 8.0 / 3.0, "2.6666666666666665" }, { -2.5, "-2.5" },
        { 333333333.33333329, "333333333.3333333" }, { 1e30, "1e+30" }, { 4.5, "4.5" }, { 0.002, "0.002" }, { 1e-27, "1e-27" } };
    std::string bad;
    for (const auto& n : nums) {
        double back = 0;
        if (num(n.v) != n.want || !to_double(num(n.v) == "0" ? "0" : num(n.v), back) || back != n.v) bad += std::string(" ") + n.want;
    }
    t.check(bad.empty(), "numbers print in the shortest form that reads back exactly (18 cases, RFC 8785's among them)", bad);
    {
        JV j;
        std::string err;
        const bool ok = json_parse("{\"b\":[1,2.50,\"x\\u00e9\\n\"],\"a\":{\"z\":null,\"y\":true}}", j, err);
        t.check(ok && json_canonical(j) == "{\"a\":{\"y\":true,\"z\":null},\"b\":[1,2.5,\"x\xC3\xA9\\n\"]}", "JSON: parse, then canonical form with sorted keys", err);
        int refused = 0;
        for (const char* s : { "[1,]", "{\"a\":1,\"a\":2}", "01", "\"a\tb\"", "{\"a\":1} x", "[1 2]", "nul", "{'a':1}" })
            refused += !json_parse(s, j, err);
        t.check(refused == 8, "JSON: eight malformed texts are refused (trailing comma, repeated key, leading zero, raw tab, ...)");
    }
    {
        bool ok = true;
        const unsigned long long* want[3] = { vectors::kRng0, vectors::kRng1, vectors::kRng101 };
        const uint64_t seeds[3] = { 0, 1, 101 };
        for (int s = 0; s < 3; ++s) {
            Rng r(seeds[s]);
            for (int k = 0; k < 4; ++k) ok = ok && r.next() == want[s][k];
        }
        Rng r(7);
        int counts[5] = {};
        for (int k = 0; k < 50000; ++k) counts[r.below(5)]++;
        for (int c : counts) ok = ok && c > 9500 && c < 10500;
        t.check(ok, "the random stream matches an independent transcription (seeds 0, 1, 101) and below(5) is even");
    }
}

// ---- .npy, .npz, inflate -----------------------------------------------------------------------------
void test_npz(Suite& t) {
    std::string bad;
    for (const auto& v : vectors::kDeflate) {
        const std::string in = unhex(v.hex);
        std::string out, err;
        const bool ok = inflate(in, v.size, out, err) && sha256_hex(out) == v.sha256 && (int)(((unsigned char)in[0] >> 1) & 3) == v.block_type;
        if (!ok) bad += std::string(" ") + v.name + (err.empty() ? "" : ": " + err);
        std::string cut, e2;
        if (inflate(std::string_view(in).substr(0, in.size() / 2), v.size, cut, e2)) bad += std::string(" ") + v.name + " (accepted when cut in half)";
    }
    t.check(bad.empty(), "inflate: zlib's stored, fixed-code and dynamic-code streams give back their data; cut in half they are refused", bad);
    {
        const std::string file = unhex(vectors::kCompressedNpzHex);
        bool ok = false;
        std::string why;
        try {
            const std::vector<NpyArray> arrays = npz_read(file, "the test file");
            ok = arrays.size() == 2 && arrays[0].name == "a" && arrays[0].descr == "<f8" && arrays[0].shape == std::vector<size_t>{ 3, 4 } &&
                 arrays[1].name == "b" && arrays[1].descr == "<f4" && arrays[1].shape == std::vector<size_t>{ 3 };
            const std::vector<double> a = npy_f64(arrays[0], "t"), b = npy_f64(arrays[1], "t");
            for (size_t k = 0; k < 12 && ok; ++k) ok = a[k] == (double)k / 8.0;
            ok = ok && b[0] == 1.5 && b[1] == -2.25 && b[2] == 0.0;
            std::string damaged = file;
            damaged[damaged.size() / 3] ^= 0x10;
            bool refused = false;
            try { npz_read(damaged, "the damaged file"); } catch (const Fail&) { refused = true; }
            ok = ok && refused;
        } catch (const Fail& f) {
            why = f.first().message;
        }
        t.check(ok, "a numpy.savez_compressed file reads back (deflate, ZIP64 fields, float64 and float32); a flipped bit is refused", why);
    }
    {
        Network n;
        n.tokens = { "a", "b", "c" };
        n.d = 5;
        n.n = 0;                                                  // no neurons at all: arrays of shape (0, 5) and (5, 0)
        n.E.assign(15, 0.25);
        n.r0 = { 1, 0, -0.0, 5e-324, 1.7976931348623157e308 };
        n.Wout.assign(10, -1.5);
        n.bout = { 0, 0 };
        Network back;
        arrays_from_npz(weights_npz(n), "round trip", back);
        const bool ok = back.d == 5 && back.n == 0 && back.E == n.E && back.Wout == n.Wout && memcmp(back.r0.data(), n.r0.data(), 40) == 0;
        t.check(ok, "weights.npz written by quill reads back bit for bit, also with no neurons and extreme values");
    }
}

// ---- the language ------------------------------------------------------------------------------------
void test_language(Suite& t) {
    struct Case { const char* spec; const char* want; int line, column; const char* hint; };
    const Case cases[] = {
        { "net x\ntokens a b\nfields A\ntau A <- 2*B - 0.5\nclear A\nreadout 8*A - 4\n", "unknown field 'B'", 4, 12, "declare it first" },
        { "net x\ntokens a b\nfields A Bee\ntau A <- 2*Be - 0.5\nclear A\nreadout 8*A - 4\n", "unknown field 'Be'", 4, 12, "did you mean 'Bee'" },
        { "net x\ntokens a b\nfields A B\ntau A <- A*B\nclear A\nreadout A\n", "not linear", 4, 11, "" },
        { "net x\ntokens a b\nfields A\ntau A <- 2*A \xE2\x88\x92 0.5\nclear A\nreadout A\n", "unexpected character", 4, 14, "ASCII minus" },
        { "net x\ntokens a b\nfields A\ntau A \xE2\x86\x90 2*A\nclear A\nreadout A\n", "unexpected character", 4, 7, "write <-" },
        { "net x\nfields A\nreadout A\n", "has no `tokens` statement", 0, 0, "" },
        { "net x\ntokens a b\nfields A\nclear A\nclear A\nreadout A\n", "already cleared at line 4", 5, 7, "subtract it twice" },
        { "net x\ntokens a b\nfields A\ntau one <- 2*A\nreadout A\n", "`one` is the constant 1", 4, 5, "" },
        { "net x\ntokens a b\nfields A\ntua A <- 2*A\nreadout A\n", "unknown statement 'tua'", 4, 1, "did you mean 'tau'" },
        { "net x\ntokens a b\nfields A B\nreadout A\nwidth 2\n", "width 2 is too small", 5, 1, "use width 3" },
        { "net x\ntokens 1 0\nfields A\nreadout A\n", "would be token number 0", 2, 8, "its own position" },
        { "net x\ntokens a b\nfields A\ntau A <- 2A - 0.5\nreadout A\n", "neither a number nor a field name", 4, 10, "write 2*A" },
        { "net x\ntokens a b\nmachine S states=2\n  on a: 0->5\nemit S.1\n", "a state of machine S is expected", 4, 12, "" },
        { "net x\ntokens a b\nmachine S states=2\n  on a: 0->1\n  on a: 0->0\nemit S.1\n", "state 0 already goes to state 1 (line 4)", 5, 9, "" },
        { "net x\ntokens a b\nfields A\n", "has no readout", 0, 0, "" },
        { "net x\ntokens a b\nlatch A set=a toggle=a\nemit A\n", "has two roles", 3, 22, "" },
        { "net x\ntokens a b\nfields A\ntau A <- 2*in.zzz - 0.5\nreadout A\n", "'zzz' is not a token", 4, 12, "" },
        { "net x\ntokens a b\nfields A\nembed a A\nembed a A\nclear A\nreadout A\n", "already adds 1 to 'A'", 5, 9, "" },
        { "net x\ntokens a b\nfields A\ntau A <- 2/A\nreadout A\n", "divided by a number only", 4, 11, "" },
        { "net x\ntokens a b\nfields A\ntau A <- 2*in.a - 0.5\nclear in.a\nreadout A\n", "cleared by quill itself", 5, 7, "" },
        { "net x\ntokens a b\nfields A\ntau A 2*A\nreadout A\n", "a tau has the form", 4, 1, "for example" },
        { "net x\ntokens a b\nfields A spare3\nreadout A\n", "is reserved", 3, 10, "" },
        { "tokens a b\nfields A\nreadout A\n", "has no `net NAME` statement", 0, 0, "" },
    };
    std::string bad;
    for (const auto& c : cases) {
        try {
            parse_spec(c.spec);
            bad += std::string(" [accepted: ") + c.want + "]";
        } catch (const Fail& f) {
            const Issue& i = f.first();
            if (!contains(i.message, c.want) || i.line != c.line || i.column != c.column || !contains(i.hint, c.hint) || i.code != "E_SPEC")
                bad += std::string(" [") + c.want + ssprintf(": got line %d, column %d: ", i.line, i.column) + i.message + " | hint: " + i.hint + "]";
        }
    }
    t.check(bad.empty(), ssprintf("%zu faulty specifications are each refused with the right message, line, column and hint", std::size(cases)), bad);
    {
        size_t count = 0;
        try {
            parse_spec("net x\ntokens a b\nfields A\ntau A <- 2*B\ntau C <- 2*A\nfrobnicate\nclear A\nreadout A\n");
        } catch (const Fail& f) {
            count = f.issues.size();
        }
        t.check(count == 3, "every error of a specification is reported at once (three in one text)", ssprintf("%zu", count));
    }
    {
        bool ok = false;
        std::string why;
        try {
            const Spec s = parse_spec("net arith\ntokens a b\nfields A B\n"
                                      "tau A <- -(A - 2*in.a) - 1/2     # the meaning of this rule\n"
                                      "neuron 3*(A + B) - A -> 2*B - A\nclear A B\n"
                                      "readout 8/3*A - (2 + 2)*one + 1 - 1\n");
            const Built b = assemble(s);
            const size_t d = (size_t)s.width;
            const int A = s.field("A"), B = s.field("B"), ina = s.field("in.a");
            const auto w1 = [&](size_t n, int f) { return b.net.W1[n * d + (size_t)f]; };
            const auto w2 = [&](int f, size_t n) { return b.net.W2[(size_t)f * (size_t)b.net.n + n]; };
            ok = s.width == 32 && ina == 3 && b.net.n == 9 &&
                 w1(0, A) == -1 && w1(0, ina) == 2 && b.net.b1[0] == -0.5 && b.net.b1[1] == -1.5 && w2(A, 0) == 1 && w2(A, 1) == -1 &&
                 w1(2, A) == 2 && w1(2, B) == 3 && b.net.b1[2] == 0 && w2(B, 2) == 2 && w2(A, 2) == -1 &&
                 b.net.Wout[d + (size_t)A] == 0.5 * (8.0 / 3.0) && b.net.Wout[d] == -2 && b.net.Wout[0] == 2 && b.net.Wout[(size_t)B] == 0 &&
                 !std::signbit(b.net.Wout[(size_t)B]) && b.net.E[(size_t)ina] == 1 && b.net.r0[0] == 1 &&
                 b.rules.size() == 5 && b.rules[0].note == "the meaning of this rule" && b.rules[4].text == "clear in.a" && b.rules[4].from == "automatic";
        } catch (const Fail& f) {
            why = f.first().message;
        }
        t.check(ok, "expressions (+ - * / and parentheses), in.TOKEN fields, comments as meanings: the weights are the formulas", why);
    }
    {
        std::string fields28, fields27;
        for (int k = 0; k < 28; ++k) fields28 += " f" + std::to_string(k);
        for (int k = 0; k < 27; ++k) fields27 += " f" + std::to_string(k);
        const Spec a = parse_spec("net w\ntokens a\nfields" + fields27 + "\nreadout f0\n"), b = parse_spec("net w\ntokens a\nfields" + fields28 + "\nreadout f0\n");
        t.check(a.width == 32 && b.width == 40, "default width: fields plus four spares, rounded up to 8, at least 32");
    }
    {
        std::string bad2;
        for (const auto& ex : examples()) {
            if (!contains(ex.file, ".spec")) continue;
            try {
                const Spec s = parse_spec(ex.text);
                const std::string once = expanded_text(s);
                const Spec s2 = parse_spec(once);
                const Built b1 = assemble(s), b2 = assemble(s2);
                const bool same = b1.net.E == b2.net.E && b1.net.W1 == b2.net.W1 && b1.net.b1 == b2.net.b1 && b1.net.W2 == b2.net.W2 &&
                                  b1.net.r0 == b2.net.r0 && b1.net.Wout == b2.net.Wout && s2.sugar.empty();
                if (!same || expanded_text(s2) != once) bad2 += std::string(" ") + ex.name;
            } catch (const Fail& f) {
                bad2 += std::string(" ") + ex.name + ": " + f.first().message;
            }
        }
        t.check(bad2.empty(), "every example, written back in primitive statements, builds the identical weights (and the text is a fixed point)", bad2);
    }
}

// ---- the kernel against its definition -----------------------------------------------------------------
double reference_step(const Network& net, std::vector<double>& r, int tok) {
    const size_t d = (size_t)net.d, n = (size_t)net.n;
    std::vector<double> x(d), z(n);
    for (size_t j = 0; j < d; ++j) x[j] = r[j] + net.E[(size_t)tok * d + j];
    for (size_t i = 0; i < n; ++i) {
        double acc = net.b1[i];
        for (size_t j = 0; j < d; ++j) acc += net.W1[i * d + j] * x[j];
        z[i] = acc;
    }
    for (size_t k = 0; k < d; ++k) {
        double acc = x[k];
        for (size_t i = 0; i < n; ++i)
            if (z[i] > 0) acc += net.W2[k * n + i] * z[i];
        r[k] = acc;
    }
    double s[4] = { 0, 0, 0, 0 };
    for (size_t j = 0; j < d; ++j) s[j & 3] += (net.Wout[d + j] - net.Wout[j]) * r[j];
    return ((s[0] + s[1]) + (s[2] + s[3])) + (net.bout[1] - net.bout[0]);
}

void test_kernel(Suite& t) {
    size_t steps = 0, differ = 0;
    for (const char* name : { "R1_plain", "R3_overcomplete", "R5_redundant", "counter_mod3" })
        for (int rot = 0; rot < 2; ++rot) {
            const Model m = model_from_text(example(name)->text, rot ? 2 : 1, 12345);
            const Kernel K(m.net);
            std::vector<double> state((size_t)K.padded()), ref(m.net.r0);
            K.load(m.net.r0.data(), state.data());
            Rng rng(99);
            for (int k = 0; k < 3000; ++k, ++steps) {
                const int tok = (int)rng.below((uint32_t)m.net.T());
                K.step(state.data(), tok);
                const double want = reference_step(m.net, ref, tok);
                bool same = K.margin(state.data()) == want;
                for (size_t j = 0; j < (size_t)m.net.d && same; ++j) same = state[j] == ref[j];
                differ += !same;
            }
        }
    t.check(differ == 0, ssprintf("the kernel equals its written definition, value for value, on %zu steps (axis-aligned and rotated)", steps),
            differ ? ssprintf("%zu steps differ", differ) : std::string());
}

// ---- the kit's five articles as specifications ------------------------------------------------------------
int truth_decision(int which, int n) {
    switch (which) {
        case 1: return n % 3 != 0;               // R2: the count modulo 3
        case 2: return (n % 12) % 3 != 0;        // R3: the count modulo 12, read modulo 3
        default: return n % 2;                   // R1, R4, R5: the parity
    }
}

void test_kit_specs(Suite& t) {
    const char* names[5] = { "R1_plain", "R2_quotient", "R3_overcomplete", "R4_two_bits", "R5_redundant" };
    const long long want_states[5] = { 2, 3, 12, 4, 2 };
    const int steps = 20000;
    std::vector<int> toks((size_t)steps);
    Rng rng(20261126);
    for (auto& tk : toks) {
        const double u = rng.uniform();                           // the kit's stream: zero 15%, one 45%, two 20%, three 20%
        tk = u < 0.15 ? 0 : u < 0.60 ? 1 : u < 0.80 ? 2 : 3;
    }
    for (int k = 0; k < 5; ++k) {
        const std::string spec = example(names[k])->text;
        const std::string who = names[k];
        const Answer p = verb_prove(inline_spec(spec), ProveOptions(), nullptr);
        const bool proved = p.status == "ok" && p.grade == "proved" && dig_int(p.result, { "results", "reachable", "states" }) == want_states[k] &&
                            dig_true(p.result, { "results", "exactness", "holds" }) && dig_true(p.result, { "results", "cleanliness", "holds" }) &&
                            dig_num(p.result, { "results", "exactness", "min_distance" }) == 0.5 && dig_str(p.result, { "arithmetic" }) == "exact" &&
                            dig_num(p.result, { "results", "reachable", "closure_residual" }) == 0;
        t.check(proved, who + ": built and proved exact (" + std::to_string(want_states[k]) + " states, every threshold 0.5 clear of its ramp, exact arithmetic)",
                proved ? std::string() : p.text.substr(0, p.text.find('\n')));
        const Answer b = verb_build(inline_spec(spec), std::string(), false);
        t.check(dig_str(b.result, { "weights_sha256" }) == vectors::kKitNpz[k].sha256,
                who + ": weights.npz is byte-identical to the kit's own axis-aligned article as numpy saves it");
        size_t wrong = 0;
        for (int rot = 0; rot < 2; ++rot) {
            const Model m = model_from_text(spec, rot ? 2 : 1, 101 + (unsigned long long)k);
            const Kernel K(m.net);
            std::vector<double> state((size_t)K.padded());
            K.load(m.net.r0.data(), state.data());
            int n = 0;
            for (int tk : toks) {
                if (tk == 0) n = 0;
                else if (tk == 1) ++n;
                K.step(state.data(), tk);
                wrong += (K.margin(state.data()) > 0) != (truth_decision(k, n) != 0);
            }
        }
        const Answer pr = verb_prove(inline_spec(spec, 2, 101 + (unsigned long long)k), ProveOptions(), nullptr);
        const bool layered = pr.status == "ok" && pr.grade == "checked" && dig_true(pr.result, { "rotated", "holds" }) &&
                             dig_str(pr.result, { "results", "exactness", "grade" }) == "proved" &&
                             dig_num(pr.result, { "rotated", "closure_residual" }) < 1e-12;
        t.check(wrong == 0 && layered, who + ssprintf(": the decision is the planted truth on %d tokens, axis-aligned and rotated; the rotated build is checked against the logic", steps),
                wrong ? ssprintf("%zu wrong decisions", wrong) : layered ? std::string() : pr.text.substr(0, pr.text.find('\n')));
    }
    struct Eq { const char* a; const char* b; bool same; const char* differs; };
    const Eq eqs[] = {
        { "R1_plain", "parity.machine", true, "" },     { "R2_quotient", "mod3.machine", true, "" },   { "R3_overcomplete", "R2_quotient", true, "" },
        { "R4_two_bits", "parity.machine", true, "" },  { "R5_redundant", "R1_plain", true, "" },      { "parity_latch", "R1_plain", true, "" },
        { "counter_mod3", "mod3.machine", true, "" },   { "counter_mod3", "R2_quotient", true, "" },   { "R1_plain", "mod3.machine", false, "one one" },
        { "R2_quotient", "R1_plain", false, "one one" } };
    std::string bad;
    for (const auto& e : eqs) {
        const Source other = inline_spec(example(e.b)->text);
        const Answer p = verb_prove(inline_spec(example(e.a)->text), ProveOptions(), &other);
        const JV* q = dig(p.result, { "results", "equivalence" });
        std::string path;
        if (q && q->get("differs_after") && q->get("differs_after")->t == JV::Arr)
            for (const auto& x : q->get("differs_after")->arr) path += (path.empty() ? "" : " ") + x.s;
        const bool ok = q && (e.same ? p.status == "ok" && dig_true(*q, { "holds" }) && dig_str(*q, { "grade" }) == "proved"
                                     : p.status == "refuted" && path == e.differs);
        if (!ok) bad += std::string(" ") + e.a + " vs " + e.b;
    }
    t.check(bad.empty(), "equivalence: eight pairs proved to decide alike on every token sequence; two pairs refuted with the shortest sequence (one one)", bad);
}

// ---- 200 random machines ---------------------------------------------------------------------------------
void test_machines(Suite& t) {
    Rng rng(20261129);
    size_t steps = 0, wrong = 0, unproved = 0, rotated = 0, neurons_min = ~(size_t)0, neurons_max = 0;
    std::string first_bad;
    for (int i = 0; i < 200; ++i) {
        const int S = 2 + (int)rng.below(7), T = 2 + (int)rng.below(5);
        std::vector<int> delta((size_t)S * (size_t)T);
        for (auto& v : delta) v = (int)rng.below((uint32_t)S);
        std::vector<char> emit((size_t)S);
        for (auto& v : emit) v = rng.below(2) != 0;
        std::string spec = "net m" + std::to_string(i) + "\ntokens";
        for (int e = 0; e < T; ++e) spec += " t" + std::to_string(e);
        spec += "\nmachine S states=" + std::to_string(S) + " init=0\n";
        for (int e = 0; e < T; ++e) {
            spec += "  on t" + std::to_string(e) + ":";
            for (int s = 0; s < S; ++s) spec += " " + std::to_string(s) + "->" + std::to_string(delta[(size_t)s * (size_t)T + (size_t)e]);
            spec += "\n";
        }
        std::string listed;
        for (int s = 0; s < S; ++s)
            if (emit[(size_t)s]) listed += " S." + std::to_string(s);
        spec += listed.empty() ? "readout -4\n" : "emit" + listed + "\n";
        try {
            const bool rot = i % 4 == 0;
            rotated += rot;
            const Model m = model_from_text(spec, rot ? 2 : 1, 2000 + (unsigned long long)i);
            neurons_min = std::min(neurons_min, (size_t)m.net.n);
            neurons_max = std::max(neurons_max, (size_t)m.net.n);
            const Kernel K(m.net);
            std::vector<double> state((size_t)K.padded());
            K.load(m.net.r0.data(), state.data());
            const int f0 = m.field("S.0");
            int cur = 0;
            size_t bad_here = 0;
            for (int k = 0; k < 1500; ++k, ++steps) {
                const int tok = (int)rng.below((uint32_t)T);
                cur = delta[(size_t)cur * (size_t)T + (size_t)tok];
                K.step(state.data(), tok);
                bool ok = (K.margin(state.data()) > 0) == (emit[(size_t)cur] != 0);
                for (int s = 0; s < S && ok; ++s) {
                    const double v = m.value(state.data(), f0 + s), want = s == cur ? 1.0 : 0.0;
                    ok = rot ? std::fabs(v - want) <= kTol : v == want;
                }
                bad_here += !ok;
            }
            wrong += bad_here;
            // and for every token sequence at once: the network against its own table
            Moore table;
            table.label = "its table";
            table.tokens = m.net.tokens;
            table.states = S;
            table.next = delta;
            table.emit = emit;
            const Model logic = rot ? model_from_text(spec, 1, 0) : m;
            const Proof p = prove(logic, nullptr, ProveOptions(), &table, nullptr);
            const bool proved = p.status == "ok" && p.grade == "proved" && dig_true(p.result, { "results", "equivalence", "holds" });
            unproved += !proved;
            if ((bad_here || !proved) && first_bad.empty()) first_bad = "machine " + std::to_string(i) + ": " + p.text.substr(0, p.text.find('\n'));
        } catch (const Fail& f) {
            ++wrong;
            if (first_bad.empty()) first_bad = "machine " + std::to_string(i) + ": " + f.first().message;
        }
    }
    t.check(wrong == 0, ssprintf("200 random machines (2 to 8 states, 2 to 6 tokens) built from machine statements: state and decision equal a plain "
                                 "simulation on 1,500 random tokens each (%s steps; %zu of the machines rotated)", commas(steps).c_str(), rotated),
            wrong ? ssprintf("%zu steps differ; ", wrong) + first_bad : ssprintf("%zu to %zu neurons", neurons_min, neurons_max));
    t.check(unproved == 0, "the same 200 machines: each proved exact and proved equivalent to its transition table on every token sequence",
            unproved ? ssprintf("%zu not proved; ", unproved) + first_bad : std::string());
}

// ---- planted faults, the counter, the cap ------------------------------------------------------------------
void test_proofs(Suite& t) {
    struct Fault { const char* name; std::vector<const char*> must; const char* failing; };
    const Fault faults[] = {
        { "fault_ramp", { "rule 1 (line 9: tau A <- in1 - A - 0.5)", "z = 0.5", "strictly inside (0, 1)", "state {all fields 0}", "token one" }, "exactness" },
        { "fault_two_setters", { "field A is 2", "token one", "state {A=1}", "reached by: one", "1 from rule 1 (line 9", "1 from rule 2 (line 10" }, "cleanliness" },
        { "fault_never_cleared", { "field N is 2", "token one", "state {N=1}", "1 kept from before (nothing clears N)", "1 from rule 1 (line 9" }, "cleanliness" },
    };
    for (const auto& f : faults) {
        const Answer p = verb_prove(inline_spec(example(f.name)->text), ProveOptions(), nullptr);
        bool ok = p.status == "refuted" && p.exit_code() == 1 && p.grade == "proved";
        const JV* claim = dig(p.result, { "results", f.failing });
        ok = ok && claim && claim->get("holds") && claim->get("holds")->t == JV::Bool && !claim->get("holds")->b;
        std::string msg;
        if (ok) {
            const JV& v = claim->get("violations")->arr[0];
            msg = dig_str(v, { "message" });
            for (const char* part : f.must) ok = ok && contains(msg, part);
            ok = ok && dig_str(v, { "token" }) == "one" && v.get("state") && v.get("path");
        }
        t.check(ok, std::string(f.name) + ": refused, naming the state, the token and the rule", ok ? msg : p.text.substr(0, 300));
    }
    {
        const Answer p = verb_prove(inline_spec(example("counter_mod3")->text), ProveOptions(), nullptr);
        const JV* mc = dig(p.result, { "machines" });
        const bool ok = p.status == "ok" && p.grade == "proved" && dig_int(p.result, { "results", "reachable", "states" }) == 3 &&
                        dig_int(p.result, { "results", "reachable", "transitions" }) == 12 && mc && mc->arr.size() == 1 &&
                        mc->arr[0].get("reachable")->arr.size() == 3 && dig(p.result, { "state_table" })->arr.size() == 3;
        t.check(ok, "a counter modulo 3 with reset: 3 reachable count states, grade proved", p.text.substr(0, p.text.find('\n')));
    }
    {
        const Answer p = verb_prove(inline_spec(example("R1_plain")->text), ProveOptions(), nullptr);
        const double linf = dig_num(p.result, { "results", "noise", "linf" }), l2 = dig_num(p.result, { "results", "noise", "l2" });
        t.check(std::fabs(linf - 0.5 / 6.0) < 1e-15 && std::fabs(l2 - 0.5 / std::sqrt(12.0)) < 1e-15 && dig_num(p.result, { "results", "noise", "single" }) == 0.25 &&
                    dig_int(p.result, { "results", "noise", "rule" }) == 2 && dig_num(p.result, { "results", "noise", "decision_linf" }) == 4.0 / 12.0,
                "noise tolerance of R1_plain: 0.5/6 on every coordinate (its second rule), 0.5/sqrt(12) in Euclidean norm, 0.25 on one alone");
    }
    {
        ProveOptions small;
        small.cap = 2;
        const Answer p = verb_prove(inline_spec(example("counter_mod3")->text), small, nullptr);
        t.check(p.status == "undecided" && p.grade == "undecided" && p.exit_code() == 3 && dig_str(p.result, { "evidence", "sampled", "grade" }) == "sampled" &&
                    dig_int(p.result, { "evidence", "sampled", "steps" }) == 64 * 4096 && dig_int(p.result, { "evidence", "sampled", "violations" }) == 0 &&
                    dig_int(p.result, { "results", "reachable", "exhaustive_to_depth" }) == 1,
                "at a cap of 2 states the counter is undecided, with what was covered and 262,144 sampled steps as evidence");
    }
    {
        const Answer p = verb_prove(inline_spec("net edge\ntokens a b\nfields A\ntau A <- in.a\nclear A\nreadout 8*A - 4\n"), ProveOptions(), nullptr);
        const JV* w = dig(p.result, { "warnings" });
        bool warned = false;
        if (w)
            for (const auto& x : w->arr) warned = warned || dig_str(x, { "code" }) == "W_EDGE";
        t.check(p.status == "ok" && warned && dig_num(p.result, { "results", "exactness", "min_distance" }) == 0 && dig_num(p.result, { "results", "noise", "linf" }) == 0,
                "a threshold that lands exactly on 0 and on 1 is exact, with distance 0, noise tolerance 0 and a warning");
    }
    {
        // 2.2 and 0.7 are not exact in binary: the sums round, the states close only to 2e-16, and quill says so
        const Answer p = verb_prove(inline_spec("net rounded\ntokens a b\nfields A\ntau A <- 2.2*in.a - 2.2*A - 0.7\n"
                                                "tau A <- 2.2*A - 2.2*in.a - 0.7\nclear A\nreadout 8*A - 4\n"),
                                    ProveOptions(), nullptr);
        const double gap = dig_num(p.result, { "results", "reachable", "closure_residual" });
        t.check(p.status == "ok" && p.grade == "checked" && dig_str(p.result, { "arithmetic" }) == "rounded" && gap > 0 && gap < 1e-12 &&
                    dig_int(p.result, { "results", "reachable", "states" }) == 2,
                "weights that are not exact in binary: the same network is graded checked, not proved, and reported as rounded");
    }
    {
        const Answer a = verb_prove(inline_spec(example("R4_two_bits")->text), ProveOptions(), nullptr), b = verb_prove(inline_spec(example("R4_two_bits")->text), ProveOptions(), nullptr);
        const JV ra = receipt_json(a), rb = receipt_json(b);
        JV parsed;
        std::string err;
        const bool ok = json_parse(answer_json(a), parsed, err) && dig_str(parsed, { "receipt", "id" }) == ra.get("id")->s && ra.get("id")->s == rb.get("id")->s &&
                        ra.get("id")->s.size() == 7 + 12 && ra.get("id")->s.compare(0, 7, "quill1:") == 0 &&
                        ra.get("result_sha256")->s == sha256_hex(json_canonical(*parsed.get("result"))) && dig_str(parsed, { "status" }) == "ok" &&
                        dig_str(parsed, { "quill" }) == kVersion && dig_str(parsed, { "verb" }) == "prove";
        t.check(ok, "an answer is one JSON object; its receipt is the same on a second run and re-derives from the printed result", err);
    }
}

// ---- run, explain, the 8-bit copy, the MCP server ---------------------------------------------------------
void test_surfaces(Suite& t) {
    {
        RunOptions o;
        o.have_tokens = true;
        o.tokens = "zero one, 1 two one";
        const Answer r = verb_run(inline_spec(example("R1_plain")->text), o);
        const bool ok = r.status == "ok" && dig_int(r.result, { "steps" }) == 5 && dig_int(r.result, { "emit" }) == 2 &&
                        dig_str(r.result, { "decisions_sha256" }) == sha256_hex("01001") && dig_num(r.result, { "final_state", "A" }) == 1 &&
                        dig(r.result, { "rows" })->arr.size() == 5 && dig_num(dig(r.result, { "rows" })->arr[1], { "margin" }) == 4;
        bool refused = false;
        try {
            o.tokens = "zero four";
            verb_run(inline_spec(example("R1_plain")->text), o);
        } catch (const Fail& f) {
            refused = contains(f.first().message, "'four' is not a token");
        }
        t.check(ok && refused, "run: names and positions mix (\"zero one, 1 two one\" gives hold emit hold hold emit); an unknown token is refused");
    }
    {
        const Model m = model_from_text(example("parity_latch")->text);
        const std::string md = explain_markdown(m);
        const JV j = explain_json(m);
        bool ok = contains(md, "## Fields") && contains(md, "## Neurons") && contains(md, "## Expansions") &&
                  contains(md, "fires (z >= 1): on one when A=0") && contains(md, "fires (z >= 1): on two, three when A=1") &&
                  dig(j, { "neurons" })->arr.size() == (size_t)m.net.n && dig(j, { "rules" })->arr.size() == m.rules.size();
        size_t lines = 0;
        for (size_t at = md.find("\n| "); at != std::string::npos; at = md.find("\n| ", at + 1)) ++lines;
        ok = ok && lines >= (size_t)m.net.n;
        const Firing f = firing(model_from_text(example("R5_redundant")->text), 0);
        ok = ok && f.known && f.when == "on one when s1=0, s2=0 | s1=0, s3=0 | s2=0, s3=0" && f.z == std::vector<double>{ 1.5, 3.5 };
        t.check(ok, "explain: a line per neuron, when each rule fires read off its weights, and what each shorthand statement expands to");
    }
    {
        std::vector<float> row(40);
        for (int k = 0; k < 40; ++k) row[(size_t)k] = (float)((k % 2 ? -1.0 : 1.0) * (k + 1) / 7.0);
        std::vector<double> out(40);
        q8_row(row.data(), 40, out.data());
        bool ok = true;
        for (int k = 0; k < 40; ++k) ok = ok && out[(size_t)k] == vectors::kQ8[k];
        const float ties[8] = { 127.0f, 0.5f, 1.5f, 2.5f, -0.5f, -1.5f, 3.5f, 0.0f };
        const double want[8] = { 127, 0, 2, 2, 0, -2, 4, 0 };
        double got[8];
        q8_row(ties, 8, got);
        for (int k = 0; k < 8; ++k) ok = ok && got[k] == want[k];
        const float zeros[4] = { 0, 0, 0, 0 };
        q8_row(zeros, 4, got);
        ok = ok && got[0] == 0 && got[3] == 0;
        t.check(ok, "the 8-bit copy: blocks of 32 scaled by their largest magnitude over 127, halves rounded to even, as the kit's q8");
    }
    {
        JV r;
        std::string err;
        const auto call = [&](const std::string& line) {
            const std::string out = mcp_handle(line);
            return !out.empty() && json_parse(out, r, err);
        };
        bool ok = call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}}}") &&
                  dig_str(r, { "result", "serverInfo", "name" }) == "quill" && dig_str(r, { "result", "protocolVersion" }) == "2025-06-18";
        ok = ok && mcp_handle("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}").empty();
        ok = ok && call("{\"jsonrpc\":\"2.0\",\"id\":\"two\",\"method\":\"tools/list\"}") && dig(r, { "result", "tools" })->arr.size() == 4 && dig_str(r, { "id" }) == "two";
        JV req = JV::object(), params = JV::object(), args = JV::object();
        args.set("spec", example("counter_mod3")->text).set("equiv", example("mod3.machine")->text);
        params.set("name", "quill_prove").set("arguments", args);
        req.set("jsonrpc", "2.0").set("id", 3).set("method", "tools/call").set("params", params);
        JV answer;
        ok = ok && call(json_compact(req)) && !dig_true(r, { "result", "isError" }) &&
             json_parse(dig(r, { "result", "content" })->arr[0].get("text")->s, answer, err) && dig_str(answer, { "status" }) == "ok" &&
             dig_str(answer, { "grade" }) == "proved" && dig_true(answer, { "result", "results", "equivalence", "holds" });
        args = JV::object();
        args.set("spec", "net x\ntokens a b\nfields A\ntau A <- 2*B\nreadout A\n");
        params.set("arguments", args);
        req.set("params", params);
        ok = ok && call(json_compact(req)) && dig_true(r, { "result", "isError" }) &&
             json_parse(dig(r, { "result", "content" })->arr[0].get("text")->s, answer, err) && dig_str(answer, { "error", "code" }) == "E_SPEC" &&
             dig_int(answer, { "error", "line" }) == 4 && dig_int(answer, { "error", "column" }) == 12;
        params.set("name", "quill_explain");
        args = JV::object();
        args.set("spec", example("parity")->text);
        params.set("arguments", args);
        req.set("params", params);
        ok = ok && call(json_compact(req)) && contains(dig(r, { "result", "content" })->arr[0].get("text")->s, "## Rules");
        ok = ok && call("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"nope\"}") && dig_int(r, { "error", "code" }) == -32601;
        ok = ok && call("not json") && dig_int(r, { "error", "code" }) == -32700;
        t.check(ok, "the MCP server: initialize, tools/list (four tools), a proof with an equivalence, a faulty specification with line and column, unknown method", err);
    }
    {
        Source s = inline_spec(example("parity")->text);
        const Answer p = verb_prove(s, ProveOptions(), nullptr);
        t.check(p.status == "ok" && p.grade == "proved" && contains(language_card(), "tau     A <- 2*in.one - 2*A - 0.5"),
                "the example printed by --help is a valid specification and proves");
    }
}

void test_disk(Suite& t) {
    const std::string dir = path_join(exe_dir(), "specs");
    if (!is_dir(dir)) {
        t.note("no specs/ folder beside quill.exe: the examples inside the executable were used (quill examples --write DIR writes them)");
        return;
    }
    std::string differ;
    size_t same = 0;
    for (const auto& ex : examples()) {
        std::string text, err;
        if (read_file(path_join(dir, ex.file), text, err) && text == ex.text) ++same;
        else differ += std::string(" ") + ex.file;
    }
    t.check(differ.empty(), ssprintf("specs/ beside quill.exe holds the %zu examples, byte for byte as inside the executable", same), differ);
}

void note_speed(Suite& t) {
    const Model m = model_from_text(example("R1_plain")->text, 2, 101);
    const Kernel K(m.net);
    std::vector<double> state((size_t)K.padded());
    K.load(m.net.r0.data(), state.data());
    Rng rng(1);
    std::vector<int> toks(65536);
    for (auto& tk : toks) tk = (int)rng.below(4);
    const size_t steps = 2000000;
    double acc = 0;
    const double t0 = now_ms();
    for (size_t k = 0; k < steps; ++k) {
        K.step(state.data(), toks[k & 65535]);
        acc += K.margin(state.data());
    }
    const double seconds = (now_ms() - t0) / 1000.0;
    t.note(ssprintf("speed, not a check: %s steps per second on R1_plain rotated (dense, width 32), one thread%s",
                    commas((unsigned long long)((double)steps / std::max(seconds, 1e-9))).c_str(), acc == acc ? "" : " (nan)"));
}

}  // namespace

Answer verb_selftest() {
    Answer a;
    a.verb = "selftest";
    Suite t;
    const double t0 = now_ms();
    const auto section = [&](void (*fn)(Suite&), const char* name) {
        try {
            fn(t);
        } catch (const Fail& f) {
            t.check(false, std::string(name) + ": stopped by an error", f.first().code + ": " + f.first().message);
        } catch (const std::exception& e) {
            t.check(false, std::string(name) + ": stopped by an exception", e.what());
        }
    };
    section(test_base, "base");
    section(test_npz, "npz");
    section(test_language, "language");
    section(test_kernel, "kernel");
    section(test_kit_specs, "the kit's articles");
    section(test_machines, "random machines");
    section(test_proofs, "proofs");
    section(test_surfaces, "surfaces");
    section(test_disk, "specs on disk");
    section(note_speed, "speed");
    a.status = t.failed ? "refuted" : "ok";
    a.grade = "checked";
    a.result.set("passed", t.passed).set("failed", t.failed).set("checks", t.checks);
    a.meta.set("ms", now_ms() - t0).set("notes", t.notes);
    a.input.set("verb", "selftest").set("version", kVersion);
    a.text = std::string("quill ") + kVersion + " --selftest\n" + t.text +
             (t.failed ? ssprintf("SELFTEST: %d FAILURE(S), %d passed\n", t.failed, t.passed) : ssprintf("SELFTEST: ALL PASS (%d checks)\n", t.passed));
    return a;
}

}  // namespace quill
