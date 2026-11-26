# quill

**Write a small neural network as a short text, get its weights by formula, a proof that it is exact, and a fast runner.**

quill is one self-contained Windows executable. A language model (or a person) writes a
specification in terms of named fields and threshold rules. quill expands it into explicit
weights with a line of meaning for every neuron, enumerates every state the network can reach
and proves that it never leaves its clean values, and runs it at millions of steps per second.
Nothing is trained.

It exists because a model writing weights by hand is good at choosing the algorithm and the
meaning of each number, and bad at long exact arithmetic, bookkeeping, and seeing its own
mistakes. quill does the exact part and returns a graded result, as a certified-arithmetic tool
does for numbers. The recipe is that of
[Looped Weight-Space Programming](https://mbzuai-oryx.github.io/LoopedWSP/) (MBZUAI, 2026); the
reference implementation is the Python planted kit, a prototype that is not part of this
repository. quill reads and writes the kit's network folders (section 6), and needs nothing from
the kit to build, to pass its self-test or to run; only `check-kit` reads the kit's article
folders, each a network with a recorded run (section 4.5).

Version 0.1.2 covers the **recurrent class**: one step per token, no attention.
C++20, operating-system APIs only, no third-party code, about 1.2 MB.

It corrects two defects of earlier versions; `CHANGELOG.md` has the details:
- 0.1.0's `prove` could pass as `ok` a network that leaves its clean values by less than 1e-9,
  doubles that at every step and loses its bit after a few dozen steps (section 4.2, "How states,
  fields and thresholds are compared").
- 0.1.1's `prove` read out of bounds, and could crash, when the start state itself is not clean.

This file is the contract: what every verb promises, exactly. Every example output below was
printed by this version; only timings can differ from one run to the next.

```
$ quill examples parity
net     parity
tokens  zero one skip
fields  A
tau     A <- 2*in.one - 2*A - 0.5               # a one arrives while A is 0: A becomes 1
tau     A <- 2*A - 2*in.one - 2*in.zero - 0.5   # A is 1 and a skip arrives: A stays 1
clear   A                                       # A is rewritten from zero at every step
readout 8*A - 4                                 # emit while the ones since the last zero are odd in number

$ quill prove specs/parity.spec
prove parity: ok, grade proved
  reachable    2 states, 6 transitions; the set is closed bit for bit, in exact arithmetic   [proved]
  exactness    all 2 thresholds stay outside (0, 1) at every state and token; smallest distance 0.5 (rule 1 (line 4: tau A <- 2*in.one - 2*A - 0.5) at z = -0.5, state {all fields 0}, token zero)   [proved]
  cleanliness  every one of the 4 named fields is 0 or 1 in every state   [proved]
  noise        an error of up to 0.083333333 on every coordinate of the state cannot move a threshold into (0, 1); 0.144337567 in Euclidean norm; 0.25 on one coordinate alone (coordinates are fields)   [proved]
  sizes        width 32, 10 neurons, 844 weights (37 not zero), 37 operations per step (682 dense)   [proved]
  decisions    emit in 1 state, hold in 1; smallest |margin| 4
  receipt      quill1:881624541aaa  input a59b11d5775063b2c544aba9897cf611fd67817ada4062b85717ae234780bc48  result 41657ddd6140d967a5588de77ab80352e471b0091e05944f6d95431271528a1f

$ quill run specs/parity.spec --tokens "one one skip one zero"
run parity: 5 steps, 2 emit, 3 hold, margins from -4 to 4
    step  token  margin        decision
       0  one    4             emit
       1  one    -4            hold
       2  skip   -4            hold
       3  one    4             emit
       4  zero   -4            hold
  final state {all fields 0}
  speed not measured: 5 steps took under a millisecond (--repeat K runs the sequence K times over)
```

A mistake is named, not hinted at. With `- 2*in1` left out of a hold rule:

```
$ quill prove specs/fault_two_setters.spec
prove fault_two_setters: REFUTED, grade proved
  reachable    3 states, 8 transitions; 1 state is not clean and was not followed further   [undecided]
  exactness    all 2 thresholds stay outside (0, 1) at every explored state and token; smallest distance 0.5 (rule 1 (line 9: tau A <- 2*in1 - 0.5) at z = -0.5, state {all fields 0}, token zero)   [undecided]
  cleanliness  FAILS: field A is 2, not 0 or 1, after token one in state {A=1} (reached by: one): A = 1 from rule 1 (line 9: tau A <- 2*in1 - 0.5) at z = 1.5 + 1 from rule 2 (line 10: tau A <- 2*A - 2*in0 - 0.5) at z = 1.5   [proved]
  ...
```

## Contents

1. [Build](#1-build)
2. [The network](#2-the-network)
3. [The specification language](#3-the-specification-language)
4. [The verbs](#4-the-verbs): build, prove, run, explain, check-kit, examples, npz, --selftest, --about, --mcp
5. [The answer: JSON, grades, receipts, exit codes](#5-the-answer)
6. [The file format](#6-the-file-format)
7. [Limits, and what is not in 0.1](#7-limits-and-what-is-not-in-01)
8. [Rerunning the acceptance tests](#8-rerunning-the-acceptance-tests)
9. [Where things are](#9-where-things-are)

## 1. Build

```
.\build.bat      VS2022: cl /std:c++20 /O2 /W4 /WX /permissive- /utf-8 /MT /fp:strict  ->  quill.exe
```

- Needs Visual Studio 2022 with the x64 C++ tools. Nothing else: no package, no download, no Python.
- Run it from the repository root as `.\build.bat`, which works in cmd and in PowerShell alike
  (PowerShell does not run a script from the current folder by its bare name).
- A clean build prints the twelve file names and `OK: quill.exe`. Any warning stops the build (`/WX`).
- `/fp:strict` is deliberate: nothing is fused or reordered, and `prove` reads the processor's inexact flag.
- `quill --selftest` then checks the executable from the inside (64 checks, about a second, no file written).
  The last check compares a `specs/` folder beside the executable with the examples inside it; an
  executable with no `specs/` beside it (the release's `quill.exe` on its own) skips that check, says
  so, and runs 60.

## 2. The network

- **State** `r`: `width` float64 numbers. Named **fields** are coordinates of it. The field `one`
  always exists and always holds 1: it carries every constant, and the readout has no bias.
- **One step per token:** `x = r + E[token]`, then `r' = x + W2 relu(W1 x + b1)`.
- **Readout:** two rows `[hold, emit]`. The **margin** is `emit - hold`, a linear function of the
  state. The **decision** is `emit` when the margin is above 0, `hold` otherwise.
- **A threshold** is `tau(z) = relu(z) - relu(z - 1)`: 0 for z <= 0, 1 for z >= 1. Rules are written
  so that z lands at 1.5 or above, or at -0.5 or below. Each threshold is two neurons.
- **Every rule reads `x`**, the state with the token added and before any rule has written. One rule
  cannot read what another writes in the same step. This is the one trap of the model.
- **Rotation (optional):** the same network expressed in a basis rotated by an orthogonal matrix Q
  from a seed, `state_rotated = Q state`. No coordinate means anything; each field lies along a
  column of Q.

**The arithmetic is fixed**, so that the same file gives the same bits on every machine:
one rounded multiply and one rounded add per term, nothing fused, in this order:

```
z[i]    starts at b1[i] and takes W1[i][j] * x[j] for j = 0, 1, 2, ...
r'[k]   starts at x[k] and takes W2[k][i] * z[i] for the neurons i with z[i] > 0, in increasing i
margin  is ((s0 + s1) + (s2 + s3)) + (bout[emit] - bout[hold]), where s[j mod 4] takes
        (Wout[emit][j] - Wout[hold][j]) * r'[j] for j = 0, 1, 2, ...
```

Terms that are exactly zero may be skipped, which changes no value. There are two code paths: a
sparse one, which visits the nonzero weights only and is taken when fewer than a quarter of the
entries of W1 and W2 are nonzero (an axis-aligned network), and a dense one with SSE2 for the rest
(a rotated network). `--selftest` holds both to this definition value for value.

## 3. The specification language

Line-oriented text, UTF-8. `#` starts a comment. One statement per line; blank lines are ignored.

```
net     R1_plain
tokens  zero one two three
fields  in0 in1 in2 A                 # "one" is implicit
embed   zero in0                      # when this token arrives, add 1 to this field
embed   one  in1
embed   two  in2                      # a token with no embed line writes nothing
init    A=0                           # start values; one=1 is implicit
tau     A <- 2*in1 - 2*A - 0.5        # A += tau(expression)
tau     A <- 2*A - 2*in1 - 2*in0 - 0.5
clear   A in0 in1 in2                 # x += -relu(x) + relu(-x): the field is rewritten from zero
readout 8*A - 4*one                   # the margin
rotate  101                           # optional
width   32                            # optional
```

### 3.1 Names and numbers

| thing | form |
|---|---|
| network name | letters, digits, `_`, `.`; at most 64 characters |
| token name | letters, digits, `_`. A name made of digits only must be the token's own position (`tokens 0 1 2`), because `run --tokens` takes names and positions alike |
| field name | starts with a letter or `_`, then letters, digits, `_`; parts may be joined by `.` (`S.0`). Reserved: `one`; `spare0`, `spare1`, ...; and every name that starts with `in.`, which is the input field of a token (3.3) |
| number | `2`, `0.5`, `.5`, `2.5e-1` |
| expression | a sum of `number*field` terms and numbers, with `+ - * /` and parentheses. It must be linear in the fields: `2*(A + B) - A/2` is fine, `A*B` is refused, and so is a division by a field. `8/3*A` gives the double `8.0/3.0`. A term whose weight comes to 0 is dropped |

Use weights that are exact in binary (2, 0.5, 2.5, 4, ...). Then no sum rounds and `prove` can
grade the network `proved`. With weights such as 2.2 and 0.7 the same network works, but its
sums round and the grade is `checked`. With exact sums `prove` tolerates nothing: a threshold
argument that lands 2^-40 inside (0, 1) is on its ramp, and is refuted as such (4.2).

### 3.2 Statements

Order: `tokens` comes before any statement that names a token; a field is declared before it is
used (an `in.TOKEN` field is declared by its first use). Otherwise the order is free. The order of
the rules is the order of the neurons; the clears of the `in.TOKEN` fields come after the last rule.

| statement | meaning | how often |
|---|---|---|
| `net NAME` | the network's name | exactly once |
| `tokens T1 T2 ...` | the input alphabet; positions count from 0 | exactly once |
| `fields F1 F2 ...` | declares fields; coordinates are given in order of declaration, after `one`. `fields in.TOKEN` declares that token's input field at that point (3.3) | any |
| `embed TOKEN F1 [F2 ...]` | `E[TOKEN][F] += 1` for each field. A (token, field) pair may appear once | any |
| `init F=V [F=V ...]` | start value of a field (default 0) | once per field |
| `tau T1 [T2 ...] <- EXPR` | two neurons with argument z = EXPR: `relu(z)` adds +1 to every target, `relu(z - 1)` adds -1. So each target gets `+= tau(z)` | any |
| `neuron EXPR -> OUT` | one neuron: `relu(EXPR)` times each weight of OUT is added to that field. OUT is a sum of weighted fields with no constant, such as `A - B` or `+1*A` | any |
| `clear F1 [F2 ...]` | per field two neurons, `relu(F)` adding -1 and `relu(-F)` adding +1: together they subtract the field's value, so the taus write it afresh | once per field |
| `readout EXPR` | the margin. Rows: `hold = -EXPR/2`, `emit = +EXPR/2` | exactly one `readout` or `emit` |
| `emit F1 [F2 ...]` | shorthand for `readout 8*F1 + 8*F2 + ... - 4`: margin +4 when exactly one listed field is 1, -4 when none is | |
| `rotate SEED` | build in a rotated basis; SEED is a whole number from 0 to 9007199254740991 | at most once |
| `width N` | the state's width, 1 to 4096. Default: the fields (with `one`) plus 4 spares, rounded up to a multiple of 8, at least 32 | at most once |
| `latch`, `machine`, `on` | shorthand, section 3.4 | |

The items of a list (tokens, fields, the fields of an `embed`, tau targets, `clear`, `emit`, the
tokens and the moves of an `on` line) may be separated by spaces or commas; the tokens of a latch
key are joined by commas (`set=a,b`).

Where a constant goes: in `tau` and `neuron`, a bare number is the neuron's bias and `c*one` is a
weight on the constant field (the two compute the same thing). In `readout` there is no bias, so
a bare number is the weight on `one`: `8*A - 4` and `8*A - 4*one` are the same readout.

A comment on a `tau`, `neuron`, `clear` or `readout` line is kept as **that rule's meaning** and
shown by `explain` and in EXPLAIN.md.

### 3.3 `in.TOKEN`

`in.TOKEN` is the **input field** of a token: it is 1 while that token is being read and 0
otherwise. It is declared by its first use, in an expression or anywhere a field is named, or
explicitly by `fields in.TOKEN`, which places its coordinate there. Either way quill embeds the
token into it and clears it after the last rule; an `embed` or `clear` naming it is refused. The
explicit form with ordinary names (`fields in0`, `embed zero in0`, `clear in0`) does the same by
hand; the kit-equivalent specifications under `specs/` use it, so their weights match the kit's
byte for byte.

### 3.4 Shorthand

Every shorthand statement becomes primitive statements. `quill explain` shows what each one
expanded to, and prints the whole network back in primitive statements; that text is itself a
specification and builds the identical weights. In those texts a token's input field appears as a
`fields in.TOKEN` line where it is first needed; its embed and clear are implied (3.3).

**`latch NAME [set=T,..] [reset=T,..] [toggle=T,..] [init=0|1]`** is a one-bit register. A token has
at most one role; every other token holds the bit. NAME is declared if it is not yet (it must be
an ordinary field). It expands to

```
tau NAME <- 2*in.S1 + 2*in.S2 ... - 0.5                      # if there are set tokens
tau NAME <- 2*in.G1 ... - 2*NAME - 0.5                       # if there are toggle tokens: on, while NAME is 0
tau NAME <- 2*NAME - 2*in.(every set, reset, toggle token) - 0.5      # NAME stays 1 unless one of them arrives
clear NAME
```

**`machine NAME states=N [init=K]`** followed by `on` lines is any finite-state machine, one field
per state (`NAME.0` ... `NAME.N-1`), exactly one of them 1. `init` defaults to 0. A machine may not
be named `in`.

```
machine S states=3 init=0
  on one:  0->1 1->2 2->0          # on TOKEN[, TOKEN ...]: FROM->TO ...
  on zero: *->0                    # * is every state not listed for that token
emit    S.1 S.2                    # (state, token) pairs that are not listed hold
```

The `on` lines of a machine follow it directly (comments and blank lines may come between). A
token may appear on several `on` lines; a listed move overrides that token's `*`; two different
moves for one (state, token) pair, or two different `*` for one token, are refused. The machine
expands to, tokens in the order of `tokens`, states in increasing order:

```
tau NAME.t <- 2*in.e - 0.5                  # for a token e whose only line is *->t
tau NAME.t <- 2*in.e + 2*NAME.s - 2.5       # for every other move s -> t with t != s
tau NAME.s <- 2*NAME.s - 2*in.e1 - 2*in.e2 ... - 0.5     # one per state that holds on some token:
                                            #   e1, e2, ... are the tokens on which it does not hold
clear NAME.0 ... NAME.N-1
```

### 3.5 Errors and warnings

A faulty specification is refused (exit code 2, code `E_SPEC`) with **every** error found (the first
in each statement, at most 50), each with its line, its column, the source line and, where quill can
tell, what to write instead. With this `bad.spec`:

```
# a specification with three mistakes, to see how they are reported
net     bad
tokens  zero one
fields  A B
tau     A <- 2*in.one - 2*C - 0.5
tau     B <- 2*A × 3
clear   A A
readout 8*A - 4
```

```
$ quill prove bad.spec
quill prove: error E_SPEC: line 5, column 27: unknown field 'C'. Declared so far: one A B in.one
    tau     A <- 2*in.one - 2*C - 0.5
                              ^
    hint: declare it first with: fields C
quill prove: error E_SPEC: line 6, column 18: unexpected character '×'
    tau     B <- 2*A × 3
                     ^
    hint: write * for multiplication
quill prove: error E_SPEC: line 7, column 11: the field 'A' is listed twice
    clear   A A
              ^
3 errors.
```

These are errors because each is a mistake that would otherwise be silent: a field cleared twice
(it would be subtracted twice), anything that writes `one`, a product of two fields, a token with
two roles in a latch, two different moves for one (state, token) pair, a `width` smaller than the
fields, an ordinary field or a machine named like an input field, a non-ASCII minus sign or arrow.

Warnings do not stop a build and are listed in every answer:

| code | when |
|---|---|
| `W_NOT_CLEARED` | a tau or a token writes a field that is never cleared, so it grows |
| `W_UNUSED_FIELD` | a declared field that nothing reads or writes and that starts at 0 |
| `W_EDGE` | (prove) the threshold argument closest to (0, 1) has no room, or almost none: it lands exactly on 0 or 1; or within 1e-9 outside (0, 1), and the message says how far; or, when the arithmetic rounds, inside (0, 1) by no more than rounding explains there, and the message gives both numbers |
| `W_MARGIN_ZERO` | (prove) a reachable state's margin is 0, or within 1e-9 of it: its decision has no room, and rests on rounding when the readout rounds; the message gives the margin in full |
| `W_OUTSIDE` | (prove) reachable states hold something outside the named fields |
| `W_SPEC_MISMATCH`, `W_DIRECTIONS` | (folders) the manifest does not match the weights, or its directions are not orthonormal |

## 4. The verbs

`SPEC` is a specification file and `DIR` is a network folder (section 6). Where a verb takes
`SPEC|DIR`, a folder is loaded and a file is read as a specification. Every verb takes `--json`
(or `-j`) and then prints exactly one JSON object on one line to stdout (section 5). Without it, a
report goes to stdout and an error to stderr, as plain text. On a `SPEC`, `--rotate SEED` and
`--no-rotate` override the specification's own `rotate` line (build, prove, run, explain).

quill writes files only where it is told to: `build -o DIR` (and the MCP tool `quill_build` with
`out`), `examples --write DIR`, `run --dump` and `run --margins`.

### 4.1 `quill build SPEC [-o DIR] [--force]`

Assembles the network. With `-o DIR` it writes four files and reads each back to confirm it:
`weights.npz`, `network.json`, `manifest.json` (section 6) and `EXPLAIN.md` (the output of
`quill explain`). Without `-o` it writes nothing and still reports everything.

- It refuses (`E_EXISTS`) to write into a folder that holds a network quill did not write, unless
  `--force`, and to write where a file of that name is.
- The files contain no path and no time: the same specification gives the same bytes anywhere.
- Result: `name`, `basis` (`axis` or `rotated`), `rotate_seed`, `sizes`, `rules` (how many),
  `fields`, `tokens`, `weights_sha256`, `files` (bytes and sha256 of each of the four), `written`,
  `warnings`.
- build does not prove. Run `quill prove` on the specification or on the folder.

### 4.2 `quill prove SPEC|DIR [--equiv FILE] [--cap N] [--states]`

Enumerates every reachable state breadth-first from the start state under every token, and reports
six results. Each carries a **grade** (section 5.2).

**How states, fields and thresholds are compared.** It depends on the arithmetic, which `prove`
reads off the processor's inexact flag around every step (`arithmetic`).

- **Exact** (no operation of any state update rounded, in an axis-aligned network): the float64
  values are the real-number values of the stored weights, so nothing is tolerated. Two states are
  the same only bit for bit (0 and -0 are equal), a threshold argument strictly inside (0, 1) by any
  amount is on its ramp, and every named field must be exactly 0 or 1. Thresholds that land exactly
  on 0 or 1 still prove (with `W_EDGE`).
- **Rounded** (some step rounded, or a rotated network): a successor that is not bit for bit an
  enumerated state is merged with one within 1e-9 in every coordinate only where the merge
  provably changes nothing. That state's own noise margin must be at least 2e-9: on every token, an
  error of 2e-9 on every coordinate moves no threshold argument across an edge and no readout
  margin across 0, and no lone neuron is active. In an axis-aligned network the two must also differ
  only in fields that a clear rewrites. Both then give every threshold the same value, and the
  clears erase the gap. Any other successor is kept as a state of its own (`kept_apart` counts
  those within 1e-9 of another), so a drift is followed step by step until it shows as a fault, or
  until the cap stops the enumeration. A threshold argument inside (0, 1) is on its ramp unless
  rounding explains it. In an axis-aligned network with field names that is a bound on the rounding
  of that very step (when the step rounded) plus what the state's own deviation from clean values
  does to z; in a rotated network, or one without names, it is 1e-9. A named field must be within
  1e-9 of 0 or 1.

`closure_residual` is the largest gap that was merged, 0 when every match was bit for bit. quill
0.1.0 used the 1e-9 tolerance in exact arithmetic too: it merged a state 2^-40 away from another
and took a z of 1 - 2^-40 to be on the edge, so `specs/fault_edge_drift.spec`, a latch that loses
its bit after 40 steps, proved `ok`. It is now refuted at the first step where the drift shows.
`specs/` holds the four cases that an independent review found: `fault_edge_drift`,
`fault_edge_decimal`, `fault_creep_up` and `fault_creep_down`.

| result | what it says |
|---|---|
| `reachable` | `states`, `transitions` (state-token pairs evaluated), `depth` (the longest shortest path), `complete`, `closure_residual`, `kept_apart`; at the cap also `exhaustive_to_depth` |
| `exactness` | for every expanded state, token and threshold, the argument z. The distance of z from the open interval (0, 1) is `max(-z, z - 1)`: positive outside, negative inside. `min_distance` is the smallest, as it is (not rounded), and `at` gives the rule, state, token and path where it occurs. The claim fails when some z is on its ramp (see above): `violation_count` counts the cases and `violations` describes the first 8, each with `rounding_bound`, how far inside rounding could have put that z (0 when nothing rounded). `inside_by_rounding` is null, or the deepest z inside (0, 1) that rounding explains: `by` how much, and the `bound` there |
| `cleanliness` | every named field is 0 or 1 in every reachable state (exactly when the arithmetic is exact, to within 1e-9 when it rounds), and `one` is 1 (`max_offset` is the largest deviation accepted, `outside_named_fields` what the states hold beyond the named fields). Or `violations` describes up to 8 unclean states, each with its first unclean field (`value`: in full when the arithmetic is exact), the state and token before it, what was kept of the field's old value and what each rule added (`written_by`) |
| `noise` | from the distances above and the weights. For a threshold with weight row w (stored coordinates) and room d = its smallest distance: `linf` is the smallest d / (sum of the absolute values of w), `l2` the smallest d / (Euclidean norm of w), `single` the smallest d / (largest absolute value of w), and `rule` attains `linf`. Meaning: if the state at the start of a step is off by at most `linf` in every coordinate (or by at most `l2` in Euclidean norm, or by at most `single` in one coordinate only), no threshold argument enters (0, 1) at that step, so every threshold gives exactly the value it would have. `decision_linf` is the same for the sign of the margin. Computed only when exactness and cleanliness hold over the complete set |
| `equivalence` | only with `--equiv`: see below |
| `sizes` | `width`, `neurons`, `tokens`, `weights` (every stored number), `nonzero_weights`, `ops_per_step` (the largest number of nonzeros in a row of E, plus the nonzeros of W1 and of W2, plus one per neuron) and `ops_per_step_dense` (width + 2 * width * neurons + neurons) |

**A fault is reported with a witness:** the state as field values, the token, the rule with its
source line, and `path`, a shortest token sequence from the start to that state. A state that is
not clean is not followed further: it is a fault already.

**`--equiv FILE`** proves that the network and another machine make the same decision after every
non-empty token sequence, by breadth-first search over pairs of states; or gives `differs_after`,
a shortest token sequence after which they decide differently, with `network_decision` and
`other_decision`. FILE is one of:

- a transition table in JSON (a file whose first character other than white space is `{`):
  `{"name": "mod3", "tokens": ["zero", "one", "two", "three"], "delta": [[0, 1, 0, 0], [0, 2, 1, 1], [0, 0, 2, 2]], "init": 0, "emit": [1, 2]}`.
  `delta[state][token]` is the next state, tokens in the order of the file's own `tokens` (which
  must be the network's token names; if `tokens` is left out, the network's order is used);
  `init` defaults to 0 and `emit`, the states in which the decision is emit, to none.
  Any other key is refused (`E_EQUIV`);
- another specification (its logic, axis-aligned), or another network folder.

**`--cap N`** (1 to 100,000,000; default 200,000). When more than N states are reachable the
enumeration stops: status `undecided`, exit code 3. The answer then says up to what length every
token sequence was covered (`exhaustive_to_depth`) and adds `evidence.sampled`: 64 random walks of
4,096 steps from the start (seed 1), graded `sampled`. A fault found before the cap still refutes.

**A rotated network that comes with its specification** (a `SPEC` with `rotate`, or a folder quill
built rotated) is proved in two layers. The logic, in the field basis where arithmetic is exact,
gives the six results above. Then the rotated float64 build is enumerated in the rounded regime and
compared with the logic state by state: `rotated.holds`, `seed`, `states`, `transitions`,
`complete`, `closure_residual`, `kept_apart`, `max_field_gap`, `min_distance`, `noise`,
`difference`, grade `checked`. The overall grade is then `checked`, and the first line says why. A
fault of the float64 build refutes. A build that does not close as the logic does, because near
states were kept apart where the logic leaves no room for rounding, is `undecided` (grade
`undecided`), never `ok`. Thresholds exactly on an edge are exact in the logic but rounded off it in
the build, and a hold that doubles that error loses the bit: the latch `tau A <- 2*in.a - 1`,
`tau A <- 2*A - 2*in.a - 2*in.b - 1` proves in the logic, and rotated it forgets A after about 50
holds (0.1.0 passed its rotated build as checked). `--no-rotate` proves the logic alone.

**A folder quill did not build** (a planted-kit article) has no specification. Its rules are
recovered from the weights (pairs of neurons that form a threshold or a clear) and named through
the field directions in `manifest.json`. A rotated one is graded `checked`.

Also in the result: `name`, `input`, `options` (`cap`, and `tolerance`: the 1e-9 of the rounded
regime), `basis`, `arithmetic` (`exact` when no operation of any state update rounded, by the
processor's inexact flag read around every step; `rounded` otherwise), `decisions` (how many
deciding states emit and hold, the smallest |margin|, the distinct margins to 1e-9, and
`arithmetic`: whether computing the margins rounded; a state decides if some token leads to it),
`rules` (per rule: how often it fires, the values of z seen, its smallest distance), `fields` (up to
6 values each field takes), `machines` (per `machine`: `reached`, `not_reached`, and `complete`,
which says whether "not reached" is final), `state_table` (every state with its fields, decision,
margin, where each token leads and a shortest path to it; present when there are at most 64 states;
`--states` prints it as text), `evidence` (at the cap), `rotated` (two layers), `warnings`. The
values of z and of the fields are as they are when the arithmetic is exact, and to 1e-9 when it
rounds; the report prints a number in full wherever 1e-9 would hide what decides a verdict (a z of
1 - 2^-40 is never shown as 1, nor a distance of 2^-40 as 0).

Status: `ok` when every claim holds, `refuted` when one fails, `undecided` at the cap (or when the
other side of `--equiv` stopped at the cap, or when a rotated build does not close as its logic
does).

### 4.3 `quill run SPEC|DIR` with tokens

Runs the network from its start state `r0`, one step per token.

| option | meaning |
|---|---|
| `--tokens "zero one, 1"` | token names or positions, separated by spaces or commas. A name wins over a position |
| `--tokens-file F.i32` | token positions as little-endian int32 |
| `--random N [--seed S]` | N uniform random tokens from quill's own generator (seed default 1) |
| `--repeat K` | the token sequence K times over, the state carried on. With `--random`, the same N tokens each time. Steps in all: at most 2^40 |
| `--start F.f32 [--start-row R]` | start from row R (default 0) of a file of float32 rows of `width` values, instead of `r0` |
| `--dump OUT.f32` | write the state after every token as float32, `width` values per step |
| `--margins OUT.f64` | write every margin as float64 |
| `--show N` | list the first N steps (default 16; at most 1,000,000, or `all`; with `--json` at most 100,000) |

Exactly one of `--tokens`, `--tokens-file`, `--random` is required.

- Result: `name`, `steps`, `emit`, `hold`, `margin_min`, `margin_max`, `start` (`r0` or `file`),
  `shown`, `rows` (the first `--show` steps: `step`, `token`, `margin`, `decision`), `final_state`
  (field values), `final_state_sha256` (of the final state's `width` float64 values), and
  `tokens_sha256`, `margins_sha256`, `decisions_sha256` over the **whole** run (int32 tokens,
  float64 margins, one byte `0` or `1` per decision; all little-endian), so two runs can be compared
  by hash.
- `meta.steps_per_second` and `meta.seconds` time the stepping and the margins only: not reading,
  hashing, printing or writing. One thread. `meta.kernel` says which code path ran. A run whose
  stepping takes under a millisecond reports no speed (`steps_per_second` is null).

### 4.4 `quill explain SPEC|DIR`

Prints the explanation as Markdown (the content of EXPLAIN.md); `--json` gives the same content as
data (`fields`, `tokens`, `rules`, `neurons`, `readout`, `notes`, `warnings`, `expansions`,
`expanded`) plus the Markdown under `markdown`.

- **Fields:** coordinate, role (the constant, input, state, unused), start value, which tokens add
  to it, which rules write, clear and read it.
- **Tokens:** what each adds.
- **Rules:** each with its source line, its meaning (the author's comment, or quill's own words for
  a shorthand rule), the statement that wrote it, and **when it fires**, read off the weights
  alone with every field taken as 0 or 1: `on one when A=0`. A case in which z would land
  strictly inside (0, 1) is flagged as a possible ramp; `prove` says whether it is reachable. In an
  axis-aligned network the weights are read as stored and nothing is tolerated (a z of 1 - 2^-40 is a
  possible ramp, not firing); in a rotated one they are read off along the field directions, with
  rounding, and a z within 1e-9 of an edge counts as on it.
- **Neurons:** one line each: its rule and half, its argument z, what it writes, when it is active.
- **Readout:** the margin and the two rows.
- **Expansions:** each shorthand statement and the primitive statements written for it, then the
  whole network in primitive statements, with the rotation it was built with.

On a folder quill built, the specification kept in `manifest.json` is rebuilt and compared with the
weights bit for bit before it is trusted. On any other folder the rules are recovered from the
weights and numbers are shown rounded to 1e-9.

### 4.5 `quill check-kit DIR [--spec SPEC]`

Conformance against a recurrent article folder of the planted kit. It loads `weights.npz`, runs
`tokens.i32` session by session, and compares with the article's own dump (`probe_dump.meta.json`,
`session_start.f32`, `probe_hidden.f32`, `probe_dump.jsonl`; `readout.f32` when present). A session
starts from `r0` when its row of `session_start.f32` is the float32 copy of `r0` bit for bit (true
for every article but the session one), and from that float32 row otherwise.

An article folder is a network folder (section 6) together with a recorded run of the network.
`probe_dump.meta.json` gives `rows` and `sessions` (the rows are split evenly over the sessions);
`tokens.i32` holds one token position per row (little-endian int32, session after session);
`session_start.f32` holds one start state per session and `probe_hidden.f32` the state after every
row (float32, `width` values per state); `probe_dump.jsonl` holds one JSON object per row with
`l_emit_re`, `l_hold_re` and the logged 8-bit `margin`; `readout.f32`, when present, is `Wout` in
float32. The kit's articles are not part of this repository.

| check | compares | limit |
|---|---|---|
| `hidden` | quill's float64 state with `probe_hidden.f32` | 1e-6 |
| `margin_re` | quill's float64 margin with `l_emit_re - l_hold_re` of `probe_dump.jsonl` | 1e-4 |
| `margin_q8` | the 8-bit margin computed from quill's own states (cast to float32) with the logged `margin` | 1e-4 |
| `margin_q8_file` | the 8-bit margin computed from `probe_hidden.f32` with the logged `margin` | 1e-9 |
| `margin_re_file` | the float32 readout of `probe_hidden.f32` with `l_emit_re - l_hold_re` | 1e-9 |
| `readout_f32` | `readout.f32` is the float32 copy of `Wout` (when the file is there) | identical |
| `spec_decisions` | with `--spec`: the specification, built axis-aligned and run beside the article from its own start state in every session, makes the same decision on every row | every row |

The 8-bit copy is the kit's: blocks of 32 values, each scaled by the block's largest magnitude
over 127, rounded half to even, then the float32 readout. `margin_q8_file` tests that rule exactly.
`margin_q8` has the looser limit because the session article's start states are only known in
float32, so quill's float32 states can differ from the dump's in the last bit.

Status `ok` when every check passes, `refuted` otherwise; grade `checked`. It also checks that
`weights.npz` has the sha256 that `network.json` records. The result adds `start_states`,
`float32_identical` (of `float32_values`), `margin_q8_rows_off`, and with `--spec` a `spec` block
with `first_difference_row`.

### 4.6 `quill examples [NAME] [--write DIR]`, `quill npz FILE`

- `quill examples` lists the specifications inside the executable, `quill examples NAME` prints one
  (NAME or its file name), `--write DIR` writes them as files (an existing file that differs is
  left alone, `E_EXISTS`). `specs/` in this repository is that output.
- `quill npz FILE` lists the arrays of a numpy `.npz`: name, element type, shape, stored or deflate,
  count, nonzeros, range, the first four values and the sha256 of the data.

### 4.7 `quill --selftest`, `--about`, `--mcp`

- **`--selftest`** runs 64 checks and writes no file: known answers from zlib and numpy
  (`selftest_vectors.h`); 25 faulty specifications and their messages; the language (expressions,
  lists, `in.TOKEN`) and its printed-back form; the kernel against its written definition; the five kit articles built, proved
  exact, byte-compared with the kit's own weight files (their hashes are inside the executable), and compared with the planted truth on
  20,000 tokens, axis-aligned and rotated; ten equivalences; 200 random machines (2 to 8 states, 2
  to 6 tokens, written with explicit moves, `*`, unlisted pairs that hold, several tokens on one
  line and a random start state) simulated on 1,500 random tokens each and each proved equivalent
  to its table; the three planted faults; the four near-edge drifts that 0.1.0 passed as `ok`, each
  refuted at its first step with z and the field in full, and the same drift with rounded arithmetic
  and in a rotated network without its specification; a threshold 2^-40 outside (0, 1), which
  proves; thresholds exactly on an edge, whose rotated build is undecided; the counter; the cap; the
  readout's inexact flag; receipts; `run` and `explain`; the MCP server. If a `specs/` folder sits
  beside the executable it must equal the examples inside it (without one, that check is skipped
  and 60 run). Status `ok` or `refuted`, grade `checked`, exit code 0 or 1.
- **`--about`** prints one JSON object (with or without `--json`): `organ` ("quill"), `version`,
  `path`, `family`, `purpose`, `stage`, `verbs` (each with `verb`, `what` and `example`), `mcp`
  (`command`, `args`, `tools`, `register`), `health` (`ok` and `detail`: it builds and proves the
  example R1_plain), `docs` (the README beside the executable), `tape` (null: quill keeps no log).
  Exit code 0, or 2 when the health check fails.
- **`--mcp`** is an MCP server on stdio: newline-delimited JSON-RPC 2.0 with `initialize`, `ping`,
  `tools/list` and `tools/call`; notifications get no answer. Four tools, each on **inline
  specification text**:

| tool | arguments | returns |
|---|---|---|
| `quill_build` | `spec`, and optionally `out` (a folder to write), `force`, `rotate`, `no_rotate` | the build answer |
| `quill_prove` | `spec`, and optionally `equiv` (machine JSON text or another specification's text), `cap`, `rotate`, `no_rotate` | the prove answer |
| `quill_run` | `spec`, and `tokens` or `random` (up to 2^32, with `seed`), optionally `repeat` (up to 2^32), `show` (at most 1,000), `rotate`, `no_rotate` | the run answer |
| `quill_explain` | `spec`, optionally `format` (`markdown`, the default, or `json`), `rotate`, `no_rotate` | the Markdown, or the explain answer |

  A tool's result is one text block holding the same JSON answer the command line prints with
  `--json` (for `quill_explain` in Markdown format: the Markdown itself). `isError` is true only for
  status `error`, whose answer names the verb (`prove`, not `quill_prove`). An argument a tool does
  not have is refused. The language reference is in `quill_build`'s description. Register with
  `claude mcp add quill -- path/to/quill.exe --mcp`; `quill --about` prints that line with the
  executable's full path (`mcp.register`).

## 5. The answer

### 5.1 One JSON object

```
{"quill": "0.1.2", "verb": "prove", "status": "ok", "grade": "proved",
 "result": {...}, "text": "prove parity: ok, grade proved",
 "receipt": {"id": "quill1:881624541aaa", "input_sha256": "...", "result_sha256": "..."},
 "meta": {"ms": 0.16}}
```

- `status`: `ok`, `refuted`, `undecided` or `error`.
- `grade`: `proved`, `checked`, `sampled`, `undecided`, or `none` for verbs that claim nothing
  (build, run, explain, examples, npz) and for errors.
- `result` is a function of the inputs' bytes and the options only. It holds no path, no time and
  no speed: those are in `meta` (`ms`; for run `seconds`, `steps_per_second`, `kernel`; the paths
  written), which is never hashed.
- `text` is the first line of the plain-text report.
- An error has `error` in place of `result` and no receipt:
  `{"code", "message", "line", "column", "source", "hint", "count", "all": [...]}`; `all` lists
  every error found, in the same form. Lines and columns count from 1; `null` when they do not apply.

Error codes: `E_SPEC` (the specification), `E_ARGS` (the command line or a tool's arguments),
`E_IO` (a file cannot be read or written; the message gives the system's reason), `E_FORMAT` (a
file is not what it should be), `E_UNSUPPORTED` (a sequence network with attention heads),
`E_LIMIT` (too large, or out of memory), `E_EXISTS` (would overwrite), `E_EQUIV` (the machine
file), `E_INTERNAL`.

### 5.2 Grades

| grade | meaning |
|---|---|
| `proved` | Every successor of every enumerated state is, bit for bit, an enumerated state. The kernel is a deterministic function on float64 states, so by induction no token sequence of any length leaves the set, and what was checked at every state and token holds for ever. When `arithmetic` is `exact`, no operation of any state update rounded and nothing was tolerated: the claims about states and thresholds are also true of the stored weights taken as real numbers, in any order of evaluation (`decisions.arithmetic` says the same of the margins) |
| `checked` | The arithmetic rounds and the set closed only within 1e-9 (a rotated basis rounds at every step), or the claim is a comparison with given data (`check-kit`). Everything was verified at every state and token, a state was merged with a near one only where its noise margin shows that the gap changes nothing (4.2), and the largest gap is reported. It is not a proof for runs of unbounded length: rounding can accumulate along directions nothing rewrites |
| `sampled` | Verified on random token sequences only; the seed and the count are stated. Evidence, attached to an `undecided` answer |
| `undecided` | The enumeration stopped at the cap before closing, or a rotated build did not close as its logic does |

A claim that fails is graded by its witness: `proved` when the witness was computed without
rounding, `checked` otherwise. The answer's grade: for status `ok`, the weakest grade among its
claims; for `refuted`, the grade of the first claim that fails, in the order of the report; for
`undecided`, `undecided`.

### 5.3 Receipts

- `result_sha256` is the SHA-256 of the RFC 8785 canonical JSON of `result` (keys sorted, numbers in
  shortest form). It can be recomputed from the printed answer alone.
- `input_sha256` is the SHA-256 of the canonical JSON of the verb's input description: the verb,
  the sha256 of each input file (a specification's bytes; a folder's `weights.npz`, `network.json`
  and `manifest.json`), and the options that can change the result. The plain sha256 of each input
  file is also inside `result.input`, where `sha256sum` can check it.
- `id` is `quill1:` and the first 12 hex digits of the SHA-256 of
  `{"input_sha256", "quill", "result_sha256"}` in canonical form.

The same inputs give the same receipt on every run and every machine.

### 5.4 Exit codes

`0` ok. `1` refuted: a proof found a fault, a check-kit comparison failed, or a self-test check
failed. `2` error: bad arguments or bad input (and `--about` when its health check fails). `3`
undecided.

## 6. The file format

A network folder is **exactly the planted kit's**, so quill and the kit read each other's folders.

| file | content |
|---|---|
| `weights.npz` | numpy `.npz` with float64 arrays `E` (tokens x width), `W1` (neurons x width), `b1` (neurons), `W2` (width x neurons), `r0` (width), `Wout` (2 x width, rows `[hold, emit]`), `bout` (2, zeros) |
| `network.json` | the kit's keys (`name`, `family` "R", `n_embd`, `n_input_vocab`, `tokens`, `n_neurons`, `n_weights`, `weights_sha256`, ...) and a `quill` block: `version`, `basis`, `rotate_seed`, `spec_sha256` |
| `manifest.json` | the kit's keys: `fields` (each named field's direction, a list of `width` numbers), `true_shams` (up to four directions nothing reads or writes), `margin`, ...; and a `quill` block: `version`, `basis`, `rotate_seed`, `spec_sha256`, `weights_sha256` and the specification text (`spec`) |
| `EXPLAIN.md` | the explanation (not read back by anything) |

- **Writing.** `weights.npz` is written the way numpy writes it: stored entries, ZIP64 local
  headers, 1980-01-01 stamps, `.npy` format 1.0 with the header padded to 64 bytes. For each of the
  kit's articles R1 to R5, `quill build specs/<name>.spec` gives a file byte-identical to the kit's
  own article built axis-aligned and saved by `numpy.savez` (numpy 2.4.2, on Windows); the
  self-test holds the five hashes.
- **Reading.** Stored and deflate entries, ZIP64 sizes, `.npy` formats 1 to 3, float64 or float32,
  either byte order, C or Fortran order. The CRC-32 of every entry is checked. `network.json` must
  record the sha256 of the `weights.npz` beside it, when it records one. A folder whose
  `network.json` describes attention heads, or whose arrays include heads, is refused with
  `E_UNSUPPORTED`.
- **Rotation.** The rotation is not numpy's. quill uses the kit's recipe (the Q factor of a
  Gaussian matrix, columns signed so that R's diagonal is positive) on its own random stream
  (xoshiro256** seeded through splitmix64, Gaussians by the polar method), computed with `+ - * /`
  and `sqrt` only, so a seed gives the same bits on every machine. To compare with a kit article,
  use the article's own weight file, or build axis-aligned.
- quill reads a folder's `manifest.json` for field names and directions, and nothing else from
  the kit's other files, except in `check-kit`.

## 7. Limits, and what is not in 0.1

Limits of a specification: width, fields, tokens and states of one machine up to 4,096; 1,000,000
neurons and 8,000,000 entries in W1. Limits of a folder read: width and tokens up to 65,536,
1,000,000 neurons. Token and state names are ASCII. Every named field must be 0 or 1 in every
reachable state (exactly, when the arithmetic is exact): a design that keeps other values in a field
is refused by `prove`. Windows x64 only.

Planned, not in this version:

- attention heads and sequence networks (the kit's S articles are refused with `E_UNSUPPORTED`);
- the post's own multi-field file format and its looped transformers (`quill npz` can list the
  post's `maze.npz`; nothing more);
- a stencil backend for lattice rules;
- bit-plane backends;
- planting options beyond `rotate`;
- an AVX path for the dense kernel, and a proof with error bounds for rotated networks.

## 8. Rerunning the acceptance tests

From the repository root, with the folder of the planted kit in the variable `QUILL_KIT`
(`$QUILL_KIT/articles/R1_plain` is one of its article folders, section 4.5). Tests 3, 4 and 6 need
the kit's articles, which are not part of this repository; the others need only this repository.
Under each command is what it printed when version 0.1.2 was tagged (speeds vary with the
machine's load).

`bash tests/acceptance.sh` (Git Bash) runs tests 2 to 7 after a build: it prints each command with
its output and exit code, and ends with how many checks came out as expected (exit code 0 only when
all did). It reads `QUILL_KIT` from `tests/paths.local.env` (copy `tests/paths.example.env`; git
ignores the copy) or from the environment.

1. Build with no warnings at `/W4` (`/WX` turns any warning into a failed build). From an empty
   `obj/`, the compiler prints each source's name and nothing else:

   ```
   .\build.bat
   base.cpp
   npz.cpp
   net.cpp
   spec.cpp
   explain.cpp
   prove.cpp
   verbs.cpp
   kit.cpp
   examples.cpp
   selftest.cpp
   mcp.cpp
   quill.cpp
   OK: quill.exe
   ```

2. The self-test (the five kit specifications, 200 random machines, and the rest):

   ```
   quill --selftest
   SELFTEST: ALL PASS (64 checks)
   ```

3. check-kit on the nine recurrent articles, `quill check-kit $QUILL_KIT/articles/NAME` for
   each NAME; each is `ok`, exit code 0. The first line of each answer:

   ```
   check-kit R0_memoryless: ok, grade checked: 8,000 rows in 40 sessions; hidden 2.5e-08, margin 1.2e-07, 8-bit margin 8.9e-16
   check-kit R1_plain: ok, grade checked: 8,000 rows in 40 sessions; hidden 1.4e-08, margin 9.7e-08, 8-bit margin 0
   check-kit R1_rewired: ok, grade checked: 8,000 rows in 40 sessions; hidden 1.4e-08, margin 7.3e-08, 8-bit margin 0
   check-kit R2_quotient: ok, grade checked: 8,000 rows in 40 sessions; hidden 1.5e-08, margin 2.6e-07, 8-bit margin 8.9e-16
   check-kit R3_overcomplete: ok, grade checked: 8,000 rows in 40 sessions; hidden 2.9e-08, margin 4.0e-07, 8-bit margin 2.7e-15
   check-kit R4_two_bits: ok, grade checked: 8,000 rows in 40 sessions; hidden 2.4e-08, margin 1.6e-07, 8-bit margin 1.3e-15
   check-kit R5_redundant: ok, grade checked: 8,000 rows in 40 sessions; hidden 2.6e-08, margin 1.4e-07, 8-bit margin 0
   check-kit R9_session: ok, grade checked: 8,000 rows in 40 sessions; hidden 2.1e-07, margin 1.4e-06, 8-bit margin 4.7e-07
   check-kit C1_even: ok, grade checked: 8,000 rows in 40 sessions; hidden 2.8e-08, margin 8.8e-08, 8-bit margin 5.6e-16
   ```

4. The R1_plain specification, built axis-aligned, against the kit's R1_plain on every token:

   ```
   quill check-kit $QUILL_KIT/articles/R1_plain --spec specs/R1_plain.spec
   check-kit R1_plain: ok, grade checked: 8,000 rows in 40 sessions; hidden 1.4e-08, margin 9.7e-08, 8-bit margin 0; specification R1_plain decides alike on 8,000 of 8,000 rows
   ```

   For every token sequence, not only that file's:

   ```
   quill prove specs/R1_plain.spec --equiv $QUILL_KIT/articles/R1_plain
   prove R1_plain: ok, grade checked (this network closes bit for bit; the other side of the equivalence closes only to a tolerance of 1e-9 in float64)
     equivalence  the network R1_plain and the folder's network R1_plain make the same decision after every token sequence (2 reachable pairs of states; 2 states of the other side)   [checked]
   ```

5. The three planted faults, each refused with the state, the token and the rule (exit code 1):

   ```
   quill prove specs/fault_ramp.spec
   prove fault_ramp: REFUTED, grade proved
     exactness    FAILS: rule 1 (line 9: tau A <- in1 - A - 0.5) has z = 0.5, strictly inside (0, 1), in state {all fields 0} on token one (reached by: the start state)   [proved]

   quill prove specs/fault_two_setters.spec
   prove fault_two_setters: REFUTED, grade proved
     cleanliness  FAILS: field A is 2, not 0 or 1, after token one in state {A=1} (reached by: one): A = 1 from rule 1 (line 9: tau A <- 2*in1 - 0.5) at z = 1.5 + 1 from rule 2 (line 10: tau A <- 2*A - 2*in0 - 0.5) at z = 1.5   [proved]

   quill prove specs/fault_never_cleared.spec
   prove fault_never_cleared: REFUTED, grade proved
     cleanliness  FAILS: field N is 2, not 0 or 1, after token one in state {N=1} (reached by: one): N = 1 kept from before (nothing clears N) + 1 from rule 1 (line 9: tau N <- 2*in1 - 0.5) at z = 1.5   [proved]
   ```

6. Speed on R1_plain, the kit's rotated article, its own 8,000 tokens a thousand times over:

   ```
   quill run $QUILL_KIT/articles/R1_plain --tokens-file $QUILL_KIT/articles/R1_plain/tokens.i32 --repeat 1000 --show 0
   run R1_plain: 8,000,000 steps, 3,520,000 emit, 4,480,000 hold, margins from -4 to 4
     8,898,734 steps per second (8,000,000 steps in 0.899004 s, one thread, dense kernel)
   ```

   (8.8 to 8.9 million steps per second in the runs for 0.1.1, and 5.4 to 8.6 million over those made while building 0.1.0, on a shared machine; the brief asks for 2 million.)

7. A counter modulo 3 with reset:

   ```
   quill prove specs/counter_mod3.spec
   prove counter_mod3: ok, grade proved
     reachable    3 states, 12 transitions; the set is closed bit for bit, in exact arithmetic   [proved]
     ...
     machine C    3 of its 3 states are reachable
   ```

## 9. Where things are

```
build.bat             the whole build
quill.cpp             arguments, help, --about
app.h, verbs.cpp      the verbs and how an answer is printed
spec.h, spec.cpp      the language, the assembler, the rotation, the model
explain.cpp           the explanation, network.json, manifest.json
prove.h, prove.cpp    the enumeration and the six results
net.h, net.cpp        the seven arrays, the folder, the kernel
npz.h, npz.cpp        CRC-32, inflate, ZIP, .npy
kit.cpp               check-kit
mcp.cpp               the MCP server
examples.cpp          the specifications inside the executable, and the language card
selftest.cpp          the self-test;  selftest_vectors.h: its known answers from zlib and numpy
quill.h, base.cpp     formatting, SHA-256, files, JSON
specs/                the examples as files (quill examples --write specs)
tests/                acceptance.sh (tests 2 to 7 of section 8);  paths.example.env: where it finds the kit
docs/BRIEF.md         what was asked;  docs/devlog.md: what was decided, and why
```

MIT license, see `LICENSE`. Owner: Bo Chen.
