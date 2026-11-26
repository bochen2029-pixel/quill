// quill · explain.cpp — what a network means: its fields, tokens, rules, neurons and readout,
// as Markdown and as JSON; and the two JSON files that sit beside weights.npz.
#include "spec.h"

#include <cmath>
#include <set>

namespace quill {

double snap9(double v) {
    if (!(std::fabs(v) < 1e6)) return v;
    const double s = std::round(v * 1e9) / 1e9;
    return s == 0 ? 0.0 : s;
}

std::string state_text(const Model& m, const double* state) {
    if (m.F() == 0) return "{raw coordinates}";
    std::string out;
    for (int f = 0; f < m.F(); ++f) {
        const double v = snap9(m.value(state, f));
        if (f == m.one ? v == 1 : v == 0) continue;
        out += (out.empty() ? "" : ", ") + m.fields[(size_t)f] + "=" + num(v);
    }
    return out.empty() ? "{all fields 0}" : "{" + out + "}";
}

JV state_json(const Model& m, const double* state) {
    JV o = JV::object();
    for (int f = 0; f < m.F(); ++f) {
        const double v = snap9(m.value(state, f));
        if (f == m.one && v == 1) continue;
        o.set(m.fields[(size_t)f], v);
    }
    return o;
}

JV sizes_json(const Network& net) {
    const size_t d = (size_t)net.d, n = (size_t)net.n;
    const auto nnz = [](const std::vector<double>& a) {
        size_t c = 0;
        for (double v : a)
            if (v != 0) ++c;
        return c;
    };
    size_t widest = 0;
    for (size_t t = 0; t < (size_t)net.T(); ++t) {
        size_t c = 0;
        for (size_t j = 0; j < d; ++j)
            if (net.E[t * d + j] != 0) ++c;
        widest = std::max(widest, c);
    }
    JV o = JV::object();
    o.set("width", net.d).set("neurons", net.n).set("tokens", net.T());
    o.set("weights", (unsigned long long)net.weights()).set("nonzero_weights", (unsigned long long)net.nonzero());
    o.set("ops_per_step", (unsigned long long)(widest + nnz(net.W1) + nnz(net.W2) + n));
    o.set("ops_per_step_dense", (unsigned long long)(d + 2 * n * d + n));
    return o;
}

// ======================================================================
// reading a rule off its weights
// ======================================================================
namespace {

struct Statics {
    std::vector<char> embedded, written, pure;
    std::vector<int> clear_rule;           // per field: the rule that clears it, or -1
    explicit Statics(const Model& m) {
        const size_t F = (size_t)m.F(), T = (size_t)m.net.T();
        embedded.assign(F, 0);
        written.assign(F, 0);
        pure.assign(F, 0);
        clear_rule.assign(F, -1);
        for (size_t t = 0; t < T; ++t)
            for (size_t f = 0; f < F; ++f)
                if (std::fabs(m.em[t * F + f]) > kTol) embedded[f] = 1;
        for (size_t k = 0; k < m.rules.size(); ++k) {
            const Rule& r = m.rules[k];
            if (r.kind == Rule::Clear) {
                if (r.targets.size() == 1) clear_rule[(size_t)r.targets[0]] = (int)k;
                continue;
            }
            for (int i : { r.n0, r.n1 })
                if (i >= 0)
                    for (size_t f = 0; f < F; ++f)
                        if (std::fabs(m.wr[(size_t)i * F + f]) > kTol) written[f] = 1;
        }
        // A pure input: only tokens write it and it is cleared, so during a step it is E[token] exactly.
        for (size_t f = 0; f < F; ++f)
            pure[f] = embedded[f] && !written[f] && clear_rule[f] >= 0 && std::fabs(m.r0f[f]) <= kTol;
    }
};

// A cover of the given minterms over k variables by cubes: (which bits are fixed, their values).
std::vector<std::pair<unsigned, unsigned>> cover(const std::vector<unsigned>& on, int k) {
    typedef std::pair<unsigned, unsigned> Cube;
    const unsigned full = (1u << k) - 1u;
    std::set<Cube> cur;
    for (unsigned mt : on) cur.insert({ full, mt });
    std::vector<Cube> primes;
    while (!cur.empty()) {
        const std::vector<Cube> v(cur.begin(), cur.end());
        std::set<Cube> next, used;
        for (size_t a = 0; a < v.size(); ++a)
            for (size_t b = a + 1; b < v.size(); ++b) {
                if (v[a].first != v[b].first) continue;
                const unsigned diff = v[a].second ^ v[b].second;
                if (diff == 0 || (diff & (diff - 1)) != 0) continue;
                next.insert({ v[a].first & ~diff, v[a].second & ~diff });
                used.insert(v[a]);
                used.insert(v[b]);
            }
        for (const auto& c : v)
            if (!used.count(c)) primes.push_back(c);
        cur.swap(next);
    }
    const auto bits = [](unsigned x) {
        int c = 0;
        for (; x; x &= x - 1) ++c;
        return c;
    };
    std::set<unsigned> left(on.begin(), on.end());
    std::vector<Cube> out;
    while (!left.empty()) {
        int best = -1, best_count = -1;
        for (size_t i = 0; i < primes.size(); ++i) {
            int c = 0;
            for (unsigned mt : left)
                if ((mt & primes[i].first) == primes[i].second) ++c;
            const bool better = c > best_count ||
                                (c == best_count && best >= 0 && (bits(primes[i].first) < bits(primes[(size_t)best].first) ||
                                                                   (bits(primes[i].first) == bits(primes[(size_t)best].first) && primes[i] < primes[(size_t)best])));
            if (better) { best = (int)i; best_count = c; }
        }
        out.push_back(primes[(size_t)best]);
        for (auto it = left.begin(); it != left.end();)
            if ((*it & primes[(size_t)best].first) == primes[(size_t)best].second) it = left.erase(it);
            else ++it;
    }
    std::sort(out.begin(), out.end());
    return out;
}

Firing firing_with(const Model& m, const Statics& st, int rule) {
    Firing out;
    const Rule& r = m.rules[(size_t)rule];
    if (r.kind == Rule::Clear || m.F() == 0) return out;
    const size_t F = (size_t)m.F(), T = (size_t)m.net.T(), i = (size_t)r.n0;
    if (m.rd_out[i] > 0) return out;                 // it reads directions that have no name
    const bool tau = r.kind == Rule::Tau;
    std::vector<int> free;
    for (size_t f = 0; f < F; ++f)
        if (std::fabs(m.rd[i * F + f]) > kTol && !st.pure[f] && (int)f != m.one) free.push_back((int)f);
    if (free.size() > 10) return out;
    out.known = true;
    const int k = (int)free.size();
    std::vector<std::string> fire_desc(T), ramp_desc(T);
    std::set<double> zs;
    const auto describe = [&](const std::vector<unsigned>& on) -> std::string {
        if (on.empty()) return std::string();
        if (on.size() == ((size_t)1 << k)) return "always";
        std::string text;
        for (const auto& c : cover(on, k)) {
            std::string cube;
            for (int b = 0; b < k; ++b)
                if (c.first & (1u << b)) cube += (cube.empty() ? "" : ", ") + m.fields[(size_t)free[(size_t)b]] + "=" + ((c.second >> b) & 1u ? "1" : "0");
            text += (text.empty() ? "" : " | ") + cube;
        }
        return text;
    };
    for (size_t t = 0; t < T; ++t) {
        double base = m.net.b1[i];
        for (size_t f = 0; f < F; ++f) {
            if ((int)f == m.one) base += m.rd[i * F + f];
            else if (st.pure[f]) base += m.rd[i * F + f] * m.em[t * F + f];
        }
        std::vector<unsigned> fire, ramp;
        for (unsigned a = 0; a < (1u << k); ++a) {
            double z = base;
            for (int b = 0; b < k; ++b)
                if (a & (1u << b)) z += m.rd[i * F + (size_t)free[(size_t)b]];
            if (tau ? z >= 1.0 - kTol : z > kTol) { fire.push_back(a); zs.insert(snap9(z)); }
            else if (tau && z > kTol) ramp.push_back(a);
        }
        fire_desc[t] = describe(fire);
        ramp_desc[t] = describe(ramp);
    }
    const auto compose = [&](const std::vector<std::string>& desc) -> std::string {
        std::vector<std::pair<std::string, std::vector<size_t>>> groups;
        for (size_t t = 0; t < T; ++t) {
            if (desc[t].empty()) continue;
            bool found = false;
            for (auto& g : groups)
                if (g.first == desc[t]) { g.second.push_back(t); found = true; }
            if (!found) groups.push_back({ desc[t], { t } });
        }
        if (groups.empty()) return std::string();
        if (groups.size() == 1 && groups[0].second.size() == T)
            return groups[0].first == "always" ? std::string("on every token") : "when " + groups[0].first + " (any token)";
        std::string text;
        for (const auto& g : groups) {
            std::string toks;
            for (size_t x = 0; x < g.second.size(); ++x) {
                if (x == 6 && g.second.size() > 7) { toks += ssprintf(", ... (%zu tokens)", g.second.size()); break; }
                toks += (x ? ", " : "") + m.net.tokens[g.second[x]];
            }
            text += (text.empty() ? "" : "; ") + ("on " + toks) + (g.first == "always" ? "" : " when " + g.first);
        }
        return text;
    };
    out.when = compose(fire_desc);
    if (out.when.empty()) out.when = "never, while the fields are 0 or 1";
    out.ramp = compose(ramp_desc);
    out.z.assign(zs.begin(), zs.end());
    return out;
}

std::string capped(const std::vector<std::string>& v, size_t most) {
    if (v.empty()) return "-";
    std::string out;
    for (size_t i = 0; i < v.size() && i < most; ++i) out += (i ? ", " : "") + v[i];
    if (v.size() > most) out += ssprintf(", ... (%zu)", v.size());
    return out;
}

Lin readout_lin(const Model& m) {                         // the margin over the fields, the constant `one` last
    Lin l;
    for (int pass = 0; pass < 2; ++pass)
        for (int f = 0; f < m.F(); ++f)
            if ((f == m.one) == (pass == 1) && std::fabs(m.ro[(size_t)f]) > kTol) l.terms.push_back({ f, snap9(m.ro[(size_t)f]) });
    return l;
}

std::string md_cell(std::string s) {                      // keep a table cell on one row
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] == '|') { s.insert(i, "\\"); ++i; }
        else if (s[i] == '\n') s[i] = ' ';
    return s;
}

std::string one_line(const std::string& s) {              // a statement of several lines, on one
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\n') { out += s[i]; continue; }
        out += ";";
        while (i + 1 < s.size() && s[i + 1] == ' ') ++i;
        out += ' ';
    }
    return out;
}

const char* kind_name(Rule::Kind k) { return k == Rule::Tau ? "tau" : k == Rule::Clear ? "clear" : "neuron"; }

}  // namespace

Firing firing(const Model& m, int rule) {
    const Statics st(m);
    return firing_with(m, st, rule);
}

// ======================================================================
// the two JSON files beside weights.npz (the kit's keys, plus a "quill" block)
// ======================================================================
JV network_json(const Model& m, const std::string& weights_sha256) {
    JV o = JV::object();
    o.set("name", m.net.name).set("family", "R").set("kit_version", "0.2").set("n_embd", m.net.d).set("n_vocab", 2);
    o.set("n_input_vocab", m.net.T()).set("tokens", JV::strings(m.net.tokens));
    o.set("readout_bias", "none: the constant is carried by the state's 'one' coordinate");
    o.set("n_neurons", m.net.n).set("n_heads", 0).set("head_scales", JV::array());
    o.set("n_weights", (unsigned long long)m.net.weights());
    o.set("readout_rows", JV::strings({ "hold", "emit" })).set("emit_tok", 1).set("hold_tok", 0).set("context", 1);
    o.set("weights_sha256", weights_sha256);
    JV q = JV::object();
    q.set("version", kVersion).set("basis", m.rotated ? "rotated" : "axis");
    q.set("rotate_seed", m.rotated && m.seed_known ? JV(m.seed) : JV());
    if (const JV* h = m.input.get("sha256")) q.set("spec_sha256", *h);
    o.set("quill", q);
    return o;
}

JV manifest_json(const Model& m, const std::string& weights_sha256) {
    const size_t d = (size_t)m.net.d;
    JV o = JV::object();
    o.set("name", m.net.name).set("family", "R").set("kit_version", "0.2").set("storage_form", "written by quill from a specification");
    o.set("note", "Every weight follows by formula from the specification kept below under quill.spec. "
                  "EXPLAIN.md beside this file says what each field, rule and neuron means.");
    o.set("n_embd", m.net.d).set("n_weights", (unsigned long long)m.net.weights()).set("tokens", JV::strings(m.net.tokens));
    o.set("margin", "emit minus hold = " + m.lin_text(readout_lin(m)) + "; rows of Wout are [hold, emit]; no bias");
    const auto direction = [&](size_t f) {
        JV a = JV::array();
        for (size_t j = 0; j < d; ++j) a.push(m.rotated ? m.dirs[f * d + j] : ((int)j == m.coord[f] ? 1.0 : 0.0));
        return a;
    };
    JV fields = JV::object(), shams = JV::object();
    for (size_t f = 0; f < m.fields.size(); ++f) fields.set(m.fields[f], direction(f));
    for (size_t k = 0; k < m.sham_names.size(); ++k) {
        JV a = JV::array();
        for (size_t j = 0; j < d; ++j) a.push(m.sham_dirs[k * d + j]);
        shams.set(m.sham_names[k], a);
    }
    o.set("fields", fields).set("true_shams", shams);
    o.set("true_sham_note", "Directions no neuron and no readout row reads or writes. An edit along one has exactly no effect.");
    JV q = JV::object();
    q.set("version", kVersion).set("basis", m.rotated ? "rotated" : "axis");
    q.set("rotate_seed", m.rotated && m.seed_known ? JV(m.seed) : JV());
    if (const JV* h = m.input.get("sha256")) q.set("spec_sha256", *h);
    q.set("weights_sha256", weights_sha256).set("spec", m.spec_text);
    o.set("quill", q);
    return o;
}

// ======================================================================
// the explanation
// ======================================================================
namespace {

struct Explained {
    Statics st;
    std::vector<Firing> fire;                                // per rule
    std::vector<std::vector<std::string>> written_by, read_by, set_by;   // per field
    explicit Explained(const Model& m) : st(m) {
        const size_t F = (size_t)m.F(), T = (size_t)m.net.T();
        written_by.resize(F);
        read_by.resize(F);
        set_by.resize(F);
        for (size_t k = 0; k < m.rules.size(); ++k) {
            fire.push_back(firing_with(m, st, (int)k));
            const Rule& r = m.rules[k];
            if (r.kind == Rule::Clear) continue;
            const size_t i = (size_t)r.n0;
            for (size_t f = 0; f < F; ++f) {
                if (std::fabs(m.rd[i * F + f]) > kTol) read_by[f].push_back("rule " + std::to_string(k + 1));
                if (std::fabs(m.wr[i * F + f]) > kTol) written_by[f].push_back("rule " + std::to_string(k + 1));
            }
        }
        for (size_t f = 0; f < F; ++f) {
            if (std::fabs(m.ro[f]) > kTol) read_by[f].push_back("readout");
            for (size_t t = 0; t < T; ++t)
                if (std::fabs(m.em[t * F + f]) > kTol) set_by[f].push_back(m.net.tokens[t]);
        }
    }
    const char* role(const Model& m, size_t f) const {
        if ((int)f == m.one) return "the constant 1";
        if (st.pure[f]) return "input";
        if (written_by[f].empty() && read_by[f].empty() && set_by[f].empty()) return "unused";
        return "state";
    }
};

std::string neuron_reads(const Model& m, size_t i) {
    if (m.F() == 0) return ssprintf("row %zu of W1", i);
    Lin l;
    const size_t F = (size_t)m.F();
    for (size_t f = 0; f < F; ++f)
        if (std::fabs(m.rd[i * F + f]) > kTol) l.terms.push_back({ (int)f, snap9(m.rd[i * F + f]) });
    l.constant = snap9(m.net.b1[i]);
    return m.lin_text(l) + (m.rd_out[i] > 0 ? " + (unnamed directions)" : "");
}
std::string neuron_writes(const Model& m, size_t i) {
    if (m.F() == 0) return ssprintf("column %zu of W2", i);
    Lin l;
    const size_t F = (size_t)m.F();
    for (size_t f = 0; f < F; ++f)
        if (std::fabs(m.wr[i * F + f]) > kTol) l.terms.push_back({ (int)f, snap9(m.wr[i * F + f]) });
    if (l.terms.empty()) return m.wr_out[i] > 0 ? "(unnamed directions)" : "nothing";
    return m.lin_text(l) + (m.wr_out[i] > 0 ? " + (unnamed directions)" : "");
}

std::string z_list(const std::vector<double>& z) {
    if (z.empty()) return std::string();
    if (z.size() == 1) return "z = " + num(z[0]);
    std::string out = "z in {";
    for (size_t i = 0; i < z.size() && i < 6; ++i) out += (i ? ", " : "") + num(z[i]);
    return out + (z.size() > 6 ? ", ...}" : "}");
}

}  // namespace

JV explain_json(const Model& m) {
    const Explained ex(m);
    const size_t F = (size_t)m.F(), T = (size_t)m.net.T();
    JV o = JV::object();
    o.set("name", m.net.name).set("origin", m.origin).set("basis", m.rotated ? "rotated" : "axis");
    o.set("rotate_seed", m.rotated && m.seed_known ? JV(m.seed) : JV());
    o.set("input", m.input).set("sizes", sizes_json(m.net));
    JV fields = JV::array();
    for (size_t f = 0; f < F; ++f) {
        JV x = JV::object();
        x.set("index", (int)f).set("name", m.fields[f]).set("role", ex.role(m, f)).set("start", snap9(m.r0f[f]));
        x.set("coordinate", m.rotated ? JV() : JV(m.coord[f]));
        x.set("tokens", JV::strings(ex.set_by[f])).set("written_by", JV::strings(ex.written_by[f]));
        x.set("cleared_by", ex.st.clear_rule[f] >= 0 ? JV(ex.st.clear_rule[f] + 1) : JV());
        x.set("read_by", JV::strings(ex.read_by[f]));
        fields.push(x);
    }
    o.set("fields", fields);
    JV tokens = JV::array();
    for (size_t t = 0; t < T; ++t) {
        JV adds = JV::object();
        for (size_t f = 0; f < F; ++f)
            if (std::fabs(m.em[t * F + f]) > kTol) adds.set(m.fields[f], snap9(m.em[t * F + f]));
        JV x = JV::object();
        x.set("index", (int)t).set("name", m.net.tokens[t]).set("adds", adds);
        tokens.push(x);
    }
    o.set("tokens", tokens);
    JV rules = JV::array();
    for (size_t k = 0; k < m.rules.size(); ++k) {
        const Rule& r = m.rules[k];
        JV x = JV::object();
        x.set("rule", (int)k + 1).set("kind", kind_name(r.kind)).set("line", r.line ? JV(r.line) : JV()).set("text", r.text);
        x.set("meaning", r.note.empty() ? JV() : JV(r.note)).set("from", r.from.empty() ? JV() : JV(r.from));
        JV ns = JV::array();
        ns.push(r.n0);
        if (r.n1 >= 0) ns.push(r.n1);
        x.set("neurons", ns);
        JV tg = JV::array();
        for (int f : r.targets) tg.push(m.fields[(size_t)f]);
        x.set("targets", tg);
        const Firing& fi = ex.fire[k];
        x.set("fires", r.kind == Rule::Clear ? JV("on every step: it removes the field's old value") : fi.known ? JV(fi.when) : JV());
        x.set("z_when_firing", JV::numbers(fi.z)).set("possible_ramp", fi.ramp.empty() ? JV() : JV(fi.ramp));
        rules.push(x);
    }
    o.set("rules", rules);
    JV neurons = JV::array();
    for (size_t i = 0; i < (size_t)m.net.n; ++i) {
        const int k = m.neuron_rule[i];
        const Rule& r = m.rules[(size_t)k];
        const char* half = r.kind == Rule::Neuron ? "relu" : r.kind == Rule::Tau ? ((int)i == r.n0 ? "tau+" : "tau-") : ((int)i == r.n0 ? "clear+" : "clear-");
        JV x = JV::object();
        x.set("neuron", (int)i).set("rule", k + 1).set("role", half).set("reads", neuron_reads(m, i)).set("writes", neuron_writes(m, i));
        neurons.push(x);
    }
    o.set("neurons", neurons);
    JV ro = JV::object(), coefs = JV::object();
    const Lin rl = readout_lin(m);
    for (const auto& x : rl.terms) coefs.set(m.fields[(size_t)x.field], x.coef);
    ro.set("margin", F ? m.lin_text(rl) : std::string("(Wout[emit] - Wout[hold]) . r")).set("coefficients", coefs);
    ro.set("decision", "emit when the margin is above 0, hold otherwise");
    o.set("readout", ro);
    JV notes = JV::array(), warns = JV::array();
    for (const auto& s : m.notes) notes.push(s);
    for (const auto& w : m.warnings) {
        JV x = JV::object();
        x.set("code", w.code).set("line", w.line ? JV(w.line) : JV()).set("message", w.message).set("hint", w.hint.empty() ? JV() : JV(w.hint));
        warns.push(x);
    }
    o.set("notes", notes).set("warnings", warns);
    if (!m.spec_text.empty()) {
        Spec s = parse_spec(m.spec_text);
        s.rotate = m.rotated;                                // as built: a --rotate or --no-rotate override decides, not the text
        s.seed = m.seed;
        JV sugar = JV::array();
        for (size_t g = 0; g < s.sugar.size(); ++g) {
            JV lines = JV::array();
            for (const auto& op : s.ops)
                if (op.sugar == (int)g) lines.push(op_text(s, op));
            JV x = JV::object();
            x.set("line", s.sugar[g].line).set("statement", s.sugar[g].text).set("expands_to", lines);
            sugar.push(x);
        }
        o.set("expansions", sugar).set("expanded", expanded_text(s));
    }
    return o;
}

std::string explain_markdown(const Model& m) {
    const Explained ex(m);
    const size_t F = (size_t)m.F(), T = (size_t)m.net.T(), d = (size_t)m.net.d;
    std::string o = "# " + m.net.name + "\n\n";
    o += std::string("Explained by quill ") + kVersion + " from a " + m.origin;
    if (const JV* h = m.input.get("sha256")) o += " (sha256 " + h->s + ")";
    else if (const JV* w = m.input.get("weights_sha256")) o += " (weights.npz sha256 " + w->s + ")";
    o += ".\n\n";
    o += ssprintf("Width %d, %d neurons in %zu rules, %zu tokens, %zu named fields, %s weights (%s not zero). ", m.net.d, m.net.n,
                  m.rules.size(), T, F, commas(m.net.weights()).c_str(), commas(m.net.nonzero()).c_str());
    o += m.rotated ? (m.seed_known ? "Rotated basis, seed " + std::to_string(m.seed) + ": each field lies along a direction, not a coordinate.\n"
                                   : std::string("Rotated basis: each field lies along a direction, not a coordinate.\n"))
                   : std::string("Axis-aligned: each field is one coordinate.\n");
    for (const auto& s : m.notes) o += "Note: " + s + ".\n";
    for (const auto& w : m.warnings) o += "Warning " + w.code + (w.line ? ssprintf(" (line %d)", w.line) : std::string()) + ": " + w.message + ".\n";
    o += "\nOne step per token: `x = r + E[token]`, then `r' = x + W2 relu(W1 x + b1)`. Every rule reads `x`, the state "
         "with the token added and before any rule has written. So one rule cannot read what another writes in the same step.\n";

    o += "\n## Fields\n\n";
    if (!F) o += "No field names are known for this network.\n";
    else {
        o += "| # | field | role | start | tokens that add to it | written by | cleared by | read by |\n|---|---|---|---|---|---|---|---|\n";
        for (size_t f = 0; f < F; ++f)
            o += "| " + (m.rotated ? std::string("-") : std::to_string(m.coord[f])) + " | " + md_cell(m.fields[f]) + " | " + ex.role(m, f) + " | " +
                 show(m.r0f[f]) + " | " + capped(ex.set_by[f], 8) + " | " + capped(ex.written_by[f], 8) + " | " +
                 (ex.st.clear_rule[f] >= 0 ? "rule " + std::to_string(ex.st.clear_rule[f] + 1) : std::string("-")) + " | " + capped(ex.read_by[f], 8) + " |\n";
        if (F < d) o += ssprintf("\nThe other %zu %s spare: nothing reads or writes them.\n", d - F, m.rotated ? "directions are" : "coordinates are");
    }

    o += "\n## Tokens\n\n| # | token | adds |\n|---|---|---|\n";
    for (size_t t = 0; t < T; ++t) {
        std::string adds;
        for (size_t f = 0; f < F; ++f)
            if (std::fabs(m.em[t * F + f]) > kTol) adds += (adds.empty() ? "" : ", ") + show(m.em[t * F + f]) + " to " + md_cell(m.fields[f]);
        o += "| " + std::to_string(t) + " | " + md_cell(m.net.tokens[t]) + " | " + (adds.empty() ? (F ? "nothing: the network cannot see it" : "row of E") : adds) + " |\n";
    }

    o += "\n## Rules\n\nA tau adds `tau(z) = relu(z) - relu(z - 1)` to its targets: 0 when z <= 0, 1 when z >= 1. It is exact only while z "
         "never lands strictly between 0 and 1; `quill prove` checks that over every reachable state. A clear removes a field's old "
         "value, so that the taus write it afresh. \"Fires\" below is read from the weights alone, taking every field as 0 or 1.\n";
    for (size_t k = 0; k < m.rules.size(); ++k) {
        const Rule& r = m.rules[k];
        const Firing& fi = ex.fire[k];
        o += ssprintf("\n### rule %zu%s: `%s`\n\n", k + 1, r.line ? ssprintf(" (line %d)", r.line).c_str() : "", r.text.c_str());
        if (!r.from.empty()) o += "- from: " + (r.from == "automatic" ? std::string("quill, for an `in.<token>` field") : "`" + one_line(r.from) + "`") + "\n";
        if (!r.note.empty()) o += "- meaning: " + r.note + "\n";
        if (r.kind == Rule::Clear) o += "- acts on every step: the field goes back to 0 before the taus add to it\n";
        else if (fi.known) {
            o += std::string("- ") + (r.kind == Rule::Tau ? "fires (z >= 1): " : "active (z > 0): ") + fi.when;
            if (!fi.z.empty()) o += "  [" + z_list(fi.z) + "]";
            o += "\n";
            if (!fi.ramp.empty()) o += "- **possible ramp** (0 < z < 1): " + fi.ramp + ". `quill prove` says whether a reachable state gets there.\n";
        } else if (F) o += "- reads too many fields, or unnamed directions, for a short description\n";
        o += r.n1 >= 0 ? ssprintf("- neurons %d and %d\n", r.n0, r.n1) : ssprintf("- neuron %d\n", r.n0);
    }

    o += "\n## Neurons\n\nEach neuron computes `h = relu(z)` and adds `h` times its weights to the fields it writes.\n\n"
         "| n | rule | z | writes (per unit of h) | active when |\n|---|---|---|---|---|\n";
    for (size_t i = 0; i < (size_t)m.net.n; ++i) {
        const size_t k = (size_t)m.neuron_rule[i];
        const Rule& r = m.rules[k];
        const Firing& fi = ex.fire[k];
        std::string role, when;
        if (r.kind == Rule::Tau) {
            const bool first = (int)i == r.n0;
            role = first ? "tau, rising half" : "tau, capping half";
            when = (first ? std::string("z > 0: ") : std::string("z > 0, that is the rule's z > 1: ")) + (fi.known ? fi.when : std::string("see the rule"));
        } else if (r.kind == Rule::Clear) {
            const bool first = (int)i == r.n0;
            role = first ? "clear, positive half" : "clear, negative half";
            when = first ? "the field is above 0" : "the field is below 0 (never, for a clean field)";
        } else {
            role = "lone relu";
            when = fi.known ? "z > 0: " + fi.when : std::string("z > 0");
        }
        o += ssprintf("| %zu | %zu: %s | `%s` | `%s` | %s |\n", i, k + 1, role.c_str(), md_cell(neuron_reads(m, i)).c_str(),
                      md_cell(neuron_writes(m, i)).c_str(), md_cell(when).c_str());
    }

    o += "\n## Readout\n\n";
    if (F) {
        const Lin rl = readout_lin(m);
        Lin hold = rl, emit = rl;
        for (auto& x : hold.terms) x.coef = snap9(-0.5 * x.coef);
        for (auto& x : emit.terms) x.coef = snap9(0.5 * x.coef);
        o += "margin = emit - hold = `" + m.lin_text(rl) + "`\n\nRows of Wout: hold = `" + m.lin_text(hold) + "`, emit = `" + m.lin_text(emit) +
             "`. There is no readout bias: constants ride on `one`. The decision is emit when the margin is above 0, hold otherwise.\n";
    } else {
        o += "margin = (Wout[emit] - Wout[hold]) . r. The decision is emit when the margin is above 0, hold otherwise.\n";
    }

    if (!m.spec_text.empty()) {
        Spec s = parse_spec(m.spec_text);
        s.rotate = m.rotated;                                // as built: a --rotate or --no-rotate override decides, not the text
        s.seed = m.seed;
        if (!s.sugar.empty()) {
            o += "\n## Expansions\n\nEach shorthand statement, and the primitive statements quill wrote for it.\n";
            for (size_t g = 0; g < s.sugar.size(); ++g) {
                o += ssprintf("\nline %d:\n\n```\n%s\n```\n\nis shorthand for\n\n```\n", s.sugar[g].line, s.sugar[g].text.c_str());
                for (const auto& op : s.ops)
                    if (op.sugar == (int)g) o += op_text(s, op) + (op.note.empty() ? "" : "    # " + op.note) + "\n";
                o += "```\n";
            }
        }
        o += "\n## The whole network in primitive statements\n\nThis text is itself a specification and builds the identical network.\n\n```\n" +
             expanded_text(s) + "```\n";
    }
    return o;
}

}  // namespace quill
