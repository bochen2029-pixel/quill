// quill — write, prove and run small neural networks whose every weight is set by formula.
// A specification in named fields and threshold rules goes in; explicit weights with a line of
// meaning per neuron, an exhaustive proof that the network is exact, and a fast runner come out.
// C++20, operating-system APIs only, one executable.
//
// Build: build.bat (MSVC, /std:c++20 /W4 /WX /permissive- /utf-8 /MT /fp:strict) -> quill.exe
#include "app.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <map>
#include <new>

namespace quill {

static const char* const kHelp = R"help(quill %s - write, prove and run small neural networks whose every weight is set by formula

USAGE
  quill build SPEC -o DIR        write the network folder (weights.npz, network.json, manifest.json)
                                 and EXPLAIN.md: the field layout and a line of meaning per neuron
  quill prove SPEC|DIR           enumerate every reachable state and prove the network exact
  quill run SPEC|DIR --tokens "zero one one"      run it: margins, decisions, steps per second
  quill explain SPEC|DIR         print the explanation (fields, tokens, rules, neurons, readout)
  quill check-kit DIR            reproduce a planted-kit article's dump from its weights
  quill examples [NAME]          the specifications inside quill.exe (--write DIR writes them)
  quill npz FILE                 list the arrays of a numpy .npz file
  quill --selftest               every claim of this tool, checked from the inside
  quill --about                  a short card as JSON: verbs, MCP registration, health
  quill --mcp                    MCP server on stdio: quill_build, quill_prove, quill_run, quill_explain
  quill --help | --version

  SPEC is a specification file; DIR is a network folder in the planted kit's format.
  Every verb takes --json and then prints exactly one JSON object on one line.

OPTIONS
  build     -o DIR           where to write; without it nothing is written
            --force          overwrite a folder that quill did not write
  prove     --equiv FILE     also prove the same decisions as FILE on every token sequence;
                             FILE is a machine (JSON), another SPEC, or another DIR
            --cap N          stop, undecided, beyond N reachable states (default 200000)
            --states         list the reachable states (at most 64)
  run       --tokens "..."   token names or positions, separated by spaces or commas
            --tokens-file F  int32 little-endian token positions
            --random N       N uniform random tokens (--seed S, default 1)
            --repeat K       the token sequence K times over, the state carried on
            --start F.f32    start from a float32 row of `width` values (--start-row R, default 0)
            --dump OUT.f32   write the state after every token, float32
            --margins OUT.f64   write every margin, float64
            --show N         list the first N steps (default 16)
  check-kit --spec SPEC      also compare SPEC's decisions with the article's on every token
  build, prove, run, explain on a SPEC:
            --rotate SEED    build in a rotated basis with this seed, whatever the SPEC says
            --no-rotate      build axis-aligned, whatever the SPEC says

)help";

static const char* const kHelpTail = R"help(
GRADES   proved     the states close bit for bit: true for every token sequence of any length
                    (with exact arithmetic nothing at all is tolerated: a z 2^-40 inside (0, 1) fails)
         checked    the arithmetic rounds: verified to 1e-9 in float64, merging a near state only
                    where the gap can move no threshold; or checked against given data
         sampled    verified on random token sequences only (seed and count stated)
         undecided  the enumeration stopped at the cap before closing, or a rotated build did not close

EXIT CODES   0 ok   1 refuted (a proof or a check failed)   2 error (bad input)   3 undecided
)help";

static void print_help() {
    printf(kHelp, kVersion);
    fputs(language_card(), stdout);
    fputs(kHelpTail, stdout);
}

namespace {

struct Flag {
    const char* name;
    bool value;
};

struct Args {
    std::vector<std::string> pos;
    std::map<std::string, std::string> opt;
    bool has(const char* k) const { return opt.count(k) != 0; }
    std::string get(const char* k) const {
        const auto it = opt.find(k);
        return it == opt.end() ? std::string() : it->second;
    }
};

Args parse_args(const std::string& verb, const std::vector<std::string>& argv, size_t from, const std::vector<Flag>& flags) {
    Args a;
    for (size_t i = from; i < argv.size(); ++i) {
        std::string k = argv[i];
        if (k.size() < 2 || k[0] != '-' || (k[1] != '-' && k != "-o" && k != "-j")) {
            a.pos.push_back(k);
            continue;
        }
        if (k == "-o") k = "--out";
        if (k == "-j") k = "--json";
        const Flag* f = nullptr;
        for (const auto& x : flags)
            if (k == x.name) f = &x;
        if (!f) {
            std::vector<std::string> known;
            for (const auto& x : flags) known.push_back(x.name);
            const std::string guess = nearest(k, known);      // (windows.h takes the word `near`)
            throw Fail("E_ARGS", "unknown option '" + k + "' for quill " + verb + ". Its options are: " + join(known, " "),
                       guess.empty() ? "quill --help lists everything" : "did you mean '" + guess + "'?");
        }
        if (a.opt.count(k)) throw Fail("E_ARGS", "the option '" + k + "' is given twice");
        if (f->value) {
            if (i + 1 >= argv.size()) throw Fail("E_ARGS", "the option '" + k + "' needs a value");
            a.opt[k] = argv[++i];
        } else a.opt[k] = "1";
    }
    return a;
}

unsigned long long number(const Args& a, const char* key, unsigned long long dflt, unsigned long long lo, unsigned long long hi) {
    if (!a.has(key)) return dflt;
    unsigned long long v = 0;
    if (!to_u64(a.get(key), v) || v < lo || v > hi)
        throw Fail("E_ARGS", ssprintf("%s takes a whole number from %llu to %llu, not '%s'", key, lo, hi, a.get(key).c_str()));
    return v;
}

void need_positional(const std::string& verb, const Args& a, size_t lo, size_t hi, const char* usage) {
    if (a.pos.size() < lo || a.pos.size() > hi)
        throw Fail("E_ARGS", ssprintf("quill %s takes %s%zu argument%s, not %zu", verb.c_str(), lo == hi ? "" : "at most ", hi, hi == 1 ? "" : "s", a.pos.size()),
                   std::string("usage: ") + usage);
}

Source source_arg(const Args& a) {
    if (a.has("--rotate") && a.has("--no-rotate")) throw Fail("E_ARGS", "--rotate and --no-rotate cannot both be given");
    const int mode = a.has("--no-rotate") ? 1 : a.has("--rotate") ? 2 : 0;
    return source_from_path(a.pos[0], mode, number(a, "--rotate", 0, 0, 9007199254740991ull));
}

std::string state_table_text(const JV& result) {
    const JV* table = result.get("state_table");
    if (!table) return "  states       more than 64: not listed\n";
    std::string out = ssprintf("  states       %zu, each with its decision, where every token leads, and a shortest way to reach it\n", table->arr.size());
    for (const auto& s : table->arr) {
        std::string fields, next, path;
        if (const JV* f = s.get("fields"); f && f->t == JV::Obj)
            for (const auto& kv : f->obj)
                if (kv.second.d != 0) fields += (fields.empty() ? "" : ", ") + kv.first + "=" + show(kv.second.d);
        for (const auto& kv : s.get("next")->obj) next += " " + kv.first + "->" + (kv.second.t == JV::Int ? std::to_string(kv.second.i) : std::string("?"));
        for (const auto& p : s.get("path")->arr) path += (path.empty() ? "" : " ") + p.s;
        out += ssprintf("    %3lld  {%s}  %s (margin %s) %s   reached by: %s%s\n", s.get("id")->i, fields.empty() ? "all fields 0" : fields.c_str(),
                        s.get("decision")->s.c_str(), show(s.get("margin")->d).c_str(), next.c_str(), path.empty() ? "the start state" : path.c_str(),
                        s.get("clean")->b ? "" : "   NOT CLEAN");
    }
    return out;
}

int about() {
    const std::string exe = exe_path();
    bool ok = true;
    std::string detail;
    try {                                                     // health: build and prove the smallest example
        Source s;
        s.is_text = true;
        s.text = example("R1_plain")->text;
        const Answer a = verb_prove(s, ProveOptions(), nullptr);
        ok = a.status == "ok" && a.grade == "proved";
        detail = ok ? "built and proved the example R1_plain: 2 states, grade proved" : "the example R1_plain did not prove: " + a.text.substr(0, a.text.find('\n'));
    } catch (const Fail& f) {
        ok = false;
        detail = f.first().message;
    }
    const auto verb = [](const char* v, const char* what, const char* ex) {
        JV o = JV::object();
        o.set("verb", v).set("what", what).set("example", ex);
        return o;
    };
    JV verbs = JV::array();
    verbs.push(verb("quill build", "a specification -> a network folder in the planted kit's format, plus EXPLAIN.md", "quill build specs/R1_plain.spec -o out/R1_plain"));
    verbs.push(verb("quill prove", "exhaustive proof: reachable states, exactness, cleanliness, noise tolerance, equivalence; graded, with a receipt", "quill prove specs/counter_mod3.spec --json"));
    verbs.push(verb("quill run", "run a network on tokens: margins, decisions, steps per second", "quill run out/R1_plain --tokens \"zero one one\""));
    verbs.push(verb("quill explain", "the field layout and a line of meaning per neuron", "quill explain specs/parity_latch.spec"));
    verbs.push(verb("quill check-kit", "conformance against a planted-kit article folder", "quill check-kit $QUILL_KIT/articles/R1_plain"));
    verbs.push(verb("quill examples", "the specifications inside the executable", "quill examples counter_mod3"));
    verbs.push(verb("quill npz", "list the arrays of a numpy .npz file (stored or deflated)", "quill npz out/R1_plain/weights.npz"));
    verbs.push(verb("quill --selftest", "every claim of the tool checked from the inside", "quill --selftest"));
    JV mcp = JV::object(), args = JV::array(), tools = JV::array();
    args.push("--mcp");
    for (const char* t : { "quill_build", "quill_prove", "quill_run", "quill_explain" }) tools.push(t);
    std::string fwd = exe;
    for (auto& c : fwd)
        if (c == '\\') c = '/';
    mcp.set("command", exe).set("args", args).set("tools", tools).set("register", "claude mcp add quill -- " + fwd + " --mcp");
    JV health = JV::object();
    health.set("ok", ok).set("detail", detail);
    JV o = JV::object();
    o.set("organ", "quill").set("version", kVersion).set("path", exe).set("family", "abacus (graded arithmetic) - quill (graded networks)");
    o.set("purpose", "write, prove and run small neural networks whose every weight is set by formula: a short specification in named fields "
                     "and threshold rules becomes explicit weights with a line of meaning per neuron, an exhaustive proof that the network "
                     "is exact, and a fast runner");
    o.set("stage", "0.1: the recurrent class (one step per token, no attention)");
    o.set("verbs", verbs).set("mcp", mcp).set("health", health).set("docs", exe_dir() + "\\README.md").set("tape", JV());
    const std::string line = json_compact(o) + "\n";
    fwrite(line.data(), 1, line.size(), stdout);
    return ok ? 0 : 2;
}

int emit(const Answer& a, bool json, const std::string& extra_text = std::string()) {
    if (json) {
        const std::string line = answer_json(a) + "\n";
        fwrite(line.data(), 1, line.size(), stdout);
    } else {
        const std::string text = a.text + extra_text;
        fwrite(text.data(), 1, text.size(), a.status == "error" ? stderr : stdout);
    }
    return a.exit_code();
}

std::string receipt_line(const Answer& a) {
    const JV r = receipt_json(a);
    return "  receipt      " + r.get("id")->s + "  input " + r.get("input_sha256")->s + "  result " + r.get("result_sha256")->s + "\n";
}

int dispatch(const std::vector<std::string>& argv, bool& json, std::string& verb) {
    if (argv.size() < 2) {
        print_help();
        return 0;
    }
    verb = argv[1];
    for (const auto& s : argv)
        if (s == "--json" || s == "-j") json = true;
    if (verb == "--help" || verb == "-h" || verb == "/?" || verb == "help") {
        print_help();
        return 0;
    }
    if (verb == "--version" || verb == "-v" || verb == "version") {
        printf("quill %s\n", kVersion);
        return 0;
    }
    if (verb.rfind("--", 0) == 0) verb = verb.substr(2);
    if (verb == "about") return about();
    if (verb == "mcp") return run_mcp();
    const std::vector<Flag> rot = { { "--rotate", true }, { "--no-rotate", false }, { "--json", false } };
    if (verb == "selftest") {
        const Args a = parse_args(verb, argv, 2, { { "--json", false } });
        need_positional(verb, a, 0, 0, "quill --selftest");
        return emit(verb_selftest(), json);
    }
    if (verb == "build") {
        std::vector<Flag> f = rot;
        f.push_back({ "--out", true });
        f.push_back({ "--force", false });
        const Args a = parse_args(verb, argv, 2, f);
        need_positional(verb, a, 1, 1, "quill build SPEC -o DIR");
        return emit(verb_build(source_arg(a), a.get("--out"), a.has("--force")), json);
    }
    if (verb == "prove") {
        std::vector<Flag> f = rot;
        f.push_back({ "--equiv", true });
        f.push_back({ "--cap", true });
        f.push_back({ "--states", false });
        const Args a = parse_args(verb, argv, 2, f);
        need_positional(verb, a, 1, 1, "quill prove SPEC|DIR [--equiv FILE] [--cap N]");
        ProveOptions opt;
        opt.cap = (size_t)number(a, "--cap", 200000, 1, 100000000ull);
        Source other;
        if (a.has("--equiv")) other = source_from_path(a.get("--equiv"));
        const Answer r = verb_prove(source_arg(a), opt, a.has("--equiv") ? &other : nullptr);
        return emit(r, json, (a.has("--states") ? state_table_text(r.result) : std::string()) + receipt_line(r));
    }
    if (verb == "run") {
        std::vector<Flag> f = rot;
        for (const char* k : { "--tokens", "--tokens-file", "--random", "--seed", "--repeat", "--start", "--start-row", "--dump", "--margins", "--show" })
            f.push_back({ k, true });
        const Args a = parse_args(verb, argv, 2, f);
        need_positional(verb, a, 1, 1, "quill run SPEC|DIR --tokens \"zero one one\"");
        RunOptions o;
        o.have_tokens = a.has("--tokens");
        o.tokens = a.get("--tokens");
        o.have_file = a.has("--tokens-file");
        if (o.have_file) o.token_bytes = slurp(a.get("--tokens-file"), "the token file");
        o.have_random = a.has("--random");
        o.random_n = number(a, "--random", 0, 0, 1ull << 40);
        o.seed = number(a, "--seed", 1, 0, ~0ull);
        o.repeat = number(a, "--repeat", 1, 1, 1ull << 40);
        o.have_start = a.has("--start");
        if (o.have_start) o.start_bytes = slurp(a.get("--start"), "the start file");
        o.start_row = number(a, "--start-row", 0, 0, 1ull << 40);
        if (a.has("--start-row") && !o.have_start) throw Fail("E_ARGS", "--start-row needs --start FILE");
        if (a.has("--seed") && !o.have_random) throw Fail("E_ARGS", "--seed needs --random N");
        o.dump_path = a.get("--dump");
        o.margins_path = a.get("--margins");
        o.show = a.get("--show") == "all" ? ~0ull : number(a, "--show", 16, 0, 1000000);
        if (json && o.show > 100000) throw Fail("E_ARGS", "--show is at most 100000 with --json; --margins FILE writes every margin");
        return emit(verb_run(source_arg(a), o), json);
    }
    if (verb == "explain") {
        const Args a = parse_args(verb, argv, 2, rot);
        need_positional(verb, a, 1, 1, "quill explain SPEC|DIR");
        return emit(verb_explain(source_arg(a)), json);
    }
    if (verb == "check-kit") {
        const Args a = parse_args(verb, argv, 2, { { "--spec", true }, { "--json", false } });
        need_positional(verb, a, 1, 1, "quill check-kit DIR [--spec SPEC]");
        Source spec;
        if (a.has("--spec")) {
            spec = source_from_path(a.get("--spec"), 1, 0);          // its logic, axis-aligned
            if (!spec.is_text) throw Fail("E_ARGS", "--spec takes a specification file, not a folder");
        }
        const Answer r = verb_check_kit(a.pos[0], a.has("--spec") ? &spec : nullptr);
        return emit(r, json, receipt_line(r));
    }
    if (verb == "npz") {
        const Args a = parse_args(verb, argv, 2, { { "--json", false } });
        need_positional(verb, a, 1, 1, "quill npz FILE.npz");
        return emit(verb_npz(a.pos[0]), json);
    }
    if (verb == "examples") {
        const Args a = parse_args(verb, argv, 2, { { "--write", true }, { "--json", false } });
        need_positional(verb, a, 0, 1, "quill examples [NAME] [--write DIR]");
        return emit(verb_examples(a.pos.empty() ? std::string() : a.pos[0], a.get("--write")), json);
    }
    const std::string guess = nearest(verb, { "build", "prove", "run", "explain", "check-kit", "examples", "npz", "selftest", "about", "mcp" });
    throw Fail("E_ARGS", "unknown verb '" + argv[1] + "'. The verbs are: build prove run explain check-kit examples npz --selftest --about --mcp",
               guess.empty() ? "quill --help lists everything" : "did you mean '" + guess + "'?");
}

}  // namespace

int app_main(const std::vector<std::string>& argv) {
    bool json = false;
    std::string verb = "quill";
    try {
        return dispatch(argv, json, verb);
    } catch (const Fail& f) {
        return emit(error_answer(verb, f), json);
    } catch (const std::bad_alloc&) {
        return emit(error_answer(verb, Fail("E_LIMIT", "out of memory", "a smaller --cap, or a smaller network")), json);
    } catch (const std::exception& e) {
        return emit(error_answer(verb, Fail("E_INTERNAL", std::string("an internal error: ") + e.what())), json);
    }
}

}  // namespace quill

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_BINARY);                    // one newline per line, also when piped
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
    DWORD mode = 0;
    if (GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode)) SetConsoleOutputCP(CP_UTF8);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) args.push_back(quill::narrow(argv[i]));
    const int rc = quill::app_main(args);
    fflush(stdout);
    fflush(stderr);
    return rc;
}
