// quill · prove.cpp — breadth-first enumeration of every reachable state, and what follows from it:
// exactness of every threshold, cleanliness of every field, noise tolerance, equivalence, sizes.
//
// Why "proved" means proved. The kernel is a deterministic function on float64 states. When every
// successor of every enumerated state is bit-for-bit one of the enumerated states, the set is closed:
// by induction no token sequence of any length leaves it, so what was checked at every (state, token)
// pair holds for ever. When closing needed the 1e-9 tolerance instead (a rotated basis rounds), the
// same checks were made but rounding could in principle accumulate along directions nothing rewrites,
// so the grade is "checked". The inexact flag of the processor tells which arithmetic it was.
#include "prove.h"

#include <float.h>
#include <cmath>
#include <map>
#include <unordered_map>

namespace quill {
namespace {

constexpr size_t kKeep = 8;                // witnesses kept per kind of fault
constexpr size_t kStateTable = 64;         // the state table is listed when there are at most this many states

// One fault, with everything needed to say where it is.
struct Hit {
    bool ramp = true;                      // false: a field that is not 0 or 1
    std::vector<double> before;            // the state the step started from; empty: the start state itself
    std::vector<double> after;             // unclean: the state reached
    std::vector<int> path;                 // tokens from the start to `before`
    int token = -1;
    int rule = -1;                         // ramp: the threshold
    double z = 0;
    int field = -1;                        // unclean: the field and its value
    double value = 0, kept = 0;            // kept: what remained of its old value and of the token's own write
    bool cleared = false;                  // a clear acted on it in this step
    std::vector<int> add_rule;             // the rules that wrote it in this step
    std::vector<double> add_amount, add_z;
};

struct RuleStat {
    double min_dist = 1e300, z_at = 0;
    int state = -1, token = -1;            // where the smallest distance occurs
    double z_min = 1e300, z_max = -1e300;
    size_t cases = 0, fires = 0;
    std::vector<double> zvals;             // the distinct values of z (to 1e-9), at most 12
};

struct Enum {
    int dp = 0, T = 0;
    std::vector<double> states;
    std::vector<int> parent, ptoken, depth, next;
    std::vector<char> dirty;
    std::vector<double> margin;
    std::vector<RuleStat> rule;
    std::vector<Hit> ramps, dirties;
    size_t ramp_total = 0, dirty_total = 0, transitions = 0;
    bool capped = false, exact_arith = true;
    double residual = 0, max_offset = 0, outside = 0;
    int exhaustive_depth = 0, max_depth = 0;
    int N() const { return (int)parent.size(); }
    const double* state(int i) const { return states.data() + (size_t)i * (size_t)dp; }
    std::vector<int> path(int i) const {
        std::vector<int> p;
        for (; i > 0; i = parent[(size_t)i]) p.push_back(ptoken[(size_t)i]);
        std::reverse(p.begin(), p.end());
        return p;
    }
};

// Distance of a threshold argument from the open interval (0, 1): positive outside, negative inside.
inline double edge_distance(double z) { return std::max(-z, z - 1.0); }

// The checks made at every step, shared by the enumeration and by the random walks.
struct Checker {
    const Model& m;
    Kernel K;
    std::vector<int> taus;
    std::vector<char> named;               // axis-aligned: which coordinates are named fields
    size_t F;
    explicit Checker(const Model& model) : m(model), K(model.net), F((size_t)model.F()) {
        for (size_t k = 0; k < m.rules.size(); ++k)
            if (m.rules[k].kind == Rule::Tau) taus.push_back((int)k);
        if (!m.rotated) {
            named.assign((size_t)m.net.d, 0);
            for (int c : m.coord) named[(size_t)c] = 1;
        }
    }
    // The first field that is not a clean 0 or 1 (the constant must be 1), or -1.
    int unclean(const double* s, double& value, double& offset, double& outside) const {
        int bad = -1;
        std::vector<double> v(F);
        for (size_t f = 0; f < F; ++f) {
            v[f] = m.value(s, (int)f);
            const double r = std::round(v[f]), off = std::fabs(v[f] - r);
            const bool ok = off <= kTol && ((int)f == m.one ? r == 1 : (r == 0 || r == 1));
            if (ok) offset = std::max(offset, off);
            else if (bad < 0) { bad = (int)f; value = v[f]; }
        }
        if (F) {                           // what the state holds outside the named fields
            const size_t d = (size_t)m.net.d;
            for (size_t j = 0; j < d; ++j) {
                double rest = s[j];
                if (!m.rotated) { if (named[j]) continue; }
                else for (size_t f = 0; f < F; ++f) rest -= v[f] * m.dirs[f * d + j];
                outside = std::max(outside, std::fabs(rest));
            }
        }
        return bad;
    }
    // Where an unclean value came from: what was kept, and what each rule added.
    void explain(Hit& h, const double* before, int token, const double* z) const {
        const size_t f = (size_t)h.field;
        double kept = m.value(before, h.field) + m.em[(size_t)token * F + f];
        for (size_t k = 0; k < m.rules.size(); ++k) {
            const Rule& r = m.rules[k];
            double c = 0;
            for (int i : { r.n0, r.n1 })
                if (i >= 0 && z[i] > 0) c += z[i] * m.wr[(size_t)i * F + f];
            if (std::fabs(c) <= kTol) continue;
            if (r.kind == Rule::Clear) { kept += c; h.cleared = true; continue; }
            h.add_rule.push_back((int)k);
            h.add_amount.push_back(c);
            h.add_z.push_back(z[r.n0]);
        }
        h.kept = kept;
    }
};

uint64_t hash_state(const double* s, int n) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    for (int j = 0; j < n; ++j) {
        uint64_t bits;
        memcpy(&bits, s + j, 8);
        h = (h ^ bits) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    return h;
}

// Finds a state among those seen: bit for bit first, then within the tolerance.
struct Index {
    int dp = 0;
    std::unordered_multimap<uint64_t, int> exact;
    std::multimap<double, int> proj;
    std::vector<double> pv;
    double p1 = 0;
    explicit Index(int n) : dp(n), pv((size_t)n) {
        Rng rng(0x51D5EED);
        for (auto& v : pv) {
            v = (rng.uniform() + 0.5) * ((rng.next() & 1) ? 1.0 : -1.0);
            p1 += std::fabs(v);
        }
    }
    double project(const double* s) const {
        double p = 0;
        for (int j = 0; j < dp; ++j) p += pv[(size_t)j] * s[j];
        return p;
    }
    int find(const Enum& e, const double* s, double& residual) const {
        const auto range = exact.equal_range(hash_state(s, dp));
        for (auto it = range.first; it != range.second; ++it) {
            const double* o = e.state(it->second);
            bool same = true;
            for (int j = 0; j < dp && same; ++j) same = o[j] == s[j];
            if (same) { residual = 0; return it->second; }
        }
        const double p = project(s), slack = kTol * p1 * 1.001 + std::fabs(p) * 1e-12;
        for (auto it = proj.lower_bound(p - slack); it != proj.end() && it->first <= p + slack; ++it) {
            const double* o = e.state(it->second);
            double worst = 0;
            for (int j = 0; j < dp; ++j) worst = std::max(worst, std::fabs(o[j] - s[j]));
            if (worst <= kTol) { residual = worst; return it->second; }
        }
        return -1;
    }
    void add(const double* s, int id) {
        exact.emplace(hash_state(s, dp), id);
        proj.emplace(project(s), id);
    }
};

void note_z(RuleStat& st, double z, int state, int token) {
    const double dist = edge_distance(z);
    ++st.cases;
    if (z >= 1.0 - kTol) ++st.fires;
    if (dist < st.min_dist) { st.min_dist = dist; st.z_at = z; st.state = state; st.token = token; }
    st.z_min = std::min(st.z_min, z);
    st.z_max = std::max(st.z_max, z);
    const double zs = snap9(z);
    if (st.zvals.size() < 12 && std::find(st.zvals.begin(), st.zvals.end(), zs) == st.zvals.end()) st.zvals.push_back(zs);
}

Enum enumerate(const Checker& ck, size_t cap) {
    const Model& m = ck.m;
    const Kernel& K = ck.K;
    Enum e;
    e.dp = K.padded();
    e.T = m.net.T();
    e.rule.resize(m.rules.size());
    const size_t dp = (size_t)e.dp, T = (size_t)e.T;
    std::vector<double> cur(dp), z((size_t)K.padded_neurons() + 2);
    Index index(e.dp);
    const auto add = [&](const double* s, int parent, int token) {
        const int id = e.N();
        e.states.insert(e.states.end(), s, s + dp);
        e.parent.push_back(parent);
        e.ptoken.push_back(token);
        e.depth.push_back(parent < 0 ? 0 : e.depth[(size_t)parent] + 1);
        e.max_depth = std::max(e.max_depth, e.depth.back());
        e.dirty.push_back(0);
        e.next.insert(e.next.end(), T, -1);
        index.add(s, id);
        return id;
    };
    K.load(m.net.r0.data(), cur.data());
    for (auto& v : cur) v += 0.0;                             // -0 becomes +0
    add(cur.data(), -1, -1);
    {
        Hit h;
        h.ramp = false;
        h.field = ck.unclean(cur.data(), h.value, e.max_offset, e.outside);
        if (h.field >= 0) {
            e.dirty[0] = 1;
            ++e.dirty_total;
            h.after = cur;
            h.kept = h.value;
            e.dirties.push_back(std::move(h));
        }
    }
    for (int i = 0; i < e.N() && !e.capped; ++i) {
        if (e.dirty[(size_t)i]) continue;                     // an unclean state is a fault already; what follows it says nothing more
        e.exhaustive_depth = e.depth[(size_t)i];
        for (int t = 0; t < e.T; ++t) {
            memcpy(cur.data(), e.state(i), dp * sizeof(double));
            _clearfp();
            K.step(cur.data(), t, z.data());
            if (_statusfp() & _SW_INEXACT) e.exact_arith = false;
            ++e.transitions;
            for (int k : ck.taus) {
                const double zk = z[(size_t)m.rules[(size_t)k].n0];
                note_z(e.rule[(size_t)k], zk, i, t);
                if (edge_distance(zk) < -kTol) {
                    ++e.ramp_total;
                    if (e.ramps.size() < kKeep) {
                        Hit h;
                        h.before.assign(e.state(i), e.state(i) + dp);
                        h.path = e.path(i);
                        h.token = t;
                        h.rule = k;
                        h.z = zk;
                        e.ramps.push_back(std::move(h));
                    }
                }
            }
            for (size_t k = 0; k < m.rules.size(); ++k)       // lone neurons: record their range too
                if (m.rules[k].kind == Rule::Neuron) {
                    RuleStat& st = e.rule[k];
                    const double zk = z[(size_t)m.rules[k].n0];
                    ++st.cases;
                    if (zk > kTol) ++st.fires;
                    st.z_min = std::min(st.z_min, zk);
                    st.z_max = std::max(st.z_max, zk);
                }
            for (auto& v : cur) v += 0.0;
            double residual = 0;
            int j = index.find(e, cur.data(), residual);
            if (j < 0) {
                if ((size_t)e.N() >= cap) { e.capped = true; break; }
                j = add(cur.data(), i, t);
                Hit h;
                h.ramp = false;
                h.field = ck.unclean(cur.data(), h.value, e.max_offset, e.outside);
                if (h.field >= 0) {
                    e.dirty[(size_t)j] = 1;
                    ++e.dirty_total;
                    if (e.dirties.size() < kKeep) {
                        h.before.assign(e.state(i), e.state(i) + dp);
                        h.after = cur;
                        h.path = e.path(i);
                        h.token = t;
                        ck.explain(h, e.state(i), t, z.data());
                        e.dirties.push_back(std::move(h));
                    }
                }
            } else {
                e.residual = std::max(e.residual, residual);
            }
            e.next[(size_t)i * T + (size_t)t] = j;
        }
    }
    if (!e.capped) e.exhaustive_depth = e.max_depth + 1;
    e.margin.resize((size_t)e.N());
    for (int i = 0; i < e.N(); ++i) e.margin[(size_t)i] = K.margin(e.state(i));
    return e;
}

// Random walks from the start, for when the enumeration stopped at the cap: evidence, not proof.
struct Sampled {
    bool ran = false;
    size_t walks = 64, steps_per_walk = 4096, steps = 0;
    uint64_t seed = 1;
    std::vector<Hit> hits;
};

Sampled sample(const Checker& ck) {
    const Model& m = ck.m;
    const Kernel& K = ck.K;
    Sampled s;
    s.ran = true;
    const size_t dp = (size_t)K.padded();
    std::vector<double> cur(dp), before(dp), z((size_t)K.padded_neurons() + 2);
    Rng rng(s.seed);
    for (size_t w = 0; w < s.walks && s.hits.empty(); ++w) {
        K.load(m.net.r0.data(), cur.data());
        std::vector<int> path;
        for (size_t k = 0; k < s.steps_per_walk && s.hits.empty(); ++k) {
            const int t = (int)rng.below((uint32_t)m.net.T());
            before = cur;
            K.step(cur.data(), t, z.data());
            ++s.steps;
            for (int r : ck.taus) {
                const double zk = z[(size_t)m.rules[(size_t)r].n0];
                if (edge_distance(zk) < -kTol && s.hits.empty()) {
                    Hit h;
                    h.before = before;
                    h.path = path;
                    h.token = t;
                    h.rule = r;
                    h.z = zk;
                    s.hits.push_back(std::move(h));
                }
            }
            Hit h;
            h.ramp = false;
            double offset = 0, outside = 0;
            h.field = ck.unclean(cur.data(), h.value, offset, outside);
            if (h.field >= 0 && s.hits.empty()) {
                h.before = before;
                h.after = cur;
                h.path = path;
                h.token = t;
                ck.explain(h, before.data(), t, z.data());
                s.hits.push_back(std::move(h));
            }
            path.push_back(t);
        }
    }
    return s;
}

// ---- saying where a fault is ----------------------------------------------------------------------
std::string rule_ref(const Model& m, int k) {
    const Rule& r = m.rules[(size_t)k];
    return ssprintf("rule %d (%s%s)", k + 1, r.line ? ssprintf("line %d: ", r.line).c_str() : "", r.text.c_str());
}
std::string path_text(const Model& m, const std::vector<int>& path) {
    if (path.empty()) return "the start state";
    std::string out;
    for (size_t i = 0; i < path.size(); ++i) {
        if (i == 24 && path.size() > 25) { out += ssprintf(" ... (%zu tokens)", path.size()); break; }
        out += (i ? " " : "") + m.net.tokens[(size_t)path[i]];
    }
    return out;
}
JV path_json(const Model& m, const std::vector<int>& path) {
    JV a = JV::array();
    for (int t : path) a.push(m.net.tokens[(size_t)t]);
    return a;
}

std::string hit_text(const Model& m, const Hit& h) {
    if (h.ramp)
        return rule_ref(m, h.rule) + " has z = " + show(h.z) + ", strictly inside (0, 1), in state " + state_text(m, h.before.data()) +
               " on token " + m.net.tokens[(size_t)h.token] + " (reached by: " + path_text(m, h.path) + ")";
    const std::string& name = m.fields[(size_t)h.field];
    const std::string what = (h.field == m.one ? "the constant `one` is " + show(h.value) + ", not 1" : "field " + name + " is " + show(h.value) + ", not 0 or 1");
    if (h.before.empty()) return what + ", in the start state";
    std::string out = what + ", after token " + m.net.tokens[(size_t)h.token] + " in state " + state_text(m, h.before.data()) +
                      " (reached by: " + path_text(m, h.path) + ")";
    std::vector<std::string> parts;
    if (std::fabs(h.kept) > kTol) parts.push_back(show(h.kept) + (h.cleared ? " left after its clear" : " kept from before (nothing clears " + name + ")"));
    for (size_t i = 0; i < h.add_rule.size(); ++i)
        parts.push_back(show(h.add_amount[i]) + " from " + rule_ref(m, h.add_rule[i]) + " at z = " + show(h.add_z[i]));
    if (!parts.empty()) out += ": " + name + " = " + join(parts, " + ");
    return out;
}

JV hit_json(const Model& m, const Hit& h) {
    JV o = JV::object();
    if (h.ramp) {
        const Rule& r = m.rules[(size_t)h.rule];
        o.set("rule", h.rule + 1).set("line", r.line ? JV(r.line) : JV()).set("text", r.text).set("z", h.z);
    } else {
        o.set("field", m.fields[(size_t)h.field]).set("value", snap9(h.value));
    }
    o.set("token", h.token >= 0 ? JV(m.net.tokens[(size_t)h.token]) : JV());
    o.set("state", h.before.empty() ? JV() : state_json(m, h.before.data()));
    o.set("path", path_json(m, h.path));
    if (!h.ramp) {
        o.set("after", h.after.empty() ? JV() : state_json(m, h.after.data()));
        o.set("kept", snap9(h.kept));
        JV w = JV::array();
        for (size_t i = 0; i < h.add_rule.size(); ++i) {
            const Rule& r = m.rules[(size_t)h.add_rule[i]];
            JV x = JV::object();
            x.set("rule", h.add_rule[i] + 1).set("line", r.line ? JV(r.line) : JV()).set("text", r.text);
            x.set("adds", snap9(h.add_amount[i])).set("z", h.add_z[i]);
            w.push(x);
        }
        o.set("written_by", w);
    }
    o.set("message", hit_text(m, h));
    return o;
}

Moore moore_of(const Model& m, const Enum& e) {
    Moore a;
    a.label = "the network " + m.net.name;
    a.tokens = m.net.tokens;
    a.states = e.N();
    a.next = e.next;
    a.complete = !e.capped && e.dirty_total == 0;
    a.exact = e.residual == 0;
    for (double v : e.margin) a.emit.push_back(v > 0 ? 1 : 0);
    return a;
}

// ---- equivalence: breadth-first over pairs of states ------------------------------------------------
struct Equiv {
    int holds = -1;                      // 1 yes, 0 no, -1 not decided
    size_t pairs = 0;
    std::vector<int> path;               // a shortest token sequence on which the decisions differ
    int a_emit = 0, b_emit = 0;
    bool functional = true;              // each state of a met exactly one state of b
};

Equiv equivalence(const Moore& a, const Moore& b) {
    Equiv q;
    const size_t T = a.tokens.size();
    std::unordered_map<uint64_t, int> seen;
    std::vector<std::pair<int, int>> pair;
    std::vector<int> parent, ptoken;
    const auto key = [](int x, int y) { return ((uint64_t)(uint32_t)x << 32) | (uint32_t)y; };
    seen.emplace(key(a.start, b.start), 0);
    pair.push_back({ a.start, b.start });
    parent.push_back(-1);
    ptoken.push_back(-1);
    std::vector<int> met((size_t)a.states, -1);
    met[(size_t)a.start] = b.start;
    bool open = false;
    for (size_t i = 0; i < pair.size(); ++i) {
        for (size_t t = 0; t < T; ++t) {
            const int x = a.next[(size_t)pair[i].first * T + t], y = b.next[(size_t)pair[i].second * T + t];
            if (x < 0 || y < 0) { open = true; continue; }
            if (a.emit[(size_t)x] != b.emit[(size_t)y]) {
                q.holds = 0;
                q.a_emit = a.emit[(size_t)x];
                q.b_emit = b.emit[(size_t)y];
                q.path.push_back((int)t);
                for (int k = (int)i; k > 0; k = parent[(size_t)k]) q.path.push_back(ptoken[(size_t)k]);
                std::reverse(q.path.begin(), q.path.end());
                q.pairs = pair.size();
                return q;
            }
            if (met[(size_t)x] >= 0 && met[(size_t)x] != y) q.functional = false;
            met[(size_t)x] = y;
            if (seen.emplace(key(x, y), (int)pair.size()).second) {
                pair.push_back({ x, y });
                parent.push_back((int)i);
                ptoken.push_back((int)t);
            }
        }
    }
    q.pairs = pair.size();
    q.holds = (open || !a.complete || !b.complete) ? -1 : 1;
    return q;
}

const char* weakest(const std::string& a, const std::string& b) {
    const auto rank = [](const std::string& g) { return g == "proved" ? 3 : g == "checked" ? 2 : g == "sampled" ? 1 : 0; };
    static const char* names[] = { "undecided", "sampled", "checked", "proved" };
    return names[std::min(rank(a), rank(b))];
}

JV holds_json(int h) { return h < 0 ? JV() : JV(h == 1); }

}  // namespace

// ======================================================================
// transition tables
// ======================================================================
Moore moore_from_machine(const JV& j, const std::vector<std::string>& net_tokens) {
    const auto bad = [](const std::string& what) -> Fail {
        return Fail("E_EQUIV", "the machine file: " + what,
                    "a machine is {\"tokens\": [\"zero\", \"one\"], \"delta\": [[0, 1], [0, 0]], \"init\": 0, \"emit\": [1]}; "
                    "delta[state][token] is the next state");
    };
    if (j.t != JV::Obj) throw bad("it must be a JSON object");
    for (const auto& kv : j.obj)
        if (kv.first != "name" && kv.first != "tokens" && kv.first != "delta" && kv.first != "init" && kv.first != "emit")
            throw bad("unknown key '" + kv.first + "' (a machine has name, tokens, delta, init and emit)");
    const size_t T = net_tokens.size();
    std::vector<size_t> column(T);                            // network token -> column of delta
    for (size_t t = 0; t < T; ++t) column[t] = t;
    if (const JV* toks = j.get("tokens")) {
        if (toks->t != JV::Arr) throw bad("tokens must be a list of names");
        std::vector<std::string> names;
        for (const auto& x : toks->arr) {
            if (!x.is_str()) throw bad("tokens must be a list of names");
            names.push_back(x.s);
        }
        for (size_t t = 0; t < T; ++t) {
            const auto it = std::find(names.begin(), names.end(), net_tokens[t]);
            if (it == names.end() || names.size() != T)
                throw bad("its tokens are [" + join(names, ", ") + "] but the network's are [" + join(net_tokens, ", ") + "]: the two must be the same names");
            column[t] = (size_t)(it - names.begin());
        }
    }
    const JV* delta = j.get("delta");
    if (!delta || delta->t != JV::Arr || delta->arr.empty()) throw bad("delta is missing: a list with one row per state");
    Moore m;
    const JV* name = j.get("name");
    m.label = "the machine " + (name && name->is_str() ? name->s : std::string("table"));
    m.tokens = net_tokens;
    m.states = (int)delta->arr.size();
    m.next.assign((size_t)m.states * T, 0);
    for (size_t s = 0; s < delta->arr.size(); ++s) {
        const JV& row = delta->arr[s];
        if (row.t != JV::Arr || row.arr.size() != T)
            throw bad(ssprintf("row %zu of delta must list %zu next states, one per token", s, T));
        for (size_t t = 0; t < T; ++t) {
            const JV& v = row.arr[column[t]];
            if (v.t != JV::Int || v.i < 0 || v.i >= m.states)
                throw bad(ssprintf("delta[%zu][%zu] must be a state number from 0 to %d", s, column[t], m.states - 1));
            m.next[s * T + t] = (int)v.i;
        }
    }
    if (const JV* init = j.get("init")) {
        if (init->t != JV::Int || init->i < 0 || init->i >= m.states) throw bad(ssprintf("init must be a state number from 0 to %d", m.states - 1));
        m.start = (int)init->i;
    }
    m.emit.assign((size_t)m.states, 0);
    if (const JV* emit = j.get("emit")) {
        if (emit->t != JV::Arr) throw bad("emit must be a list of state numbers");
        for (const auto& v : emit->arr) {
            if (v.t != JV::Int || v.i < 0 || v.i >= m.states) throw bad(ssprintf("emit must list state numbers from 0 to %d", m.states - 1));
            m.emit[(size_t)v.i] = 1;
        }
    }
    return m;
}

Moore moore_from_model(const Model& m, size_t cap) {
    const Checker ck(m);
    return moore_of(m, enumerate(ck, cap));
}

// ======================================================================
// the proof
// ======================================================================
Proof prove(const Model& m, const Model* build, const ProveOptions& opt, const Moore* other_in, const JV* other_input) {
    const Checker ck(m);
    const Enum e = enumerate(ck, opt.cap);
    Sampled sm;
    if (e.capped && e.ramps.empty() && e.dirties.empty()) sm = sample(ck);
    const size_t F = ck.F, T = (size_t)e.T, d = (size_t)m.net.d;
    const bool closed = !e.capped && e.dirty_total == 0;
    const std::string base = e.residual == 0 ? "proved" : "checked";
    const std::string witness = e.exact_arith ? "proved" : "checked";
    // A decision is made after a token: a state decides only if some token leads to it. (The start
    // state does when the network can return to it, and not otherwise.)
    std::vector<char> decides((size_t)e.N(), 0);
    for (int j : e.next)
        if (j >= 0) decides[(size_t)j] = 1;
    std::vector<std::string> lines;
    JV warnings = JV::array();
    const auto warn = [&](const char* code, const std::string& msg) {
        JV w = JV::object();
        w.set("code", code).set("line", JV()).set("message", msg).set("hint", JV());
        warnings.push(w);
    };
    for (const auto& w : m.warnings) {
        JV x = JV::object();
        x.set("code", w.code).set("line", w.line ? JV(w.line) : JV()).set("message", w.message).set("hint", w.hint.empty() ? JV() : JV(w.hint));
        warnings.push(x);
    }

    JV res = JV::object();
    res.set("name", m.net.name).set("input", m.input);
    JV options = JV::object();
    options.set("cap", (unsigned long long)opt.cap).set("tolerance", kTol);
    res.set("options", options);
    res.set("basis", m.rotated ? "rotated" : "axis").set("arithmetic", e.exact_arith ? "exact" : "rounded");
    JV results = JV::object();
    int worst_holds = 1;                                     // 1 all hold, -1 something undecided, 0 something fails
    std::string refuting_grade, overall = "proved";
    const auto fold = [&](int holds, const std::string& grade) {
        if (holds == 0) { if (worst_holds != 0) refuting_grade = grade; worst_holds = 0; }
        else if (holds < 0 && worst_holds == 1) worst_holds = -1;
        if (holds == 1) overall = weakest(overall, grade);
    };

    // 1. reachable states and transitions
    {
        const int holds = closed ? 1 : -1;
        const std::string grade = closed ? base : "undecided";
        JV r = JV::object();
        r.set("holds", holds_json(holds)).set("grade", grade).set("states", e.N()).set("transitions", (unsigned long long)e.transitions);
        r.set("complete", closed).set("depth", e.max_depth).set("closure_residual", e.residual);
        std::string line = ssprintf("%s state%s, %s transition%s", commas((unsigned long long)e.N()).c_str(), e.N() == 1 ? "" : "s",
                                    commas(e.transitions).c_str(), e.transitions == 1 ? "" : "s");
        if (closed)
            line += e.residual == 0 ? std::string("; the set is closed bit for bit") + (e.exact_arith ? ", in exact arithmetic" : " (float64 rounding occurred)")
                                    : "; the set is closed to " + sci(e.residual) + " (float64, tolerance 1e-9)";
        else if (e.capped) {
            line += ssprintf("; STOPPED at the cap of %s states: every token sequence of up to %d tokens was covered", commas(opt.cap).c_str(), e.exhaustive_depth);
            r.set("exhaustive_to_depth", e.exhaustive_depth);
        } else line += ssprintf("; %zu of them %s not clean and %s not followed further", e.dirty_total, e.dirty_total == 1 ? "is" : "are", e.dirty_total == 1 ? "was" : "were");
        results.set("reachable", r);
        fold(holds, grade);
        lines.push_back("reachable    " + line + "   [" + grade + "]");
    }

    // 2. exactness
    double min_dist = 1e300;
    int min_rule = -1;
    for (int k : ck.taus)
        if (e.rule[(size_t)k].cases && e.rule[(size_t)k].min_dist < min_dist) { min_dist = e.rule[(size_t)k].min_dist; min_rule = k; }
    const std::vector<Hit>& sampled_hits = sm.hits;
    {
        const bool sampled_ramp = !sampled_hits.empty() && sampled_hits[0].ramp;
        const int holds = (e.ramp_total > 0 || sampled_ramp) ? 0 : closed ? 1 : -1;
        const std::string grade = holds == 0 ? witness : holds == 1 ? base : "undecided";
        JV r = JV::object();
        r.set("holds", holds_json(holds)).set("grade", grade).set("thresholds", (unsigned long long)ck.taus.size());
        r.set("min_distance", min_rule >= 0 ? JV(min_dist) : JV());
        if (min_rule >= 0) {
            const RuleStat& st = e.rule[(size_t)min_rule];
            const Rule& rr = m.rules[(size_t)min_rule];
            JV at = JV::object();
            at.set("rule", min_rule + 1).set("line", rr.line ? JV(rr.line) : JV()).set("text", rr.text).set("z", st.z_at);
            at.set("token", m.net.tokens[(size_t)st.token]).set("state", state_json(m, e.state(st.state))).set("path", path_json(m, e.path(st.state)));
            r.set("at", at);
        } else r.set("at", JV());
        JV v = JV::array();
        for (const auto& h : e.ramps) v.push(hit_json(m, h));
        if (sampled_ramp) v.push(hit_json(m, sampled_hits[0]));
        r.set("violation_count", (unsigned long long)(e.ramp_total + (sampled_ramp ? 1 : 0))).set("violations", v);
        results.set("exactness", r);
        fold(holds, grade);
        std::string line;
        if (holds == 0) {
            const Hit& h = e.ramps.empty() ? sampled_hits[0] : e.ramps[0];
            line = "FAILS: " + hit_text(m, h);
            if (e.ramp_total > 1) line += ssprintf("; %zu such cases in all", e.ramp_total);
        } else if (ck.taus.empty()) line = "the network has no threshold";
        else {
            const RuleStat& st = e.rule[(size_t)min_rule];
            line = ssprintf("%s %s threshold%s outside (0, 1) at every state and token; smallest distance %s (", holds == 1 ? "all" : "so far all",
                            commas(ck.taus.size()).c_str(), ck.taus.size() == 1 ? " stays" : "s stay", show(min_dist).c_str()) +
                   rule_ref(m, min_rule) + " at z = " + show(st.z_at) + ", state " + state_text(m, e.state(st.state)) + ", token " +
                   m.net.tokens[(size_t)st.token] + ")";
            if (min_dist <= kTol)
                warn("W_EDGE", rule_ref(m, min_rule) + " lands exactly on an edge of (0, 1): exact, but no error at all is tolerated there");
        }
        lines.push_back("exactness    " + line + "   [" + grade + "]");
    }

    // 3. cleanliness
    {
        const bool sampled_dirty = !sampled_hits.empty() && !sampled_hits[0].ramp;
        const bool applies = F > 0;
        const int holds = !applies ? -1 : (e.dirty_total > 0 || sampled_dirty) ? 0 : closed ? 1 : -1;
        const std::string grade = holds == 0 ? witness : holds == 1 ? base : "undecided";
        JV r = JV::object();
        r.set("holds", holds_json(holds)).set("grade", grade).set("fields", (unsigned long long)F);
        r.set("max_offset", applies ? JV(e.max_offset) : JV()).set("outside_named_fields", applies ? JV(e.outside) : JV());
        JV v = JV::array();
        for (const auto& h : e.dirties) v.push(hit_json(m, h));
        if (sampled_dirty) v.push(hit_json(m, sampled_hits[0]));
        r.set("violation_count", (unsigned long long)(e.dirty_total + (sampled_dirty ? 1 : 0))).set("violations", v);
        if (!applies) r.set("skipped", "no field names are known for this network");
        results.set("cleanliness", r);
        if (applies) fold(holds, grade);
        std::string line;
        if (!applies) line = "not checked: no field names are known (no manifest.json)";
        else if (holds == 0) {
            const Hit& h = e.dirties.empty() ? sampled_hits[0] : e.dirties[0];
            line = "FAILS: " + hit_text(m, h);
            if (e.dirty_total > 1) line += ssprintf("; %zu unclean states in all", e.dirty_total);
        } else {
            line = ssprintf("every one of the %zu named fields is 0 or 1 in every %sstate", F, holds == 1 ? "" : "explored ");
            if (e.max_offset > 0) line += " (to within " + sci(e.max_offset) + ")";
            if (e.outside > kTol) {
                line += "; the states also hold up to " + sci(e.outside) + " outside the named fields";
                warn("W_OUTSIDE", "reachable states have components outside the named fields (largest " + sci(e.outside) + "): the field values do not describe them fully");
            }
        }
        lines.push_back("cleanliness  " + line + (applies ? "   [" + grade + "]" : ""));
    }

    // 4. noise tolerance
    const bool exact_ok = e.ramp_total == 0 && sampled_hits.empty() && closed;
    {
        double linf = 1e300, l2 = 1e300, single = 1e300;
        int at = -1;
        for (int k : ck.taus) {
            const RuleStat& st = e.rule[(size_t)k];
            if (!st.cases) continue;
            const double* w = m.net.W1.data() + (size_t)m.rules[(size_t)k].n0 * d;
            double n1 = 0, n2 = 0, ninf = 0;
            for (size_t j = 0; j < d; ++j) {
                n1 += std::fabs(w[j]);
                n2 += w[j] * w[j];
                ninf = std::max(ninf, std::fabs(w[j]));
            }
            if (n1 == 0) continue;                            // it reads no coordinate: no error can move it
            const double room = std::max(st.min_dist, 0.0);
            if (room / n1 < linf) { linf = room / n1; at = k; }
            l2 = std::min(l2, room / std::sqrt(n2));
            single = std::min(single, room / ninf);
        }
        double dmin = 1e300, w1 = 0;
        for (size_t j = 0; j < d; ++j) w1 += std::fabs(m.net.Wout[d + j] - m.net.Wout[j]);
        for (int i = 0; i < e.N(); ++i)
            if (!e.dirty[(size_t)i] && decides[(size_t)i]) dmin = std::min(dmin, std::fabs(e.margin[(size_t)i]));
        const bool have = exact_ok && at >= 0;
        const std::string grade = exact_ok ? base : "undecided";
        JV r = JV::object();
        r.set("grade", grade).set("linf", have ? JV(linf) : JV()).set("l2", have ? JV(l2) : JV()).set("single", have ? JV(single) : JV());
        r.set("rule", have ? JV(at + 1) : JV());
        r.set("decision_linf", exact_ok && w1 > 0 && dmin < 1e299 ? JV(dmin / w1) : JV());
        r.set("coordinates", m.rotated ? "the stored (rotated) coordinates" : "the stored coordinates, which are the fields");
        results.set("noise", r);
        if (exact_ok) overall = weakest(overall, grade);
        std::string line;
        if (!exact_ok) line = "not computed: it needs every threshold exact over the complete set of states";
        else if (at < 0) line = "no threshold reads the state, so no error on the state can move one";
        else
            line = "an error of up to " + show(linf) + " on every coordinate of the state cannot move a threshold into (0, 1); " + show(l2) +
                   " in Euclidean norm; " + show(single) + " on one coordinate alone (" + (m.rotated ? "rotated coordinates" : "coordinates are fields") + ")";
        lines.push_back("noise        " + line + (exact_ok ? "   [" + grade + "]" : ""));
    }

    // 5. equivalence
    if (other_in) {
        Moore other = *other_in;
        if (other.tokens != m.net.tokens) {                   // put its columns in this network's token order
            std::vector<size_t> col;
            for (const auto& name : m.net.tokens) {
                const auto it = std::find(other.tokens.begin(), other.tokens.end(), name);
                if (it == other.tokens.end() || other.tokens.size() != T)
                    throw Fail("E_EQUIV", "the two sides do not read the same tokens: [" + join(m.net.tokens, ", ") + "] against [" + join(other.tokens, ", ") + "]");
                col.push_back((size_t)(it - other.tokens.begin()));
            }
            std::vector<int> next((size_t)other.states * T);
            for (size_t s = 0; s < (size_t)other.states; ++s)
                for (size_t t = 0; t < T; ++t) next[s * T + t] = other.next[s * T + col[t]];
            other.next = next;
            other.tokens = m.net.tokens;
        }
        const Moore mine = moore_of(m, e);
        const Equiv q = equivalence(mine, other);
        const std::string grade = q.holds == 1 ? weakest(base, other.exact ? "proved" : "checked")
                                  : q.holds == 0 ? weakest(witness, other.exact ? "proved" : "checked") : "undecided";
        JV r = JV::object();
        r.set("holds", holds_json(q.holds)).set("grade", grade).set("with", other_input ? *other_input : JV());
        r.set("other_states", other.states).set("pairs", (unsigned long long)q.pairs);
        std::string line;
        if (q.holds == 0) {
            r.set("differs_after", path_json(m, q.path));
            r.set("network_decision", q.a_emit ? "emit" : "hold").set("other_decision", q.b_emit ? "emit" : "hold");
            line = "FAILS: after the tokens [" + path_text(m, q.path) + "] " + mine.label + " decides " + (q.a_emit ? "emit" : "hold") + " and " +
                   other.label + " decides " + (q.b_emit ? "emit" : "hold") + "; no shorter sequence tells them apart";
        } else if (q.holds == 1) {
            r.set("differs_after", JV());
            line = ssprintf("%s and %s make the same decision after every token sequence (%zu reachable pairs of states; %d states of the other side)",
                            mine.label.c_str(), other.label.c_str(), q.pairs, other.states);
        } else {
            r.set("differs_after", JV());
            line = "not decided: one side's states were not all enumerated; no difference among the " + commas(q.pairs) + " pairs explored";
        }
        results.set("equivalence", r);
        fold(q.holds, grade);
        lines.push_back("equivalence  " + line + "   [" + grade + "]");
    }

    // 6. sizes
    {
        JV sz = sizes_json(m.net);
        sz.set("grade", "proved");
        results.set("sizes", sz);
        lines.push_back(ssprintf("sizes        width %d, %d neurons, %s weights (%s not zero), %s operations per step (%s dense)   [proved]", m.net.d, m.net.n,
                                 commas(m.net.weights()).c_str(), commas(m.net.nonzero()).c_str(),
                                 commas((unsigned long long)sz.get("ops_per_step")->i).c_str(),
                                 commas((unsigned long long)sz.get("ops_per_step_dense")->i).c_str()));
    }
    res.set("results", results);

    // what else the enumeration shows
    {
        size_t emits = 0, counted = 0, zero_margins = 0;
        double dmin = 1e300;
        std::vector<double> values;
        for (int i = 0; i < e.N(); ++i) {
            if (e.dirty[(size_t)i] || !decides[(size_t)i]) continue;
            ++counted;
            const double mg = e.margin[(size_t)i];
            if (mg > 0) ++emits;
            dmin = std::min(dmin, std::fabs(mg));
            const double v = snap9(mg);
            if (values.size() < 8 && std::find(values.begin(), values.end(), v) == values.end()) values.push_back(v);
            if (std::fabs(mg) <= kTol && ++zero_margins <= 3)
                warn("W_MARGIN_ZERO", "state " + state_text(m, e.state(i)) + " has margin 0: its decision (hold) rests on rounding");
        }
        std::sort(values.begin(), values.end());
        JV dj = JV::object();
        dj.set("emit_states", (unsigned long long)emits).set("hold_states", (unsigned long long)(counted - emits));
        dj.set("min_abs_margin", counted ? JV(dmin) : JV()).set("margins", JV::numbers(values));
        dj.set("note", "counted over the states that some token leads to: a decision is made after a token");
        res.set("decisions", dj);
        if (counted)
            lines.push_back(ssprintf("decisions    emit in %s state%s, hold in %s; smallest |margin| %s", commas(emits).c_str(), emits == 1 ? "" : "s",
                                     commas(counted - emits).c_str(), show(dmin).c_str()));
    }
    {
        JV rules = JV::array();
        for (size_t k = 0; k < m.rules.size(); ++k) {
            const Rule& r = m.rules[k];
            const RuleStat& st = e.rule[k];
            JV x = JV::object();
            x.set("rule", (int)k + 1).set("kind", r.kind == Rule::Tau ? "tau" : r.kind == Rule::Clear ? "clear" : "neuron");
            x.set("line", r.line ? JV(r.line) : JV()).set("text", r.text);
            if (r.kind != Rule::Clear && st.cases) {
                x.set("cases", (unsigned long long)st.cases).set("fires", (unsigned long long)st.fires);
                x.set("z_min", st.z_min).set("z_max", st.z_max);
                if (r.kind == Rule::Tau) {
                    std::vector<double> zs = st.zvals;
                    std::sort(zs.begin(), zs.end());
                    x.set("z_values", JV::numbers(zs)).set("min_distance", st.min_dist);
                }
            }
            rules.push(x);
        }
        res.set("rules", rules);
    }
    if (F) {
        JV fields = JV::array();
        std::vector<std::vector<double>> seen(F);
        for (int i = 0; i < e.N(); ++i)
            for (size_t f = 0; f < F; ++f) {
                const double v = snap9(m.value(e.state(i), (int)f));
                if (seen[f].size() < 6 && std::find(seen[f].begin(), seen[f].end(), v) == seen[f].end()) seen[f].push_back(v);
            }
        for (size_t f = 0; f < F; ++f) {
            std::sort(seen[f].begin(), seen[f].end());
            JV x = JV::object();
            x.set("name", m.fields[f]).set("values", JV::numbers(seen[f]));
            fields.push(x);
        }
        res.set("fields", fields);
        JV machines = JV::array();
        for (const auto& mc : m.machines) {
            JV reach = JV::array(), unreached = JV::array();
            for (int s = 0; s < mc.states; ++s) {
                const auto& vals = seen[(size_t)(mc.first_field + s)];
                (std::find(vals.begin(), vals.end(), 1.0) != vals.end() ? reach : unreached).push(s);
            }
            JV x = JV::object();
            x.set("name", mc.name).set("states", mc.states).set("reachable", reach).set("unreachable", unreached);
            machines.push(x);
            lines.push_back(ssprintf("machine %-4s %zu of its %d states are reachable", mc.name.c_str(), reach.arr.size(), mc.states) +
                            (unreached.arr.empty() ? std::string() : " (never reached: " + json_compact(unreached) + ")"));
        }
        res.set("machines", machines);
    }
    if ((size_t)e.N() <= kStateTable) {
        JV table = JV::array();
        for (int i = 0; i < e.N(); ++i) {
            JV x = JV::object(), nx = JV::object();
            x.set("id", i).set("fields", F ? state_json(m, e.state(i)) : JV()).set("clean", !e.dirty[(size_t)i]);
            x.set("margin", snap9(e.margin[(size_t)i])).set("decision", e.margin[(size_t)i] > 0 ? "emit" : "hold");
            x.set("path", path_json(m, e.path(i)));
            for (size_t t = 0; t < T; ++t) {
                const int j = e.next[(size_t)i * T + t];
                nx.set(m.net.tokens[t], j < 0 ? JV() : JV(j));
            }
            x.set("next", nx);
            table.push(x);
        }
        res.set("state_table", table);
    }
    if (sm.ran) {
        JV ev = JV::object(), sj = JV::object();
        sj.set("grade", "sampled").set("walks", (unsigned long long)sm.walks).set("steps_per_walk", (unsigned long long)sm.steps_per_walk);
        sj.set("steps", (unsigned long long)sm.steps).set("seed", (unsigned long long)sm.seed).set("violations", (unsigned long long)sm.hits.size());
        ev.set("states_explored", e.N()).set("exhaustive_to_depth", e.exhaustive_depth).set("sampled", sj);
        res.set("evidence", ev);
        lines.push_back(ssprintf("evidence     beyond the cap: %s random steps from the start (seed 1) found %s   [sampled]", commas(sm.steps).c_str(),
                                 sm.hits.empty() ? "no fault" : "a fault"));
    }

    // the rotated build, checked against the logic state by state
    if (build) {
        JV r = JV::object();
        r.set("seed", build->seed_known ? JV(build->seed) : JV());
        if (worst_holds != 1) {
            r.set("holds", JV()).set("grade", "undecided").set("skipped", "the logic above is not proved, so its rotated build was not examined");
            lines.push_back("rotated      not examined: the logic above did not pass");
        } else {
            const Checker cb(*build);
            const Enum b = enumerate(cb, opt.cap);
            std::string differ;
            double field_gap = 0;
            if (b.capped || b.N() != e.N()) differ = ssprintf("it reaches %d states where the logic reaches %d", b.N(), e.N());
            else if (b.ramp_total || b.dirty_total) differ = "in float64 " + (b.ramp_total ? hit_text(*build, b.ramps[0]) : hit_text(*build, b.dirties[0]));
            else if (b.next != e.next) differ = "its transitions between states differ from the logic's";
            else
                for (int i = 0; i < e.N() && differ.empty(); ++i) {
                    if ((b.margin[(size_t)i] > 0) != (e.margin[(size_t)i] > 0)) differ = "it decides differently in state " + state_text(m, e.state(i));
                    for (size_t f = 0; f < F; ++f) field_gap = std::max(field_gap, std::fabs(build->value(b.state(i), (int)f) - m.value(e.state(i), (int)f)));
                }
            if (differ.empty() && field_gap > kTol) differ = "its field values are off by " + sci(field_gap);
            double bmin = 1e300, blinf = 1e300, bl2 = 1e300;
            for (int k : cb.taus) {
                const RuleStat& st = b.rule[(size_t)k];
                if (!st.cases) continue;
                bmin = std::min(bmin, st.min_dist);
                const double* w = build->net.W1.data() + (size_t)build->rules[(size_t)k].n0 * d;
                double n1 = 0, n2 = 0;
                for (size_t j = 0; j < d; ++j) { n1 += std::fabs(w[j]); n2 += w[j] * w[j]; }
                if (n1 > 0) {
                    blinf = std::min(blinf, std::max(st.min_dist, 0.0) / n1);
                    bl2 = std::min(bl2, std::max(st.min_dist, 0.0) / std::sqrt(n2));
                }
            }
            const int holds = differ.empty() ? 1 : 0;
            const std::string grade = "checked";
            r.set("holds", holds == 1).set("grade", grade).set("states", b.N()).set("transitions", (unsigned long long)b.transitions);
            r.set("closure_residual", b.residual).set("max_field_gap", field_gap).set("min_distance", bmin < 1e299 ? JV(bmin) : JV());
            JV nz = JV::object();
            nz.set("linf", blinf < 1e299 ? JV(blinf) : JV()).set("l2", bl2 < 1e299 ? JV(bl2) : JV());
            r.set("noise", nz).set("difference", differ.empty() ? JV() : JV(differ));
            fold(holds, grade);
            if (holds)
                lines.push_back(ssprintf("rotated      the rotated float64 build%s has the same %d states and %s transitions; its states close to %s and its fields are within %s "
                                         "of the exact values; an error of up to %s on every rotated coordinate cannot move a threshold   [checked]",
                                         build->seed_known ? ssprintf(" (seed %llu)", build->seed).c_str() : "", b.N(), commas(b.transitions).c_str(),
                                         sci(b.residual).c_str(), sci(field_gap).c_str(), blinf < 1e299 ? show(blinf).c_str() : "any size"));
            else lines.push_back("rotated      FAILS: the rotated float64 build does not follow the logic: " + differ + "   [checked]");
        }
        res.set("rotated", r);
    }
    res.set("warnings", warnings);

    Proof p;
    p.status = worst_holds == 0 ? "refuted" : worst_holds < 0 ? "undecided" : "ok";
    p.grade = worst_holds == 0 ? refuting_grade : worst_holds < 0 ? "undecided" : overall;
    std::string head = "prove " + m.net.name + ": " + (p.status == "ok" ? "ok" : p.status == "refuted" ? "REFUTED" : "UNDECIDED") + ", grade " + p.grade;
    if (p.status == "ok" && build) head += " (the logic is " + base + "; its rotated float64 build is checked to 1e-9)";
    else if (p.status == "ok" && p.grade == "checked")
        head += " (enumerated in float64 to a tolerance of 1e-9: complete, but rounding keeps it short of a proof)";
    p.text = head + "\n";
    for (const auto& l : lines) p.text += "  " + l + "\n";
    for (const auto& w : warnings.arr) p.text += "  warning      " + w.get("code")->s + (w.get("line")->t == JV::Int ? ssprintf(" (line %lld)", w.get("line")->i) : std::string()) + ": " + w.get("message")->s + "\n";
    p.result = std::move(res);
    return p;
}

}  // namespace quill
