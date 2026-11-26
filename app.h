// quill · app.h — the verbs. Each returns an Answer; quill.cpp prints it as plain text or as one
// JSON object, and mcp.cpp serves the same answers over stdio.
#pragma once
#include "prove.h"

namespace quill {

// What every verb returns. `result` is a pure function of the inputs' bytes and the options, so its
// hash is the same on every machine; paths, timings and speeds go in `meta`, which is never hashed.
struct Answer {
    std::string verb;
    std::string status = "ok";          // ok | refuted | undecided | error
    std::string grade = "none";         // proved | checked | sampled | undecided | none
    JV result = JV::object();
    JV input = JV::object();            // what the receipt's input hash covers: the verb, its inputs, its options
    JV meta = JV::object();
    std::string text;                   // the plain-text report; its first line is the JSON member "text"
    std::vector<Issue> issues;          // status error: what is wrong
    int exit_code() const { return status == "ok" ? 0 : status == "refuted" ? 1 : status == "undecided" ? 3 : 2; }
};

// The answer as one line of JSON (with its receipt), and as an error answer from a Fail.
std::string answer_json(const Answer& a);
Answer error_answer(const std::string& verb, const Fail& f);
std::string issue_text(const Issue& i);         // "line 9, column 12: unknown field 'B' ..." with the source line and a caret
JV receipt_json(const Answer& a);

// A specification's text, or a network folder on disk.
struct Source {
    bool is_text = false;
    std::string text;                   // the specification (is_text)
    std::string path;                   // the folder; or, for messages, where the text came from
    int rotate_mode = 0;                // 0 as written, 1 axis-aligned, 2 the seed below
    unsigned long long seed = 0;
};
Source source_from_path(const std::string& path, int rotate_mode = 0, unsigned long long seed = 0);
Model load_model(const Source& s);

struct RunOptions {
    bool have_tokens = false;
    std::string tokens;                 // --tokens: names or positions, separated by spaces or commas
    bool have_file = false;
    std::string token_bytes;            // --tokens-file: int32, little-endian
    bool have_random = false;
    unsigned long long random_n = 0, seed = 1;   // --random N --seed S: uniform tokens from quill's own generator
    unsigned long long repeat = 1;      // --repeat K: the token sequence K times over, the state carried on
    bool have_start = false;
    std::string start_bytes;            // --start: float32 rows of `width` values
    unsigned long long start_row = 0;
    std::string dump_path, margins_path;   // --dump OUT.f32, --margins OUT.f64
    unsigned long long show = 16;       // rows listed in the answer
};

Answer verb_build(const Source& spec, const std::string& out_dir, bool force);
Answer verb_prove(const Source& src, const ProveOptions& opt, const Source* equiv);
Answer verb_run(const Source& src, const RunOptions& opt);
Answer verb_explain(const Source& src);
Answer verb_check_kit(const std::string& dir, const Source* spec);
Answer verb_npz(const std::string& path);
Answer verb_examples(const std::string& name, const std::string& write_dir);
Answer verb_selftest();
int run_mcp();

// The specifications that ship inside the executable (examples.cpp).
struct Example {
    const char* name;                   // "R1_plain"
    const char* file;                   // "R1_plain.spec", or "parity.machine.json"
    const char* about;
    const char* text;
};
const std::vector<Example>& examples();
const Example* example(std::string_view name);

// The 8-bit copy of a state row that the kit logs margins from (kit/build.py, q8): blocks of 32 values,
// each scaled by the block's largest magnitude over 127 and rounded half to even.
void q8_row(const float* row, int width, double* out);

}  // namespace quill
