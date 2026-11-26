// quill · mcp.cpp — the MCP server: newline-delimited JSON-RPC 2.0 on stdin and stdout.
// Four tools, each on inline specification text: quill_build, quill_prove, quill_run, quill_explain.
// A tool's result is the same one-line JSON answer the command line prints with --json.
#include "app.h"

#include <iostream>

namespace quill {

namespace {

JV prop(const char* type, const std::string& description) {
    JV o = JV::object();
    o.set("type", type).set("description", description);
    return o;
}

JV tool(const char* name, const std::string& description, const std::vector<std::pair<std::string, JV>>& props,
        const std::vector<std::string>& required) {
    JV schema = JV::object(), p = JV::object();
    for (const auto& kv : props) p.set(kv.first, kv.second);
    schema.set("type", "object").set("properties", p).set("required", JV::strings(required)).set("additionalProperties", false);
    JV o = JV::object();
    o.set("name", name).set("description", description).set("inputSchema", schema);
    return o;
}

JV tools_list() {
    const std::string spec = "The specification text (the quill language; see quill_build).";
    const std::pair<std::string, JV> rotate = { "rotate", prop("integer", "Build in a rotated basis with this seed (0 to 9007199254740991), whatever the specification says.") };
    const std::pair<std::string, JV> no_rotate = { "no_rotate", prop("boolean", "Build axis-aligned, whatever the specification says.") };
    JV tools = JV::array();
    tools.push(tool("quill_build",
                    std::string("Assemble a small neural network from a specification: explicit weights by formula, no training. Returns sizes, the "
                                "sha256 of each file, and warnings; with `out` it also writes the network folder (weights.npz, network.json, "
                                "manifest.json, EXPLAIN.md) in the planted kit's format. Call quill_prove before trusting what you built.\n\n") +
                        language_card(),
                    { { "spec", prop("string", "The specification text.") },
                      { "out", prop("string", "Folder to write the network into. Omit to assemble without writing anything.") },
                      { "force", prop("boolean", "Overwrite a folder that quill did not write.") },
                      rotate, no_rotate },
                    { "spec" }));
    tools.push(tool("quill_prove",
                    "Prove a specification exact by enumerating every reachable state. Returns, each with a grade (proved, checked, sampled, "
                    "undecided): the number of reachable states and transitions; exactness (every threshold argument z outside (0, 1), or the "
                    "state, token and rule where it is not); cleanliness (every field 0 or 1, or the first that is not and which rules wrote it); "
                    "noise tolerance; sizes; and, with `equiv`, whether it makes the same decisions as a transition table or another "
                    "specification on every token sequence, or a shortest sequence on which they differ. status is ok, refuted or undecided.",
                    { { "spec", prop("string", spec) },
                      { "equiv", prop("string", "Optional. A machine as JSON text, {\"tokens\": [...], \"delta\": [[...]], \"init\": 0, \"emit\": [...]} "
                                                "with delta[state][token] the next state, or another specification's text.") },
                      { "cap", prop("integer", "Stop, undecided, beyond this many reachable states (default 200000).") },
                      rotate, no_rotate },
                    { "spec" }));
    tools.push(tool("quill_run",
                    "Run a specification's network on a token sequence, one step per token. Returns the margin and the decision (emit when the "
                    "margin is above 0) for the first `show` steps, the counts, the final state as field values, and sha256 hashes of all "
                    "decisions and margins. Give `tokens` or `random`.",
                    { { "spec", prop("string", spec) },
                      { "tokens", prop("string", "Token names or positions, separated by spaces or commas, for example \"zero one one\".") },
                      { "random", prop("integer", "Instead of tokens: this many uniform random tokens.") },
                      { "seed", prop("integer", "Seed for `random` (default 1).") },
                      { "repeat", prop("integer", "Run the token sequence this many times over, the state carried on (default 1).") },
                      { "show", prop("integer", "List the first this many steps (default 16, at most 1000).") },
                      rotate, no_rotate },
                    { "spec" }));
    tools.push(tool("quill_explain",
                    "Explain a specification's network: the field layout, what each token adds, every rule with when it fires (read from the "
                    "weights), one line per neuron, the readout, what each shorthand statement expands to, and the whole network in primitive "
                    "statements. Returns Markdown, or the same content as JSON.",
                    { { "spec", prop("string", spec) },
                      { "format", prop("string", "\"markdown\" (default) or \"json\".") },
                      rotate, no_rotate },
                    { "spec" }));
    JV o = JV::object();
    o.set("tools", tools);
    return o;
}

// Typed access to a tool's arguments; anything unknown or of the wrong type is an error.
struct ToolArgs {
    const JV* a = nullptr;
    std::vector<std::string> seen;
    const JV* find(const char* key) {
        if (std::find(seen.begin(), seen.end(), key) == seen.end()) seen.push_back(key);
        return a && a->t == JV::Obj ? a->get(key) : nullptr;
    }
    bool has(const char* key) { return find(key) != nullptr; }
    std::string str(const char* key, bool required) {
        const JV* v = find(key);
        if (!v) {
            if (required) throw Fail("E_ARGS", std::string("the argument '") + key + "' is missing");
            return std::string();
        }
        if (!v->is_str()) throw Fail("E_ARGS", std::string("the argument '") + key + "' must be a string");
        return v->s;
    }
    unsigned long long num(const char* key, unsigned long long dflt, unsigned long long lo, unsigned long long hi) {
        const JV* v = find(key);
        if (!v) return dflt;
        if (v->t != JV::Int || v->i < 0 || (unsigned long long)v->i < lo || (unsigned long long)v->i > hi)
            throw Fail("E_ARGS", ssprintf("the argument '%s' must be a whole number from %llu to %llu", key, lo, hi));
        return (unsigned long long)v->i;
    }
    bool flag(const char* key) {
        const JV* v = find(key);
        if (!v) return false;
        if (v->t != JV::Bool) throw Fail("E_ARGS", std::string("the argument '") + key + "' must be true or false");
        return v->b;
    }
    void done(const char* tool) {
        if (!a || a->t != JV::Obj) return;
        for (const auto& kv : a->obj)
            if (std::find(seen.begin(), seen.end(), kv.first) == seen.end())
                throw Fail("E_ARGS", "the tool " + std::string(tool) + " has no argument '" + kv.first + "'. Its arguments are: " + join(seen, ", "));
    }
    Source source() {
        Source s;
        s.is_text = true;
        s.text = str("spec", true);
        s.path = "(inline)";
        const bool no = flag("no_rotate");
        if (has("rotate")) {
            if (no) throw Fail("E_ARGS", "rotate and no_rotate cannot both be given");
            s.rotate_mode = 2;
            s.seed = num("rotate", 0, 0, 9007199254740991ull);
        } else if (no) s.rotate_mode = 1;
        return s;
    }
};

// Runs one tool. `plain` is set when the result should be shown as it is, not as the JSON answer.
Answer call_tool(const std::string& name, const JV* arguments, std::string& plain) {
    ToolArgs a;
    a.a = arguments;
    if (name == "quill_build") {
        const Source s = a.source();
        const std::string out = a.str("out", false);
        const bool force = a.flag("force");
        a.done("quill_build");
        return verb_build(s, out, force);
    }
    if (name == "quill_prove") {
        const Source s = a.source();
        ProveOptions opt;
        opt.cap = (size_t)a.num("cap", 200000, 1, 100000000ull);
        Source other;
        const bool have = a.has("equiv");
        if (have) {
            other.is_text = true;
            other.text = a.str("equiv", true);
            other.path = "(inline)";
        }
        a.done("quill_prove");
        return verb_prove(s, opt, have ? &other : nullptr);
    }
    if (name == "quill_run") {
        const Source s = a.source();
        RunOptions o;
        o.have_tokens = a.has("tokens");
        o.tokens = a.str("tokens", false);
        o.have_random = a.has("random");
        o.random_n = a.num("random", 0, 0, 1ull << 32);
        o.seed = a.num("seed", 1, 0, 9007199254740991ull);
        o.repeat = a.num("repeat", 1, 1, 1ull << 32);
        o.show = a.num("show", 16, 0, 1000);
        a.done("quill_run");
        return verb_run(s, o);
    }
    if (name == "quill_explain") {
        const Source s = a.source();
        const std::string format = a.has("format") ? a.str("format", true) : std::string("markdown");
        a.done("quill_explain");
        if (format != "markdown" && format != "json") throw Fail("E_ARGS", "format must be \"markdown\" or \"json\"");
        Answer r = verb_explain(s);
        if (format == "markdown") plain = r.text;
        return r;
    }
    throw Fail("E_ARGS", "unknown tool '" + name + "'. The tools are: quill_build, quill_prove, quill_run, quill_explain");
}

std::string id_text(const JV* id) {
    if (!id) return "null";
    if (id->t == JV::Str || id->t == JV::Int || id->t == JV::Num) return json_compact(*id);
    return "null";
}

std::string reply(const std::string& id, const JV& result) { return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + json_compact(result) + "}"; }

std::string reply_error(const std::string& id, int code, const std::string& message) {
    JV e = JV::object();
    e.set("code", code).set("message", message);
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":" + json_compact(e) + "}";
}

}  // namespace

std::string mcp_handle(const std::string& line) {
    JV req;
    std::string err;
    if (!json_parse(line, req, err)) return reply_error("null", -32700, "parse error: " + err);
    if (req.t != JV::Obj) return reply_error("null", -32600, "a request must be a JSON object");
    const JV* idv = req.get("id");
    const JV* mv = req.get("method");
    const bool notification = !idv || idv->t == JV::Null;
    const std::string id = id_text(idv);
    if (!mv || !mv->is_str()) return notification ? std::string() : reply_error(id, -32600, "a request needs a method");
    const std::string& method = mv->s;
    if (notification) return std::string();                    // notifications get no answer
    const JV* params = req.get("params");
    if (method == "initialize") {
        std::string proto = "2025-06-18";
        if (params && params->t == JV::Obj)
            if (const JV* pv = params->get("protocolVersion"); pv && pv->is_str()) proto = pv->s;
        JV caps = JV::object(), info = JV::object(), r = JV::object();
        caps.set("tools", JV::object());
        info.set("name", "quill").set("version", kVersion);
        r.set("protocolVersion", proto).set("capabilities", caps).set("serverInfo", info);
        r.set("instructions", "quill writes, proves and runs small neural networks whose every weight is set by formula. Write a specification "
                              "(the language is in quill_build's description), then quill_prove it: read the grade and, when it is refuted, the "
                              "state, token and rule it names. quill_explain shows what every neuron means.");
        return reply(id, r);
    }
    if (method == "ping") return reply(id, JV::object());
    if (method == "tools/list") return reply(id, tools_list());
    if (method == "tools/call") {
        const JV* name = params && params->t == JV::Obj ? params->get("name") : nullptr;
        if (!name || !name->is_str()) return reply_error(id, -32602, "tools/call needs params.name");
        const JV* arguments = params->get("arguments");
        // an error answer names the verb, as on the command line: quill_prove -> prove
        const std::string verb = name->s.rfind("quill_", 0) == 0 ? name->s.substr(6) : name->s;
        Answer a;
        std::string plain;
        try {
            a = call_tool(name->s, arguments, plain);
        } catch (const Fail& f) {
            a = error_answer(verb, f);
        } catch (const std::bad_alloc&) {
            a = error_answer(verb, Fail("E_LIMIT", "out of memory", "a smaller cap, or a smaller network"));
        } catch (const std::exception& e) {
            a = error_answer(verb, Fail("E_INTERNAL", std::string("an internal error: ") + e.what()));
        }
        JV item = JV::object(), content = JV::array(), r = JV::object();
        item.set("type", "text").set("text", a.status != "error" && !plain.empty() ? plain : answer_json(a));
        content.push(item);
        r.set("content", content).set("isError", a.status == "error");
        return reply(id, r);
    }
    return reply_error(id, -32601, "method not found: " + method);
}

int run_mcp() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (trim(line).empty()) continue;
        const std::string out = mcp_handle(line);
        if (out.empty()) continue;
        fwrite(out.data(), 1, out.size(), stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
    return 0;
}

}  // namespace quill
