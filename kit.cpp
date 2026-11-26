// quill · kit.cpp — check-kit: reproduce a planted-kit article's dump from its weights alone.
// The layout is the kit's (kit/build.py, kit/selftest.py): tokens.i32 holds sessions x rows tokens,
// session_start.f32 one float32 start state per session, probe_hidden.f32 the float32 state after
// every token, probe_dump.jsonl one logged row per token.
#include "app.h"

#include <cmath>
#include <memory>

namespace quill {

void q8_row(const float* row, int width, double* out) {
    for (int a = 0; a < width; a += 32) {
        const int b = std::min(width, a + 32);
        double largest = 0;
        for (int j = a; j < b; ++j) largest = std::max(largest, std::fabs((double)row[j]));
        double scale = largest / 127.0;
        if (scale == 0) scale = 1.0;
        for (int j = a; j < b; ++j) out[j] = std::nearbyint((double)row[j] / scale) * scale;     // half to even, as numpy rounds
    }
}

namespace {

long long meta_int(const JV& meta, const char* key, long long dflt) {
    const JV* v = meta.get(key);
    return v && v->t == JV::Int ? v->i : dflt;
}

struct Check {
    std::string id, what;
    double value = 0, limit = 0;
    bool ok = false;
    bool counted = false;        // value and limit are counts, not differences
};

}  // namespace

Answer verb_check_kit(const std::string& dir, const Source* spec) {
    Answer a;
    a.verb = "check-kit";
    const Folder f = load_folder(dir);
    const Network& net = f.net;
    const size_t d = (size_t)net.d, T = (size_t)net.T();
    const auto need = [&](const char* name) { return slurp(path_join(dir, name), name); };
    const std::string meta_text = need("probe_dump.meta.json"), tok_bytes = need("tokens.i32"), start_bytes = need("session_start.f32"),
                      hid_bytes = need("probe_hidden.f32"), dump_text = need("probe_dump.jsonl");
    JV meta;
    std::string err;
    if (!json_parse(meta_text, meta, err) || meta.t != JV::Obj) throw Fail("E_FORMAT", "probe_dump.meta.json: " + (err.empty() ? std::string("not a JSON object") : err));
    const long long rows_ll = meta_int(meta, "rows", -1), sessions_ll = meta_int(meta, "sessions", -1);
    if (rows_ll <= 0 || sessions_ll <= 0 || rows_ll % sessions_ll)
        throw Fail("E_FORMAT", "probe_dump.meta.json must give rows and sessions, rows a multiple of sessions");
    const size_t rows = (size_t)rows_ll, sessions = (size_t)sessions_ll, per = rows / sessions;
    if (meta_int(meta, "n_embd", (long long)d) != (long long)d)
        throw Fail("E_FORMAT", ssprintf("probe_dump.meta.json says n_embd = %lld but the network's width is %zu", meta_int(meta, "n_embd", 0), d));
    if (meta_int(meta, "rows_per_session", (long long)per) != (long long)per)
        throw Fail("E_FORMAT", "probe_dump.meta.json: rows_per_session is not rows divided by sessions");
    const auto sized = [&](const std::string& bytes, size_t want, const char* name, const char* what) {
        if (bytes.size() != want) throw Fail("E_FORMAT", ssprintf("%s has %zu bytes; %zu are needed (%s)", name, bytes.size(), want, what));
    };
    sized(tok_bytes, rows * 4, "tokens.i32", "one int32 per row");
    sized(start_bytes, sessions * d * 4, "session_start.f32", "one float32 state per session");
    sized(hid_bytes, rows * d * 4, "probe_hidden.f32", "one float32 state per row");
    std::vector<int32_t> toks(rows);
    memcpy(toks.data(), tok_bytes.data(), rows * 4);
    for (size_t r = 0; r < rows; ++r)
        if (toks[r] < 0 || (size_t)toks[r] >= T) throw Fail("E_FORMAT", ssprintf("tokens.i32: value %zu is %d; the network has %zu tokens", r, toks[r], T));
    std::vector<float> start(sessions * d), hid(rows * d);
    memcpy(start.data(), start_bytes.data(), start_bytes.size());
    memcpy(hid.data(), hid_bytes.data(), hid_bytes.size());
    // the logged rows
    std::vector<double> log_margin(rows), log_re(rows);
    {
        size_t at = 0, row = 0;
        while (at < dump_text.size()) {
            size_t nl = dump_text.find('\n', at);
            if (nl == std::string::npos) nl = dump_text.size();
            const std::string_view line = std::string_view(dump_text).substr(at, nl - at);
            at = nl + 1;
            if (trim(line).empty()) continue;
            if (row >= rows) throw Fail("E_FORMAT", ssprintf("probe_dump.jsonl has more than the %zu rows that probe_dump.meta.json states", rows));
            JV j;
            if (!json_parse(line, j, err)) throw Fail("E_FORMAT", ssprintf("probe_dump.jsonl, row %zu: %s", row, err.c_str()));
            const JV* mg = j.get("margin");
            const JV* le = j.get("l_emit_re");
            const JV* lh = j.get("l_hold_re");
            if (!mg || !le || !lh || !mg->is_num() || !le->is_num() || !lh->is_num())
                throw Fail("E_FORMAT", ssprintf("probe_dump.jsonl, row %zu: margin, l_emit_re and l_hold_re must be numbers", row));
            log_margin[row] = mg->d;
            log_re[row] = le->d - lh->d;
            ++row;
        }
        if (row != rows) throw Fail("E_FORMAT", ssprintf("probe_dump.jsonl has %zu rows; probe_dump.meta.json states %zu", row, rows));
    }
    // the float32 readout the kit logs with
    std::vector<double> w32(2 * d);
    for (size_t k = 0; k < 2 * d; ++k) w32[k] = (double)(float)net.Wout[k];
    int readout_file = -1;                                    // -1 absent, 0 differs, 1 identical
    {
        std::string rb, e2;
        if (read_file(path_join(dir, "readout.f32"), rb, e2)) {
            readout_file = rb.size() == 2 * d * 4;
            for (size_t k = 0; k < 2 * d && readout_file == 1; ++k) {
                float v;
                memcpy(&v, rb.data() + 4 * k, 4);
                if (v != (float)net.Wout[k]) readout_file = 0;
            }
        }
    }
    // an optional specification, run beside the article
    Model sm;
    std::vector<double> sstate;
    if (spec) {
        sm = load_model(*spec);
        if (sm.net.tokens != net.tokens)
            throw Fail("E_ARGS", "the specification's tokens [" + join(sm.net.tokens, ", ") + "] are not the article's [" + join(net.tokens, ", ") + "]");
    }
    const Kernel K(net);
    const size_t dp = (size_t)K.padded();
    std::vector<double> state(dp), q(d), r0f(d);
    std::vector<float> row32(d);
    double hid_max = 0, re_max = 0, re_file_max = 0, q8_run_max = 0, q8_file_max = 0;
    size_t identical = 0, r0_sessions = 0, agree = 0, q8_run_off = 0;
    long long first_differ = -1;
    const std::unique_ptr<Kernel> SK(spec ? new Kernel(sm.net) : nullptr);
    if (spec) sstate.assign((size_t)SK->padded(), 0.0);
    for (size_t s = 0; s < sessions; ++s) {
        // The dump was made from a float64 start. For every article but the session one that start is r0,
        // and session_start.f32 is its float32 copy: use r0 itself wherever the copy matches bit for bit.
        bool is_r0 = true;
        for (size_t j = 0; j < d && is_r0; ++j) is_r0 = (float)net.r0[j] == start[s * d + j];
        for (size_t j = 0; j < d; ++j) r0f[j] = is_r0 ? net.r0[j] : (double)start[s * d + j];
        r0_sessions += is_r0;
        K.load(r0f.data(), state.data());
        if (spec) SK->load(sm.net.r0.data(), sstate.data());
        for (size_t i = 0; i < per; ++i) {
            const size_t row = s * per + i;
            K.step(state.data(), toks[row]);
            const float* h = hid.data() + row * d;
            for (size_t j = 0; j < d; ++j) {
                row32[j] = (float)state[j];
                hid_max = std::max(hid_max, std::fabs(state[j] - (double)h[j]));
                identical += row32[j] == h[j];
            }
            const double margin = K.margin(state.data());
            re_max = std::max(re_max, std::fabs(margin - log_re[row]));
            const auto readout = [&](const double* x) {         // as the kit: two rows, then their difference
                double hold = 0, emit = 0;
                for (size_t j = 0; j < d; ++j) {
                    hold += x[j] * w32[j];
                    emit += x[j] * w32[d + j];
                }
                return emit - hold;
            };
            for (size_t j = 0; j < d; ++j) q[j] = (double)h[j];
            re_file_max = std::max(re_file_max, std::fabs(readout(q.data()) - log_re[row]));
            q8_row(h, (int)d, q.data());
            q8_file_max = std::max(q8_file_max, std::fabs(readout(q.data()) - log_margin[row]));
            q8_row(row32.data(), (int)d, q.data());
            const double gap = std::fabs(readout(q.data()) - log_margin[row]);
            q8_run_max = std::max(q8_run_max, gap);
            q8_run_off += gap > 1e-9;
            if (spec) {
                SK->step(sstate.data(), toks[row]);
                const bool same = (SK->margin(sstate.data()) > 0) == (margin > 0);
                agree += same;
                if (!same && first_differ < 0) first_differ = (long long)row;
            }
        }
    }
    std::vector<Check> checks;
    const auto diff = [&](const char* id, const char* what, double value, double limit) {
        Check c;
        c.id = id;
        c.what = what;
        c.value = value;
        c.limit = limit;
        c.ok = value <= limit;
        checks.push_back(c);
    };
    diff("hidden", "largest gap between quill's float64 state and probe_hidden.f32", hid_max, 1e-6);
    diff("margin_re", "largest gap between quill's float64 margin and l_emit_re - l_hold_re", re_max, 1e-4);
    diff("margin_q8", "largest gap between the 8-bit margin of quill's own float32 states and the logged margin", q8_run_max, 1e-4);
    diff("margin_q8_file", "largest gap between the 8-bit margin of probe_hidden.f32 and the logged margin", q8_file_max, 1e-9);
    diff("margin_re_file", "largest gap between the float32 readout of probe_hidden.f32 and l_emit_re - l_hold_re", re_file_max, 1e-9);
    if (readout_file >= 0) {
        Check c;
        c.id = "readout_f32";
        c.what = "readout.f32 is the float32 copy of Wout";
        c.value = readout_file;
        c.limit = 1;
        c.ok = readout_file == 1;
        c.counted = true;
        checks.push_back(c);
    }
    if (spec) {
        Check c;
        c.id = "spec_decisions";
        c.what = "rows on which the specification " + sm.net.name + ", built axis-aligned, decides as the article does";
        c.value = (double)agree;
        c.limit = (double)rows;
        c.ok = agree == rows;
        c.counted = true;
        checks.push_back(c);
    }
    bool all = true;
    JV cj = JV::array();
    for (const auto& c : checks) {
        all = all && c.ok;
        JV x = JV::object();
        x.set("id", c.id).set("what", c.what).set("value", c.counted ? JV((unsigned long long)c.value) : JV(c.value));
        x.set(c.counted ? "of" : "limit", c.counted ? JV((unsigned long long)c.limit) : JV(c.limit)).set("ok", c.ok);
        cj.push(x);
    }
    a.status = all ? "ok" : "refuted";
    a.grade = "checked";
    JV st = JV::object();
    st.set("from_r0", (unsigned long long)r0_sessions).set("from_session_start_f32", (unsigned long long)(sessions - r0_sessions));
    a.result.set("name", net.name).set("rows", (unsigned long long)rows).set("sessions", (unsigned long long)sessions);
    a.result.set("rows_per_session", (unsigned long long)per).set("width", net.d).set("neurons", net.n).set("start_states", st);
    a.result.set("float32_identical", (unsigned long long)identical).set("float32_values", (unsigned long long)(rows * d));
    a.result.set("margin_q8_rows_off", (unsigned long long)q8_run_off).set("checks", cj);
    if (spec) {
        JV sj = JV::object();
        sj.set("name", sm.net.name).set("sha256", sha256_hex(spec->text)).set("agree", (unsigned long long)agree).set("rows", (unsigned long long)rows);
        sj.set("first_difference_row", first_differ < 0 ? JV() : JV(first_differ));
        a.result.set("spec", sj);
    }
    JV in = JV::object();
    in.set("kind", "article").set("weights_sha256", f.weights_sha256).set("network_sha256", f.network_sha256);
    in.set("tokens_sha256", sha256_hex(tok_bytes)).set("session_start_sha256", sha256_hex(start_bytes));
    in.set("probe_hidden_sha256", sha256_hex(hid_bytes)).set("probe_dump_sha256", sha256_hex(dump_text)).set("meta_sha256", sha256_hex(meta_text));
    a.input.set("verb", "check-kit").set("input", in).set("spec", spec ? JV(sha256_hex(spec->text)) : JV());
    a.text = ssprintf("check-kit %s: %s, grade checked: %s rows in %zu sessions; hidden %s, margin %s, 8-bit margin %s", net.name.c_str(),
                      all ? "ok" : "REFUTED", commas(rows).c_str(), sessions, sci(hid_max).c_str(), sci(re_max).c_str(), sci(q8_run_max).c_str());
    if (spec) a.text += ssprintf("; specification %s decides alike on %s of %s rows", sm.net.name.c_str(), commas(agree).c_str(), commas(rows).c_str());
    a.text += "\n";
    for (const auto& c : checks)
        a.text += c.counted ? ssprintf("  %-4s %-15s %s: %s of %s\n", c.ok ? "ok" : "FAIL", c.id.c_str(), c.what.c_str(), commas((unsigned long long)c.value).c_str(),
                                       commas((unsigned long long)c.limit).c_str())
                            : ssprintf("  %-4s %-15s %s: %s (limit %s)\n", c.ok ? "ok" : "FAIL", c.id.c_str(), c.what.c_str(), sci(c.value).c_str(), sci(c.limit).c_str());
    a.text += ssprintf("  start states: %zu of %zu sessions start from r0 (their session_start.f32 row is its float32 copy); %zu from session_start.f32\n",
                       r0_sessions, sessions, sessions - r0_sessions);
    a.text += ssprintf("  %s of %s float32 state values are bit-identical to probe_hidden.f32; the 8-bit margin of quill's own run is off by more than 1e-9 on %s rows\n",
                       commas(identical).c_str(), commas(rows * d).c_str(), commas(q8_run_off).c_str());
    if (first_differ >= 0) a.text += ssprintf("  the specification first decides differently at row %lld\n", first_differ);
    return a;
}

}  // namespace quill
