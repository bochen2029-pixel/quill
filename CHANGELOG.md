# Changelog

## 0.1.3 (2026-11-26)

The two limits that the outside review of 0.1.1 raised, and that 0.1.2 listed as known.

**Numbers float64 cannot hold are named.** A new warning, `W_ROUNDED_CONSTANT`, lists every number on a
line that float64 cannot hold as written, in the text and in JSON (`values`: `what`, `written`,
`stored`, `difference`). It covers:
- a literal (`0.1`, `2.2`, `1e-12`);
- a start value;
- a coefficient whose exact numbers combine with rounding, such as `8/3`, or the review's
  `-4.5 + 1/4503599627370496`, which is stored as `-4.5`. 0.1.2 stored, proved and printed this back as
  `- 4.5` without a word.

Whether a literal is exact is decided with integers only, by comparing its decimal value with the exact
decimal expansion of the double that holds it. Whether a combination rounds is read from the processor's
inexact flag, as `prove` reads it. Written minus stored is exact for a literal and first-order for a
combination.

The stored values, the weights, the proofs and the grades do not change: 1,500 random specifications
full of such numbers, half of them rotated, build to the same weights as in 0.1.2. Two shipped examples
now carry the warning, and should: `R5_redundant` (weights 8/3) and `fault_edge_decimal` (1e-12).

**Rotated proofs at width 4096 in a fraction of a second.** Building a rotated network computed the
whole orthogonal matrix Q by Gram-Schmidt: O(width^3) with poor memory access. That took minutes at
width 4096, where the review saw it run past seven minutes and the mutation fuzzer timed out. A build
uses only the columns of Q that its weights touch, plus four spares for the manifest. Gram-Schmidt's
column j depends on columns 0 to j-1 alone, so computing those columns only gives them bit for bit.
The Gaussian matrix is still drawn whole and in the same order, skipping the logarithm and square root
for draws that land in no kept column.

Nothing about the proof changed; only work whose result was never used was removed. The checks:
- the self-test compares the computed columns with the whole matrix's, bit for bit;
- 18 rotated builds and proofs (R1_plain at widths 256, 512 and 1024, and every shipped
  specification) give the same weights hash, result hash, status, grade and exit code as 0.1.2;
- the large cases were compared with 0.1.2 run to the end, and are the same in result hash, status,
  grade and exit code:
  - R1_plain at width 2048: 0.1.2 took 174 s, 0.1.3 0.06 s;
  - R1_plain at width 4096: 0.1.2 took 1,777 s, 0.1.3 0.2 s;
  - the mutation fuzzer's `width 4096` mutant of `fault_creep_up`, the case it had timed out on:
    0.1.2 took 1,634 s, 0.1.3 0.2 s.

| R1_plain, rotated | 0.1.2 | 0.1.3 |
|---|---|---|
| width 1024 | 2.8 s | 0.02 s |
| width 2048 | 174 s | 0.06 s |
| width 4096 | 1,777 s | 0.2 s |

**Self-test**: 68 checks. W_ROUNDED_CONSTANT axis-aligned and rotated; its silence on specifications
whose numbers are exact; the rotation's columns against the whole matrix; and a rotated proof at width
4096.

## 0.1.2 (2026-11-26)

A crash fix in `prove`, found by an outside review.

**The defect.** When the start state itself is not clean (a named field that is not 0 or 1, as with
`init A=2`), `prove` expands no state, so it evaluates no threshold and the index of the closest
threshold stays -1. The JSON branch of the exactness result checked for that; the text branch did not,
and read the per-rule table at index -1. The result was undefined behaviour:
- on Windows, a crash (access violation) in most runs;
- otherwise garbage in the report, or a spurious `E_LIMIT` "out of memory";
- through `--mcp`, the server could die.

The verdict, when one was printed, was not affected: such a network is refuted by its start state.

**The fix.** One line. With no threshold evaluated, the exactness line now says "no threshold was
evaluated: no state was expanded", and the start state's field is reported as the fault.

**Checked.**
- An audit of every other index in `prove` that can be -1 when nothing was evaluated or expanded:
  - the JSON branch of the exactness result, the noise result, the state table, the deciding states
    and both sides of `--equiv` were already guarded;
  - no other unguarded index was found.
- Under AddressSanitizer, 19 edge cases. On 0.1.1, 12 memory errors: every `prove` of an unclean start,
  whether axis-aligned, rotated, from a network folder, with `--json`, `--states`, `--equiv` or
  `--cap 1`, and over `--mcp`. On 0.1.2, none. The self-test under the sanitizer is clean.
- Three self-test cases (64 checks in all): an unclean start axis-aligned, rotated, and the review's
  own `crash3.spec`, each refuted cleanly in the text and in the JSON. With 0.1.1's `prove.cpp`, the
  sanitizer stops at the first of them.
- The review's own fuzzers, run on Windows against this build:
  - 20,000 generated specifications against its exact-rational reference model, with 0 differences
    in the 19,749 that stay in exact arithmetic. The released 0.1.1 crashed on 659 of the same specs
    and refused 8 more with "out of memory".
  - 4,000 specifications in rounded arithmetic checked by an independent float64 kernel: 0 findings.
  - 3,000 random `--equiv` machines: 0 problems.
  - 7,500 text mutants, 1,500 of them under the sanitizer: no crash and no sanitizer error.

**Known, and not changed in this version** (both reported by the same review; both addressed in 0.1.3):
- `prove --rotate` grows about sixteen times slower per doubling of the width. At the largest width a
  specification allows, 4096, it does not finish in minutes. The mutation fuzzer's only timeouts were
  `width 4096` with `--rotate`.
- A constant that float64 cannot represent is rounded where it is stored, without a warning. For
  example `- 4.5 + 1/4503599627370496` is stored and printed back as `- 4.5`. The proof is about the
  stored network, as section 4.2 says, but nothing points out the difference.

## 0.1.1 (2026-11-26)

A correction to `prove`, found by an independent adversarial review.

**The defect.** 0.1.0 compared states, fields and threshold arguments to a tolerance of 1e-9, in every
case. A successor within 1e-9 of a known state was merged with it. A threshold argument inside (0, 1)
by less than 1e-9 counted as outside. A field within 1e-9 of 0 or 1 counted as clean.

Together these let a deterministic fault pass as `ok`. The example is a set/reset latch whose hold rule
has the bias `1 + 2^-40` where `1` was meant. 0.1.0 reported it `ok, grade checked` and, with
`--equiv`, equivalent to the ideal latch. Run, it forgets its bit after 40 idle tokens: each step
doubles its distance from 1. No operation rounded (2^-40 is a power of two), so the grade line's "rounding
keeps it short of a proof" named the wrong cause. `W_EDGE` said the threshold landed "exactly on an
edge", where it sat 2^-40 inside.

**The fix.** `prove` now first enumerates in the exact regime. When no operation of any state update
rounds (axis-aligned, weights exact in binary):
- nothing is tolerated: states are the same only bit for bit;
- a threshold argument strictly inside (0, 1) by any amount is on its ramp;
- a named field must be exactly 0 or 1.

At the first step that rounds, the enumeration starts over in the rounded regime:
- A near successor is merged with a known state only where the merge provably changes nothing:
  - the state's own noise margin is at least 2e-9 on every token;
  - and, axis-aligned, the gap lies in fields that a clear rewrites.
- Any other near successor is kept as a state of its own, so a drift is followed until it shows as a
  fault or the cap stops the enumeration. `reachable.kept_apart` counts these.
- A threshold argument inside (0, 1) is on its ramp unless the rounding of that very evaluation explains
  it. `exactness.inside_by_rounding` gives the deepest such case and its bound.

**Visible changes.**
- The four planted faults of the review are examples and self-test cases: `fault_edge_drift`,
  `fault_edge_decimal`, `fault_creep_up` and `fault_creep_down`. Each is now refuted at the first step
  where z leaves 0 and 1, with the shortest path there, the rule, the state and the exact value of z.
- `W_EDGE` and `W_MARGIN_ZERO` say how far from the edge a value is, and the report prints a number in
  full wherever rounding to 1e-9 would hide what decides a verdict.
- A rotated build that cannot close as its logic does, because near states had to be kept apart, is
  `undecided` instead of failing.
- The self-test has 61 checks (52 in 0.1.0).
- Every receipt id changes, because the id includes the version.
- README: the comparison rules are stated in section 4.2; the paper is linked; the build is shown as
  `.\build.bat`.

**Unchanged.** Every shipped specification gives the same verdict as in 0.1.0, with the same states and
transitions, apart from the four new planted faults. Grades `checked` for rounded and rotated networks keep their meaning: verified at every
state and token, not a proof for all time.

## 0.1.0 (2026-11-26)

First release: write a small recurrent network as a specification, build its weights by formula with
a line of meaning for every neuron, prove it exact by enumerating every reachable state, run it, and
check it against the planted kit's articles. One self-contained Windows executable, C++20, with an MCP
server.
