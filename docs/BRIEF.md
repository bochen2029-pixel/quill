# quill: build brief (v0.1)

**For:** the agent building this tool. **From:** the session that designed it (2026-11-26).
**Owner's standing rules for tools are in section 7. Read them before writing code.**

## 1. What quill is

A single C++ executable that helps a language model (or a person) **write, prove and run small
neural networks whose every weight is set by formula**, with no training. The model writes a short
text specification in terms of named fields and threshold rules. quill expands it into explicit
weights with a line of meaning for every neuron, proves that the network is exact, and runs it fast.

Why it exists: a 2026 post, "Looped Weight-Space Programming" (MBZUAI), showed a language model
writing small transformers by formula that beat trained ones on long algorithmic tasks. Doing that
by hand, the model is good at choosing the algorithm and the meaning of each number, and bad at long
exact arithmetic, bookkeeping, and seeing its own mistakes. quill does the exact part and returns a
graded result, as the owner's math tool Abacus does for arithmetic.

A working Python prototype of everything in this brief exists and is your reference and your oracle:
the planted kit at `$QUILL_KIT` (read its `README.md` first, then `kit/asm.py`, `kit/articles.py`, `kit/run.py`,
`kit/compile.py`). Treat that folder as **read-only**. quill replaces its assembler, compiler and
runner with C++; it does not replace the kit's articles.

## 2. The network model (v0.1: the recurrent class)

- State `r`, a vector of `width` float64 numbers. Named **fields** are coordinates of it. The field
  `one` always exists and always holds 1 (it carries every constant; there is no readout bias).
- One step per input token: `r += E[token]`, then `r += W2 · relu(W1 · r + b1)`.
- Readout: two rows `[hold, emit]`; the **margin** is `emit − hold`, a linear function of `r`.
- A threshold is `tau(z) = relu(z) − relu(z − 1)`: 0 for z ≤ 0, 1 for z ≥ 1. Rules are written so that
  z lands at 1.5 or above, or at −0.5 or below. Then small errors cannot accumulate.
- Optionally the whole network is expressed in a rotated basis (an orthogonal matrix Q from a seed),
  so that no coordinate means anything and each field lies along a known direction (a column of Q).

File format of a built network: **exactly the kit's**, so the two interoperate. A folder with
`weights.npz` (numpy `.npz`, stored uncompressed; float64 arrays `E`, `W1`, `b1`, `W2`, `r0`,
`Wout`, `bout`), `network.json`, `manifest.json`. See `kit/asm.py` (`save`, `load`) and any folder
under `$QUILL_KIT/articles/`. Reading `.npz` means reading a ZIP with stored entries and the
`.npy` header; support deflate too if you can do it cleanly, since the post's own files use it.

## 3. The specification language

Line-oriented text, `#` starts a comment. This is the whole of v0.1; improve the syntax if you find
a cleaner one, but keep it this small and say what you changed.

```
net     R1_plain
tokens  zero one two three
fields  in0 in1 in2 A                 # "one" is implicit
embed   zero in0                      # when this token arrives, add 1 to this field
embed   one  in1
embed   two  in2                      # a token with no embed line writes nothing
init    A=0                           # start values; one=1 is implicit
tau     A <- 2*in1 - 2*A - 0.5        # A += tau(expression); expression is linear in fields
tau     A <- 2*A - 2*in1 - 2*in0 - 0.5
clear   A in0 in1 in2                 # x += -relu(x) + relu(-x): the field is rewritten from zero
readout 8*A - 4*one                   # the margin; rows are -half and +half of it
rotate  101                           # optional seed for a rotated basis; omit for axis-aligned
width   32                            # optional; default = fields + 4 spares, rounded up to 8, at least 32
```

Statements that expand into the ones above (the expansion must be visible in `quill explain`):

```
latch   A set=<tokens> reset=<tokens> toggle=<tokens>     # a one-bit register; other tokens hold it
machine S states=3 init=0                                  # one-hot fields S.0 S.1 S.2
  on one:  0->1 1->2 2->0
  on zero: *->0                                            # unlisted (state, token) pairs hold
emit    S.1 S.2                                            # margin +4 in these states, -4 elsewhere
neuron  2*in1 - 2*A - 0.5 -> +1*A                          # one raw relu, for full control
```

The expansions are in `kit/articles.py` (`_toggle`, `_counter`) and `kit/compile.py`
(`compile_spec`, `compile_dfa`). Note the one trap they encode: all neurons read the state as it was
before the step, so one rule cannot read what another writes in the same step.

## 4. Verbs

Every verb takes `--json` and prints one JSON object; without it, short plain text.

| verb | does |
|---|---|
| `quill build SPEC -o DIR` | writes the network folder, plus `EXPLAIN.md`: the field layout and one line per neuron saying when it fires and what it writes |
| `quill prove SPEC\|DIR` | exhaustive analysis, see section 5 |
| `quill run DIR --tokens "0 1 1"` (or `--tokens-file F.i32`, `--start F.f32`, `--dump OUT.f32`) | runs it; prints margins and decisions; reports steps per second |
| `quill explain SPEC\|DIR` | prints the explanation |
| `quill check-kit DIR` | conformance against a kit article folder, see section 6 |
| `quill --selftest`, `quill --about`, `quill --mcp` | the family's standard surfaces (section 7) |

## 5. `prove`: the part a model cannot do in its head

For the recurrent class the reachable states are finite. Enumerate them by breadth-first search from
the start state under every token (two states are the same if they agree to 1e-9; stop with grade
`undecided` at a cap, default 200,000 states). Then report:

1. the number of reachable states and transitions;
2. **exactness:** for every reachable state, every token and every threshold, the argument z, and the
   smallest distance of any z from the open interval (0, 1). If any z falls inside it, the proof
   fails and names the state (as field values), the token and the rule;
3. **cleanliness:** every named field is 0 or 1 in every reachable state (or names the first that is not);
4. **noise tolerance:** the largest uniform error on the state's coordinates that provably cannot move
   any threshold across its edge, from the margins in (2) and the weights;
5. **equivalence** (`--equiv MACHINE.json`): the network and a given transition table accept the same
   decisions on every reachable path (a bisimulation), or a shortest token sequence on which they differ;
6. sizes: width, neurons, weights, nonzero weights, operations per step.

Each result carries a **grade** in Abacus's vocabulary (`proved`, `checked`, `sampled`, `undecided`)
and a receipt: the sha256 of the input and of the result.

## 6. Acceptance tests (I will rerun all of them)

1. `build.bat` produces `quill.exe` with no warnings at `/W4`.
2. `quill --selftest` passes. It must include: specifications equivalent to the kit's `R1_plain`,
   `R2_quotient`, `R3_overcomplete`, `R4_two_bits` and `R5_redundant` (write them under `specs/`),
   each built and proved exact; 200 random machines (2 to 8 states, 2 to 6 tokens) built from
   `machine` statements and checked against a plain simulation on 1,500 random tokens each.
3. `quill check-kit` on every recurrent article under `$QUILL_KIT/articles/` (`R0_memoryless`,
   `R1_plain`, `R1_rewired`, `R2_quotient`, `R3_overcomplete`, `R4_two_bits`, `R5_redundant`,
   `R9_session`, `C1_even`): load `weights.npz`, run `tokens.i32` session by session from
   `session_start.f32` (see `kit/build.py` and `kit/selftest.py` for the layout; for all but
   `R9_session` the float64 start state is `r0` in `weights.npz`), and reproduce `probe_hidden.f32`
   to 1e-6 and the `l_emit_re − l_hold_re` margins in `probe_dump.jsonl` to 1e-4. Also reproduce the
   logged 8-bit `margin` (blocks of 32 values scaled by the block's largest magnitude over 127,
   rounded half to even as numpy does; see `q8` in `kit/build.py`).
4. Your own `R1_plain` specification, built axis-aligned, makes the same decision as the kit's
   `R1_plain` on every token of that article's `tokens.i32`.
5. Planted faults, each of which `prove` must reject with a message naming the state, token and
   rule: a threshold whose argument lands on its ramp; two rules that can both set the same field in
   one step (the field reaches 2); a field that is never cleared and grows.
6. `quill run` on `R1_plain` does at least 2,000,000 steps per second on one thread.
7. `quill prove` on a counter modulo 3 with reset reports 3 reachable count states and grade `proved`.

## 7. The owner's rules for tools (not optional)

- **C++20 only**, MSVC 2022, one self-contained `quill.exe`, operating-system APIs only, no third-party
  libraries, static CRT. Mirror vramtop (`build.bat`, file layout, code conventions) and take the
  standard surfaces from there and from abacus: `--json`, `--selftest`, `--about` (a short
  card), `--mcp` (a stdio MCP server exposing build, prove, run and explain on inline specification text).
- The repository is `quill`: `README.md` written as the contract (what every verb promises),
  `LICENSE` (MIT, same holder line as vramtop's `LICENSE`), `.gitignore`, `build.bat`, `specs/`,
  `docs/devlog.md` written as the work happens. Local git, commits at each working milestone, tag
  `v0.1.0` when the acceptance tests pass. Push nothing.
- Headless command line only. No window.
- Work autonomously. Do not ask questions; decide, note the reason in the devlog, continue.

## 8. Machine rules (they bite)

- Windows 11. Two shells: Git Bash and PowerShell 7. **Write every path with forward slashes** in
  shell commands (`quill/build.bat`); Git Bash eats backslashes.
- **Heredocs are blocked by a hook.** Never put file content in a shell command. Create files with
  the Write tool and change them with the Edit tool. Read nopdocs' `PROMPT.md` once.
- Write only inside the repository. Everything else on the machine is read-only for you. No downloads,
  no GPU, no network.
- End commit messages with: `Co-Authored-By: Claude Fable 5.2 <noreply@anthropic.com>`
- Python 3.13 with numpy is installed and you may use it **only** as a throwaway oracle while
  testing (for example to read a `.npz`). Nothing in the tool may depend on it.

## 9. Not in v0.1 (list them in the README as planned)

Attention heads and sequence networks; the post's own multi-field file format and its looped
transformers (`$QUILL_KIT/third_party/loopedwsp/`, `kit/maze0.py`); a stencil backend for
lattice rules (`kit/maze_stencil.py`); bit-plane backends; planting options beyond `rotate`.

## 10. What to hand back

A short report: what was built; the output of each acceptance test; anything you changed from this
brief and why; what you would do next. State plainly anything that does not work.
