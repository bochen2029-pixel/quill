# Changelog

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
