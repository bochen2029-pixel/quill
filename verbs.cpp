// quill · verbs.cpp — build, prove, run, explain, npz, examples; and how an answer is printed.
#include "app.h"
#include "npz.h"

#include <cmath>

namespace quill {

// ======================================================================
// answers
// ======================================================================
JV receipt_json(const Answer& a) {
    const std::string in = sha256_hex(json_canonical(a.input)), out = sha256_hex(json_canonical(a.result));
    JV both = JV::object();
    both.set("quill", kVersion).set("input_sha256", in).set("result_sha256", out);
    JV r = JV::object();
    r.set("id", "quill1:" + sha256_hex(json_canonical(both)).substr(0, 12)).set("input_sha256", in).set("result_sha256", out);
    return r;
}

static JV issue_json(const Issue& i) {
    JV o = JV::object();
    o.set("code", i.code).set("message", i.message).set("line", i.line ? JV(i.line) : JV()).set("column", i.column ? JV(i.column) : JV());
    o.set("source", i.source.empty() ? JV() : JV(i.source)).set("hint", i.hint.empty() ? JV() : JV(i.hint));
    return o;
}

std::string issue_text(const Issue& i) {
    std::string out;
    if (i.line) out += ssprintf("line %d", i.line) + (i.column ? ssprintf(", column %d", i.column) : std::string()) + ": ";
    out += i.message;
    if (!i.source.empty()) {
        std::string src = i.source;
        for (auto& c : src)
            if (c == '\t') c = ' ';
        out += "\n    " + src;
        if (i.column > 0) out += "\n    " + std::string((size_t)i.column - 1, ' ') + "^";
    }
    if (!i.hint.empty()) out += "\n    hint: " + i.hint;
    return out;
}

Answer error_answer(const std::string& verb, const Fail& f) {
    Answer a;
    a.verb = verb;
    a.status = "error";
    a.issues = f.issues;
    if (a.issues.empty()) {
        Issue i;
        i.code = "E_INTERNAL";
        i.message = "an error without a description";
        a.issues.push_back(i);
    }
    a.text = "quill " + verb + ": error " + a.issues[0].code + ": " + issue_text(a.issues[0]) + "\n";
    for (size_t k = 1; k < a.issues.size(); ++k) a.text += "quill " + verb + ": error " + a.issues[k].code + ": " + issue_text(a.issues[k]) + "\n";
    if (a.issues.size() > 1) a.text += ssprintf("%zu errors.\n", a.issues.size());
    return a;
}

std::string answer_json(const Answer& a) {
    JV o = JV::object();
    o.set("quill", kVersion).set("verb", a.verb).set("status", a.status).set("grade", a.grade);
    const std::string first = a.text.substr(0, a.text.find('\n'));
    if (a.status == "error") {
        JV e = issue_json(a.issues[0]);
        e.set("count", (unsigned long long)a.issues.size());
        JV all = JV::array();
        for (const auto& i : a.issues) all.push(issue_json(i));
        e.set("all", all);
        o.set("error", e).set("text", first);
    } else {
        o.set("result", a.result).set("text", first).set("receipt", receipt_json(a));
    }
    o.set("meta", a.meta);
    return json_compact(o);
}

static JV warnings_json(const std::vector<Issue>& ws) {
    JV a = JV::array();
    for (const auto& w : ws) {
        JV x = JV::object();
        x.set("code", w.code).set("line", w.line ? JV(w.line) : JV()).set("message", w.message).set("hint", w.hint.empty() ? JV() : JV(w.hint));
        a.push(x);
    }
    return a;
}
static std::string warnings_text(const std::vector<Issue>& ws) {
    std::string out;
    for (const auto& w : ws) out += "  warning " + w.code + (w.line ? ssprintf(" (line %d)", w.line) : std::string()) + ": " + w.message + (w.hint.empty() ? "" : " -- " + w.hint) + "\n";
    return out;
}

Source source_from_path(const std::string& path, int rotate_mode, unsigned long long seed) {
    Source s;
    s.path = path;
    s.rotate_mode = rotate_mode;
    s.seed = seed;
    if (is_dir(path)) return s;
    std::string err;
    if (!read_file(path, s.text, err))
        throw Fail("E_IO", "cannot read '" + path + "': " + err, "give a specification file, or a network folder (weights.npz and network.json)");
    s.is_text = true;
    return s;
}

Model load_model(const Source& s) {
    if (s.is_text) return model_from_text(s.text, s.rotate_mode, s.seed);
    if (s.rotate_mode) throw Fail("E_ARGS", "--rotate and --no-rotate apply to a specification; '" + s.path + "' is a folder that is already built");
    return model_from_folder(load_folder(s.path));
}

// ======================================================================
// build
// ======================================================================
Answer verb_build(const Source& spec, const std::string& out_dir, bool force) {
    Answer a;
    a.verb = "build";
    if (!spec.is_text) throw Fail("E_ARGS", "build takes a specification file; '" + spec.path + "' is a folder", "quill build SPEC -o DIR");
    const Model m = load_model(spec);
    const std::string npz = weights_npz(m.net), wsha = sha256_hex(npz);
    {
        Network back;                                         // what was written must read back bit for bit
        arrays_from_npz(npz, "weights.npz", back);
        if (back.E != m.net.E || back.W1 != m.net.W1 || back.b1 != m.net.b1 || back.W2 != m.net.W2 || back.r0 != m.net.r0 ||
            back.Wout != m.net.Wout || back.bout != m.net.bout)
            throw Fail("E_INTERNAL", "the weights do not read back as written");
    }
    const std::vector<std::pair<std::string, std::string>> files = {
        { "weights.npz", npz },
        { "network.json", json_pretty(network_json(m, wsha), 2) },
        { "manifest.json", json_pretty(manifest_json(m, wsha), 1) },
        { "EXPLAIN.md", explain_markdown(m) },
    };
    JV fj = JV::object();
    for (const auto& f : files) {
        JV x = JV::object();
        x.set("bytes", (unsigned long long)f.second.size()).set("sha256", sha256_hex(f.second));
        fj.set(f.first, x);
    }
    a.result.set("name", m.net.name).set("basis", m.rotated ? "rotated" : "axis").set("rotate_seed", m.rotated ? JV(m.seed) : JV());
    a.result.set("sizes", sizes_json(m.net)).set("rules", (unsigned long long)m.rules.size());
    a.result.set("fields", JV::strings(m.fields)).set("tokens", JV::strings(m.net.tokens));
    a.result.set("weights_sha256", wsha).set("files", fj).set("written", !out_dir.empty()).set("warnings", warnings_json(m.warnings));
    a.input.set("verb", "build").set("input", m.input);
    if (!out_dir.empty()) {
        if (is_file(out_dir)) throw Fail("E_EXISTS", "'" + out_dir + "' is a file; -o names a folder");
        const std::string nj = path_join(out_dir, "network.json");
        if (!force && (is_file(nj) || is_file(path_join(out_dir, "weights.npz")))) {
            bool ours = false;
            std::string text, err;
            JV old;
            if (read_file(nj, text, err) && json_parse(text, old, err) && old.t == JV::Obj && old.get("quill")) ours = true;
            if (!ours)
                throw Fail("E_EXISTS", "'" + out_dir + "' already holds a network that quill did not write; it is left untouched",
                           "choose another folder, or pass --force to overwrite it");
        }
        std::string err;
        if (!make_dirs(out_dir, err)) throw Fail("E_IO", "cannot create the folder '" + out_dir + "': " + err);
        for (const auto& f : files)
            if (!write_file(path_join(out_dir, f.first), f.second, err)) throw Fail("E_IO", "cannot write '" + path_join(out_dir, f.first) + "': " + err);
        for (const auto& f : files) {                         // read back what is on disk
            std::string got;
            if (!read_file(path_join(out_dir, f.first), got, err) || got != f.second)
                throw Fail("E_IO", "'" + path_join(out_dir, f.first) + "' does not read back as written");
        }
        a.meta.set("out", out_dir);
    }
    const JV sz = sizes_json(m.net);
    a.text = "build " + m.net.name + ": " + (out_dir.empty() ? std::string("assembled, nothing written (no -o DIR)") : "wrote " + out_dir + " (weights.npz, network.json, manifest.json, EXPLAIN.md)") + "\n";
    a.text += ssprintf("  width %d, %d neurons in %zu rules, %zu named fields, %s weights (%s not zero), %s\n", m.net.d, m.net.n, m.rules.size(), m.fields.size(),
                       commas(m.net.weights()).c_str(), commas(m.net.nonzero()).c_str(),
                       m.rotated ? ssprintf("rotated with seed %llu", m.seed).c_str() : "axis-aligned");
    a.text += "  weights.npz sha256 " + wsha + "\n";
    a.text += warnings_text(m.warnings);
    return a;
}

// ======================================================================
// prove
// ======================================================================
Answer verb_prove(const Source& src, const ProveOptions& opt, const Source* equiv) {
    Answer a;
    a.verb = "prove";
    const Model m = load_model(src);
    // A rotated network that comes with its specification is proved in two layers: the logic in the
    // field basis, where arithmetic is exact, and then the rotated float64 build against that logic.
    const bool layered = m.rotated && !m.spec_text.empty();
    Model logic_store;
    if (layered) {
        logic_store = model_from_text(m.spec_text, 1, 0);
        logic_store.input = m.input;
        logic_store.net.name = m.net.name;
    }
    const Model& logic = layered ? logic_store : m;
    Moore other;
    JV other_input;
    if (equiv) {
        other_input = JV::object();
        if (!equiv->is_text) {
            const Model om = load_model(*equiv);
            other = moore_from_model(om, opt.cap);
            other.label = "the folder's network " + om.net.name;
            other_input.set("kind", "folder").set("name", om.net.name).set("input", om.input);
        } else {
            const size_t first = equiv->text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
            if (first != std::string::npos && equiv->text[first] == '{') {
                JV j;
                std::string err;
                if (!json_parse(equiv->text, j, err)) throw Fail("E_EQUIV", "the machine file is not valid JSON: " + err);
                other = moore_from_machine(j, logic.net.tokens);
                other_input.set("kind", "machine").set("name", other.label.substr(12)).set("sha256", sha256_hex(equiv->text));
            } else {
                const Model om = model_from_text(equiv->text, 1, 0);      // its logic: rotation does not change decisions
                other = moore_from_model(om, opt.cap);
                other.label = "the specification " + om.net.name;
                other_input.set("kind", "specification").set("name", om.net.name).set("sha256", sha256_hex(equiv->text));
            }
        }
    }
    const double t0 = now_ms();
    const Proof p = prove(logic, layered ? &m : nullptr, opt, equiv ? &other : nullptr, equiv ? &other_input : nullptr);
    a.meta.set("ms", now_ms() - t0);
    a.status = p.status;
    a.grade = p.grade;
    a.result = p.result;
    a.text = p.text;
    JV options = JV::object();
    options.set("cap", (unsigned long long)opt.cap);
    a.input.set("verb", "prove").set("input", m.input).set("options", options).set("equiv", equiv ? other_input : JV());
    return a;
}

// ======================================================================
// run
// ======================================================================
Answer verb_run(const Source& src, const RunOptions& o) {
    Answer a;
    a.verb = "run";
    const Model m = load_model(src);
    const Network& net = m.net;
    const Kernel K(net);
    const size_t d = (size_t)net.d, dp = (size_t)K.padded(), T = (size_t)net.T();
    if ((int)o.have_tokens + (int)o.have_file + (int)o.have_random != 1)
        throw Fail("E_ARGS", "run needs the tokens from exactly one of --tokens, --tokens-file and --random",
                   "for example: quill run DIR --tokens \"zero one one\"");
    // the token sequence
    std::vector<int32_t> toks;
    if (o.have_tokens) {
        size_t i = 0;
        const std::string& s = o.tokens;
        while (i < s.size()) {
            if (strchr(" ,\t\r\n", s[i])) { ++i; continue; }
            size_t j = i;
            while (j < s.size() && !strchr(" ,\t\r\n", s[j])) ++j;
            const std::string item = s.substr(i, j - i);
            int at = -1;
            for (size_t t = 0; t < T; ++t)
                if (net.tokens[t] == item) at = (int)t;
            unsigned long long pos = 0;
            if (at < 0 && to_u64(item, pos) && pos < T) at = (int)pos;
            if (at < 0)
                throw Fail("E_ARGS", "'" + item + "' is not a token of this network. Its tokens are: " + join(net.tokens, " ") +
                                         ssprintf(" (or their positions 0 to %zu)", T - 1));
            toks.push_back(at);
            i = j;
        }
    } else if (o.have_file) {
        if (o.token_bytes.size() % 4) throw Fail("E_FORMAT", ssprintf("the token file has %zu bytes, which is not a whole number of int32 values", o.token_bytes.size()));
        toks.resize(o.token_bytes.size() / 4);
        if (!toks.empty()) memcpy(toks.data(), o.token_bytes.data(), o.token_bytes.size());
        for (size_t k = 0; k < toks.size(); ++k)
            if (toks[k] < 0 || (size_t)toks[k] >= T)
                throw Fail("E_FORMAT", ssprintf("value %zu of the token file is %d; this network has %zu tokens (0 to %zu)", k, toks[k], T, T - 1));
    }
    const unsigned long long base = o.have_random ? o.random_n : (unsigned long long)toks.size();
    if (o.repeat < 1 || (base && o.repeat > (1ull << 40) / base)) throw Fail("E_ARGS", "--repeat times the token count is out of range (at most 2^40 steps)");
    const unsigned long long steps = base * o.repeat;
    // the start state
    std::vector<double> state(dp, 0.0);
    K.load(net.r0.data(), state.data());
    std::string start = "r0";
    if (o.have_start) {
        const size_t row_bytes = 4 * d;
        if (o.start_bytes.empty() || o.start_bytes.size() % row_bytes)
            throw Fail("E_FORMAT", ssprintf("the start file has %zu bytes; it must hold float32 rows of %zu values (%zu bytes each)", o.start_bytes.size(), d, row_bytes));
        const size_t rows = o.start_bytes.size() / row_bytes;
        if (o.start_row >= rows) throw Fail("E_ARGS", ssprintf("--start-row %llu is out of range: the start file has %zu rows (0 to %zu)", o.start_row, rows, rows - 1));
        std::vector<float> f32(d);
        memcpy(f32.data(), o.start_bytes.data() + (size_t)o.start_row * row_bytes, row_bytes);
        std::vector<double> r(d);
        for (size_t j = 0; j < d; ++j) r[j] = f32[j];
        K.load(r.data(), state.data());
        start = "file";
    }
    OutFile dump, margins;
    std::string err;
    if (!o.dump_path.empty() && !dump.open(o.dump_path, err)) throw Fail("E_IO", "cannot write '" + o.dump_path + "': " + err);
    if (!o.margins_path.empty() && !margins.open(o.margins_path, err)) throw Fail("E_IO", "cannot write '" + o.margins_path + "': " + err);
    const bool dumping = !o.dump_path.empty();
    const size_t chunk = dumping ? 4096 : 65536;
    std::vector<int32_t> tbuf(chunk);
    std::vector<double> mbuf(chunk), sbuf(dumping ? chunk * dp : 0);
    std::vector<float> fbuf(dumping ? chunk * d : 0);
    std::string dbuf(chunk, '0');
    Sha256 h_tokens, h_margins, h_decisions;
    Rng rng(o.seed);
    unsigned long long done = 0, emits = 0;
    double elapsed_ms = 0, mmin = 1e300, mmax = -1e300;
    JV rows = JV::array();
    std::string row_text;
    size_t name_w = 5;
    for (const auto& tn : net.tokens) name_w = std::max(name_w, tn.size());
    while (done < steps) {
        const size_t n = (size_t)std::min<unsigned long long>(chunk, steps - done);
        for (size_t k = 0; k < n; ++k) {
            if (!o.have_random) {
                tbuf[k] = toks[(size_t)((done + k) % base)];
                continue;
            }
            if ((done + k) % base == 0) rng = Rng(o.seed);        // each repetition draws the same N tokens again
            tbuf[k] = (int32_t)rng.below((uint32_t)T);
        }
        double* st = state.data();
        const double t0 = now_ms();
        if (dumping)
            for (size_t k = 0; k < n; ++k) {
                K.step(st, tbuf[k]);
                mbuf[k] = K.margin(st);
                memcpy(sbuf.data() + k * dp, st, dp * sizeof(double));
            }
        else
            for (size_t k = 0; k < n; ++k) {
                K.step(st, tbuf[k]);
                mbuf[k] = K.margin(st);
            }
        elapsed_ms += now_ms() - t0;
        for (size_t k = 0; k < n; ++k) {
            const double mg = mbuf[k];
            const bool emit = mg > 0;
            dbuf[k] = emit ? '1' : '0';
            emits += emit;
            mmin = std::min(mmin, mg);
            mmax = std::max(mmax, mg);
            if (done + k < o.show) {
                JV r = JV::object();
                r.set("step", (unsigned long long)(done + k)).set("token", net.tokens[(size_t)tbuf[k]]).set("margin", mg).set("decision", emit ? "emit" : "hold");
                rows.push(r);
                row_text += ssprintf("  %6llu  %-*s  %-12s  %s\n", done + k, (int)name_w, net.tokens[(size_t)tbuf[k]].c_str(), show(mg).c_str(), emit ? "emit" : "hold");
            }
        }
        h_tokens.update(tbuf.data(), n * 4);
        h_margins.update(mbuf.data(), n * 8);
        h_decisions.update(dbuf.data(), n);
        if (dumping) {
            for (size_t k = 0; k < n; ++k)
                for (size_t j = 0; j < d; ++j) fbuf[k * d + j] = (float)sbuf[k * dp + j];
            if (!dump.write(fbuf.data(), n * d * 4, err)) throw Fail("E_IO", "cannot write '" + o.dump_path + "': " + err);
        }
        if (!o.margins_path.empty() && !margins.write(mbuf.data(), n * 8, err)) throw Fail("E_IO", "cannot write '" + o.margins_path + "': " + err);
        done += n;
    }
    dump.close();
    margins.close();
    const double seconds = elapsed_ms / 1000.0;
    const bool timed = seconds >= 0.001;                      // under a millisecond the clock says more about itself than about the kernel
    a.result.set("name", net.name).set("steps", steps).set("emit", emits).set("hold", steps - emits);
    a.result.set("margin_min", steps ? JV(mmin) : JV()).set("margin_max", steps ? JV(mmax) : JV());
    a.result.set("start", start).set("shown", (unsigned long long)rows.arr.size()).set("rows", rows);
    a.result.set("final_state", m.F() ? state_json(m, state.data()) : JV());
    a.result.set("final_state_sha256", sha256_hex(std::string_view((const char*)state.data(), d * 8)));
    a.result.set("tokens_sha256", h_tokens.hex()).set("margins_sha256", h_margins.hex()).set("decisions_sha256", h_decisions.hex());
    JV options = JV::object();
    options.set("repeat", o.repeat).set("random", o.have_random ? JV(o.random_n) : JV()).set("seed", o.have_random ? JV(o.seed) : JV());
    options.set("start", o.have_start ? JV(sha256_hex(o.start_bytes)) : JV()).set("start_row", o.have_start ? JV(o.start_row) : JV()).set("show", o.show);
    a.input.set("verb", "run").set("input", m.input).set("options", options);
    a.meta.set("seconds", seconds).set("steps_per_second", timed ? JV(std::floor((double)steps / seconds)) : JV());
    a.meta.set("kernel", K.sparse() ? "sparse" : "dense");
    if (dumping) a.meta.set("dump", o.dump_path);
    if (!o.margins_path.empty()) a.meta.set("margins", o.margins_path);
    a.text = ssprintf("run %s: %s step%s, %s emit, %s hold", net.name.c_str(), commas(steps).c_str(), steps == 1 ? "" : "s", commas(emits).c_str(), commas(steps - emits).c_str());
    if (steps) a.text += ", margins from " + show(mmin) + " to " + show(mmax);
    a.text += "\n";
    if (!rows.arr.empty()) {
        a.text += ssprintf("    step  %-*s  %-12s  decision\n", (int)name_w, "token", "margin") + row_text;
        if (steps > rows.arr.size()) a.text += ssprintf("  ... %s more steps not listed (--show N lists more)\n", commas(steps - rows.arr.size()).c_str());
    }
    if (m.F()) a.text += "  final state " + state_text(m, state.data()) + "\n";
    a.text += timed ? ssprintf("  %s steps per second (%s steps in %.6f s, one thread, %s kernel)\n", commas((unsigned long long)((double)steps / seconds)).c_str(),
                               commas(steps).c_str(), seconds, K.sparse() ? "sparse" : "dense")
                    : ssprintf("  speed not measured: %s step%s took under a millisecond (--repeat K runs the sequence K times over)\n", commas(steps).c_str(),
                               steps == 1 ? "" : "s");
    if (dumping) a.text += "  states written to " + o.dump_path + ssprintf(" (float32, %zu values per step)\n", d);
    if (!o.margins_path.empty()) a.text += "  margins written to " + o.margins_path + " (float64, one per step)\n";
    return a;
}

// ======================================================================
// explain, npz, examples
// ======================================================================
Answer verb_explain(const Source& src) {
    Answer a;
    a.verb = "explain";
    const Model m = load_model(src);
    a.result = explain_json(m);
    a.text = explain_markdown(m);
    a.result.set("markdown", a.text);
    a.input.set("verb", "explain").set("input", m.input);
    return a;
}

Answer verb_npz(const std::string& path) {
    Answer a;
    a.verb = "npz";
    const std::string file = slurp(path, "the file");
    const std::string label = "the file";
    JV arrays = JV::array();
    a.text = ssprintf("npz: %s bytes, sha256 %s\n", commas(file.size()).c_str(), sha256_hex(file).c_str());
    for (const auto& e : zip_list(file, label)) {
        JV x = JV::object();
        x.set("entry", e.name).set("compression", e.method == 0 ? "stored" : e.method == 8 ? "deflate" : "other");
        x.set("bytes", (unsigned long long)e.usize).set("stored_bytes", (unsigned long long)e.csize);
        if (e.name.size() > 4 && e.name.compare(e.name.size() - 4, 4, ".npy") == 0) {
            const std::string raw = zip_extract(file, e, label);
            const NpyArray arr = npy_parse(raw, e.name.substr(0, e.name.size() - 4), label);
            const std::vector<double> v = npy_f64(arr, label);
            JV shape = JV::array(), head = JV::array();
            for (size_t s : arr.shape) shape.push((unsigned long long)s);
            double lo = 1e300, hi = -1e300;
            size_t nz = 0;
            for (size_t k = 0; k < v.size(); ++k) {
                if (k < 4) head.push(v[k]);
                lo = std::min(lo, v[k]);
                hi = std::max(hi, v[k]);
                if (v[k] != 0) ++nz;
            }
            x.set("array", arr.name).set("dtype", arr.descr).set("shape", shape).set("fortran_order", arr.fortran);
            x.set("values", (unsigned long long)v.size()).set("nonzero", (unsigned long long)nz);
            x.set("min", v.empty() ? JV() : JV(lo)).set("max", v.empty() ? JV() : JV(hi)).set("first", head);
            x.set("data_sha256", sha256_hex(arr.data));
            a.text += ssprintf("  %-12s %-4s %-14s %-7s %8zu values, %zu not zero", arr.name.c_str(), arr.descr.c_str(), arr.shape_text().c_str(),
                               e.method == 8 ? "deflate" : "stored", v.size(), nz);
            if (!v.empty()) a.text += ", from " + show(lo) + " to " + show(hi);
            a.text += "\n";
        } else {
            a.text += ssprintf("  %-12s (not an array, %llu bytes)\n", e.name.c_str(), (unsigned long long)e.usize);
        }
        arrays.push(x);
    }
    a.result.set("sha256", sha256_hex(file)).set("bytes", (unsigned long long)file.size()).set("entries", arrays);
    JV in = JV::object();
    in.set("kind", "npz").set("sha256", sha256_hex(file));
    a.input.set("verb", "npz").set("input", in);
    return a;
}

Answer verb_examples(const std::string& name, const std::string& write_dir) {
    Answer a;
    a.verb = "examples";
    a.input.set("verb", "examples").set("name", name.empty() ? JV() : JV(name));
    if (!write_dir.empty()) {
        std::string err;
        if (!make_dirs(write_dir, err)) throw Fail("E_IO", "cannot create the folder '" + write_dir + "': " + err);
        JV files = JV::array();
        size_t wrote = 0;
        for (const auto& ex : examples()) {
            if (!name.empty() && name != ex.name) continue;
            const std::string path = path_join(write_dir, ex.file);
            std::string old;
            if (is_file(path)) {
                if (!read_file(path, old, err) || old != ex.text)
                    throw Fail("E_EXISTS", "'" + path + "' exists and differs from the example; it is left untouched", "remove it first, or write to another folder");
            } else if (!write_file(path, ex.text, err)) throw Fail("E_IO", "cannot write '" + path + "': " + err);
            else ++wrote;
            files.push(ex.file);
        }
        if (files.arr.empty()) throw Fail("E_ARGS", "there is no example named '" + name + "'", "quill examples lists them");
        a.result.set("files", files);
        a.meta.set("dir", write_dir);
        a.text = ssprintf("examples: %zu files in %s (%zu written, %zu already there and identical)\n", files.arr.size(), write_dir.c_str(), wrote, files.arr.size() - wrote);
        return a;
    }
    if (!name.empty()) {
        const Example* ex = example(name);
        if (!ex) {
            std::vector<std::string> names;
            for (const auto& x : examples()) names.push_back(x.name);
            const std::string near = nearest(name, names);
            throw Fail("E_ARGS", "there is no example named '" + name + "'. The examples are: " + join(names, " "), near.empty() ? "" : "did you mean '" + near + "'?");
        }
        a.result.set("name", ex->name).set("file", ex->file).set("about", ex->about).set("sha256", sha256_hex(ex->text)).set("text", ex->text);
        a.text = ex->text;
        return a;
    }
    JV list = JV::array();
    a.text = "examples: the specifications inside quill.exe (quill examples NAME prints one; --write DIR writes them)\n";
    for (const auto& ex : examples()) {
        JV x = JV::object();
        x.set("name", ex.name).set("file", ex.file).set("about", ex.about).set("sha256", sha256_hex(ex.text));
        list.push(x);
        a.text += ssprintf("  %-22s %s\n", ex.name, ex.about);
    }
    a.result.set("examples", list);
    return a;
}

}  // namespace quill
