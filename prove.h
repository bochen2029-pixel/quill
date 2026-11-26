// quill · prove.h — the exhaustive analysis: every reachable state, every threshold, every field.
#pragma once
#include "spec.h"

namespace quill {

// A finite machine with a decision in every state: what a network is once its reachable states are
// enumerated, and what a transition table is.
struct Moore {
    std::string label;                  // for messages: "the network R1_plain", "the machine parity"
    std::vector<std::string> tokens;
    int start = 0;
    int states = 0;
    std::vector<int> next;              // states x tokens; -1 where the enumeration stopped before expanding
    std::vector<char> emit;             // per state: 1 when the decision is emit
    bool complete = true;               // every reachable state was expanded
    bool exact = true;                  // the states closed bit for bit (a table is always exact)
};

// A transition table from JSON: {"tokens": [...], "delta": [[...], ...], "init": 0, "emit": [...]}.
// Its columns are put in the order of `net_tokens`. Throws Fail(E_EQUIV).
Moore moore_from_machine(const JV& machine, const std::vector<std::string>& net_tokens);
// A network's own machine, by enumeration (at most `cap` states).
Moore moore_from_model(const Model& m, size_t cap);

struct ProveOptions {
    size_t cap = 200000;                // stop, undecided, when more states than this are reachable
};

struct Proof {
    JV result;                          // the "result" member of the answer
    std::string status;                 // ok | refuted | undecided
    std::string grade;                  // proved | checked | sampled | undecided
    std::string text;                   // the report, one line per result; the first line is the verdict
};

// `logic` is the network to prove. `build`, when given, is the same network rotated: it is enumerated
// in float64 and compared with `logic` state by state. `other`, when given, is the machine to prove
// `logic` equivalent to, with `other_input` saying what it is in the result.
Proof prove(const Model& logic, const Model* build, const ProveOptions& opt, const Moore* other, const JV* other_input);

}  // namespace quill
