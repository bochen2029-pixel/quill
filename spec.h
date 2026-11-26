// quill · spec.h — the specification language, its expansion to primitive statements, the
// assembler that turns those into weights, and the model that prove, run and explain work on.
#pragma once
#include "net.h"

namespace quill {

// ---- a linear form over fields ------------------------------------------------------------------
struct Term {
    int field = 0;
    double coef = 0;
    // While a specification is read (W_ROUNDED_CONSTANT): what is written minus what float64 holds, to first
    // order; whether any rounding entered the coefficient; and whether an operation on numbers rounded.
    double err = 0;
    bool rounded = false, op_rounded = false;
};
struct Lin {
    std::vector<Term> terms;     // in the order first written; no field twice, no zero coefficient
    double constant = 0;
    double cerr = 0;             // the same three for the constant
    bool crounded = false, cop_rounded = false;
    double coef(int field) const {
        for (const auto& t : terms)
            if (t.field == field) return t.coef;
        return 0;
    }
};

// ---- a parsed specification ---------------------------------------------------------------------
struct FieldDecl {
    enum Kind { One, Declared, Input, State };
    std::string name;
    Kind kind = Declared;
    int line = 0;                // where it was declared; 0 for the constant `one`
    int token = -1;              // Input: the token it marks
    int machine = -1;            // State: its machine
    double init = 0;
    int init_line = 0;           // 0: the default start value
    int clear_line = 0;          // 0: never cleared
    bool auto_clear = false;     // an `in.<token>` field quill declared, embedded and clears
};

// Everything a specification says, as a flat list of primitive operations in order. The sugar
// statements (latch, machine, emit) and the automatic `in.<token>` fields are already expanded:
// `generated` marks what quill wrote. Printing the list back gives a specification in primitive
// statements that builds the identical network (expanded_text).
struct Op {
    enum Kind { Fields, Embed, Init, Tau, Neuron, Clear, Readout };
    Kind kind = Fields;
    int line = 0;                // the source line it comes from
    bool generated = false;
    int sugar = -1;              // index into Spec::sugar, when a sugar statement wrote it
    std::vector<int> fields;     // Fields: declared; Embed: written; Init: set; Tau: targets; Clear: cleared
    std::vector<double> values;  // Init: one per field
    int token = -1;              // Embed
    Lin in;                      // Tau, Neuron: the argument;  Readout: the margin
    Lin out;                     // Neuron: what it writes per unit of relu
    std::string note;            // the author's comment, or quill's words for a generated rule
};

struct Sugar {
    int line = 0;
    std::string text;            // the statement as written, without its comment
};

struct Machine {
    std::string name;
    int line = 0;
    int states = 0, init = 0;
    int first_field = 0;         // the field of state 0; the others follow
    std::vector<int> delta;      // states x tokens: the next state (a held pair maps to itself)
};

struct Spec {
    std::string text, sha256;
    std::vector<std::string> lines;
    std::string name;
    std::vector<std::string> tokens;
    std::vector<FieldDecl> fields;       // [0] is the constant `one`
    std::vector<Op> ops;
    std::vector<Sugar> sugar;
    std::vector<Machine> machines;
    bool rotate = false;
    unsigned long long seed = 0;
    int width = 0;                       // as written, or the default
    bool width_explicit = false;
    std::vector<Issue> warnings;
    int field(std::string_view want) const;
    int token(std::string_view want) const;
};

// Throws Fail carrying every problem found (code E_SPEC, with line, column and the source line).
Spec parse_spec(const std::string& text);
std::string lin_text(const Spec& s, const Lin& l);           // "2*in1 - 2*A - 0.5"
std::string op_text(const Spec& s, const Op& op);            // the operation as a primitive statement
// The embed and the clear quill writes for an in.<token> field: implied wherever that field is declared
// (in an expression or by `fields in.TOKEN`), so the text printed back leaves them out.
bool implied_by_input(const Spec& s, const Op& op);
std::string expanded_text(const Spec& s);                    // the whole specification, primitives only

// ---- weights --------------------------------------------------------------------------------------
struct Rule {
    enum Kind { Tau, Clear, Neuron };
    Kind kind = Tau;
    int n0 = -1, n1 = -1;        // its neurons; n1 is -1 for a lone neuron
    int line = 0;                // source line; 0 when the rule was recovered from weights
    std::string text;            // as a primitive statement
    std::string note;            // its meaning in words, when known
    std::string from;            // the sugar statement that wrote it, when one did
    std::vector<int> targets;    // the fields it writes
};

struct Built {
    Network net;                 // axis-aligned: coordinate k is field k, then spare0, spare1, ...
    std::vector<Rule> rules;
    std::vector<int> neuron_rule;
};
Built assemble(const Spec& s);                               // throws Fail(E_LIMIT) when too large

// An orthogonal matrix from a seed. The kit's recipe (the QR factor of a Gaussian matrix, columns signed
// so that R's diagonal is positive) on quill's own random stream, computed with +, -, *, / and sqrt only,
// so a seed gives the same bits everywhere. Not numpy's matrix. Only its first `cols` columns are
// computed and returned (d x cols, row-major, row stride cols); they are bit for bit those of the whole
// matrix, which the rest would cost O(d^3) to compute.
std::vector<double> rotation(int d, unsigned long long seed, int cols);
// The columns of Q that a rotated build of `axis` needs: every coordinate a weight or the start state
// touches, and up to four spares after the `fields` named fields (the manifest's true shams).
size_t rotation_columns(const Network& axis, size_t fields);
// The same function in the rotated basis: state_rotated = Q state_axis. q holds the first m columns.
Network rotated(const Network& axis, const std::vector<double>& q, size_t m);

// ---- the model: a network with what is known about it -------------------------------------------
struct Model {
    Network net;                         // the network as stored and run
    std::string origin;                  // "specification", "quill folder", "kit folder", "folder"
    bool rotated = false;                // false: every named field is one coordinate
    unsigned long long seed = 0;         // the rotation seed, when quill rotated it
    bool seed_known = false;
    std::vector<std::string> fields;     // named fields; empty when no names are known
    std::vector<int> coord;              // axis-aligned: each field's coordinate
    std::vector<double> dirs;            // rotated: fields x d, each field's direction
    std::vector<std::string> sham_names; // up to four directions nothing reads or writes (for the manifest)
    std::vector<double> sham_dirs;       // sham_names x d
    std::vector<Rule> rules;
    std::vector<int> neuron_rule;
    std::vector<Machine> machines;
    std::vector<Issue> warnings;
    std::vector<std::string> notes;      // facts about how the model was obtained
    JV input;                            // how results name the input: its kind and sha256s
    std::string spec_text;               // the specification, when one is known and verified
    // The network in field coordinates, filled by finish(): rd and wr are neurons x F, em is tokens x F.
    std::vector<double> rd, wr, em, r0f, ro;
    std::vector<double> rd_out, wr_out;  // per neuron: how much it reads or writes outside the named fields
    int one = -1;                        // the index of the constant field, or -1

    int F() const { return (int)fields.size(); }
    int field(std::string_view name) const;
    double value(const double* state, int f) const;
    void finish();                       // computes the field-coordinate views and names recovered rules
    Lin rule_lin(int rule) const;        // a tau's or neuron's argument in field coordinates (snapped)
    std::string lin_text(const Lin& l) const;
};

Model model_from_spec(const Spec& s, bool allow_rotate);
Model model_from_folder(const Folder& f);
// A specification's text. `rotate_mode`: 0 as written, 1 force axis-aligned, 2 force the seed below.
Model model_from_text(const std::string& spec_text, int rotate_mode = 0, unsigned long long seed = 0);

// ---- folders and explanations (explain.cpp) -------------------------------------------------------
JV network_json(const Model& m, const std::string& weights_sha256);
JV manifest_json(const Model& m, const std::string& weights_sha256);
std::string explain_markdown(const Model& m);
JV explain_json(const Model& m);
JV sizes_json(const Network& net);                               // width, neurons, weights, operations per step
// "{A=1}": the fields that are not 0, and every named field but `one`. Rounded to 1e-9, unless `exact` (prove, when
// nothing rounded: a field of 2^-40 is then 2^-40, not 0).
std::string state_text(const Model& m, const double* state, bool exact = false);
JV state_json(const Model& m, const double* state, bool exact = false);
double snap9(double v);                                          // to the 1e-9 grid when it is that close to it
// When a rule fires, read off its weights: "on one when A=0". Empty when it reads too many fields.
struct Firing {
    bool known = false;
    std::string when;            // the (token, state) cases in which z >= 1 (a lone neuron: z > 0)
    std::string ramp;            // the cases in which a tau's z would be strictly inside (0, 1), if any
    std::vector<double> z;       // the distinct values of z in the firing cases
};
Firing firing(const Model& m, int rule);

}  // namespace quill
