// quill · examples.cpp — the specifications that ship inside the executable. `quill examples`
// lists them, `quill examples --write specs` writes the files under specs/, and --selftest proves
// every one of them. The first five are the planted kit's recurrent articles, statement for statement.
#include "app.h"

namespace quill {

namespace {

const char* const kR1 = R"quill(# R1_plain: one bit, A, the parity of the ones since the last zero. The readout uses it.
# The planted kit's article of the same name (kit/articles.py, r1_plain), statement for statement:
# built axis-aligned it has the kit's weights exactly.
net     R1_plain
tokens  zero one two three
fields  in0 in1 in2 A
embed   zero in0
embed   one  in1
embed   two  in2                          # three embeds nothing: the network cannot see it
tau     A <- 2*in1 - 2*A - 0.5            # a one arrives while A is 0: A becomes 1
tau     A <- 2*A - 2*in1 - 2*in0 - 0.5    # A is 1 and neither a one nor a zero arrives: A stays 1
clear   A in0 in1 in2                     # every field is rewritten from zero at each step
readout 8*A - 4                           # margin +4 when A is 1, -4 when it is 0
)quill";

const char* const kR2 = R"quill(# R2_quotient: the count of ones since the last zero, modulo 3, one-hot in m0 m1 m2.
# The run length itself is nowhere in the state. The planted kit's article r2_quotient.
net     R2_quotient
tokens  zero one two three
fields  in0 in1 in2 m0 m1 m2
embed   zero in0
embed   one  in1
embed   two  in2
init    m0=1
tau     m0 <- 2*in1 + 2*m2 - 2.5          # a one moves the count on: 2 -> 0
tau     m0 <- 2*m0 - 2*in1 - 2*in0 - 0.5  # anything but a one or a zero holds it
tau     m1 <- 2*in1 + 2*m0 - 2.5          # 0 -> 1
tau     m1 <- 2*m1 - 2*in1 - 2*in0 - 0.5
tau     m2 <- 2*in1 + 2*m1 - 2.5          # 1 -> 2
tau     m2 <- 2*m2 - 2*in1 - 2*in0 - 0.5
tau     m0 <- 2*in0 - 0.5                 # a zero resets the count
clear   m0 m1 m2 in0 in1 in2
readout 8*m1 + 8*m2 - 4                   # emit unless the count is 0 modulo 3
)quill";

const char* const kR3 = R"quill(# R3_overcomplete: the count modulo 12, one-hot in c0 .. c11, read only through its value
# modulo 3. More is stored than the behaviour uses. The planted kit's article r3_overcomplete.
net     R3_overcomplete
tokens  zero one two three
fields  in0 in1 in2 c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 c11
embed   zero in0
embed   one  in1
embed   two  in2
init    c0=1
tau     c0 <- 2*in1 + 2*c11 - 2.5         # a one moves the count on
tau     c0 <- 2*c0 - 2*in1 - 2*in0 - 0.5  # anything but a one or a zero holds it
tau     c1 <- 2*in1 + 2*c0 - 2.5
tau     c1 <- 2*c1 - 2*in1 - 2*in0 - 0.5
tau     c2 <- 2*in1 + 2*c1 - 2.5
tau     c2 <- 2*c2 - 2*in1 - 2*in0 - 0.5
tau     c3 <- 2*in1 + 2*c2 - 2.5
tau     c3 <- 2*c3 - 2*in1 - 2*in0 - 0.5
tau     c4 <- 2*in1 + 2*c3 - 2.5
tau     c4 <- 2*c4 - 2*in1 - 2*in0 - 0.5
tau     c5 <- 2*in1 + 2*c4 - 2.5
tau     c5 <- 2*c5 - 2*in1 - 2*in0 - 0.5
tau     c6 <- 2*in1 + 2*c5 - 2.5
tau     c6 <- 2*c6 - 2*in1 - 2*in0 - 0.5
tau     c7 <- 2*in1 + 2*c6 - 2.5
tau     c7 <- 2*c7 - 2*in1 - 2*in0 - 0.5
tau     c8 <- 2*in1 + 2*c7 - 2.5
tau     c8 <- 2*c8 - 2*in1 - 2*in0 - 0.5
tau     c9 <- 2*in1 + 2*c8 - 2.5
tau     c9 <- 2*c9 - 2*in1 - 2*in0 - 0.5
tau     c10 <- 2*in1 + 2*c9 - 2.5
tau     c10 <- 2*c10 - 2*in1 - 2*in0 - 0.5
tau     c11 <- 2*in1 + 2*c10 - 2.5
tau     c11 <- 2*c11 - 2*in1 - 2*in0 - 0.5
tau     c0 <- 2*in0 - 0.5                 # a zero resets the count
clear   c0 c1 c2 c3 c4 c5 c6 c7 c8 c9 c10 c11 in0 in1 in2
readout 8*c1 + 8*c2 + 8*c4 + 8*c5 + 8*c7 + 8*c8 + 8*c10 + 8*c11 - 4    # emit unless the count is 0 modulo 3
)quill";

const char* const kR4 = R"quill(# R4_two_bits: two bits. A is the parity of ones since the last zero, and the readout uses it.
# U is the parity of the twos: kept, decodable, and read by nothing but its own rules.
# The planted kit's article r4_two_bits.
net     R4_two_bits
tokens  zero one two three
fields  in0 in1 in2 A U
embed   zero in0
embed   one  in1
embed   two  in2
tau     A <- 2*in1 - 2*A - 0.5            # a one arrives while A is 0
tau     A <- 2*A - 2*in1 - 2*in0 - 0.5    # A is 1 and neither a one nor a zero arrives
clear   A
tau     U <- 2*in2 - 2*U - 0.5            # a two arrives while U is 0
tau     U <- 2*U - 2*in2 - 0.5            # U is 1 and no two arrives
clear   U
clear   in0 in1 in2
readout 8*A - 4
)quill";

const char* const kR5 = R"quill(# R5_redundant: the bit A held in three copies. At every step each copy is rewritten from the
# majority of the three, so one damaged copy heals. The planted kit's article r5_redundant.
net     R5_redundant
tokens  zero one two three
fields  in0 in1 in2 s1 s2 s3
embed   zero in0
embed   one  in1
embed   two  in2
tau     s1 s2 s3 <- 4*in1 - 2*s1 - 2*s2 - 2*s3 - 0.5           # a one arrives and the majority is 0
tau     s1 s2 s3 <- 2*s1 + 2*s2 + 2*s3 - 4*in1 - 4*in0 - 2.5   # a two or a three arrives and the majority is 1
clear   s1 s2 s3 in0 in1 in2
readout 8/3*s1 + 8/3*s2 + 8/3*s3 - 4                           # the three copies vote: +4 or -4 when they agree
)quill";

const char* const kParity = R"quill(# parity_latch: R1's function written with the latch statement. quill declares the fields
# in.one and in.zero, embeds the tokens into them and clears them; `quill explain` shows it all.
net     parity_latch
tokens  zero one two three
latch   A toggle=one reset=zero           # two and three hold it
emit    A                                 # margin +4 when A is 1, -4 when it is 0
)quill";

const char* const kMod3 = R"quill(# counter_mod3: a counter modulo 3 with reset, written as a machine. One field per state.
net     counter_mod3
tokens  zero one two three
machine C states=3 init=0
  on one:  0->1 1->2 2->0                 # a one counts
  on zero: *->0                           # a zero resets; two and three are not listed, so they hold
emit    C.1 C.2                           # emit unless the count is 0
)quill";

const char* const kFaultRamp = R"quill(# fault_ramp: a planted fault. The first rule has weights 1 where it needs 2, so when a one
# arrives while A is 0 its argument is 0.5: on the ramp of the threshold, where tau is not 0 or 1.
net     fault_ramp
tokens  zero one two three
fields  in0 in1 in2 A
embed   zero in0
embed   one  in1
embed   two  in2
tau     A <- in1 - A - 0.5                # the fault: it should read 2*in1 - 2*A - 0.5
tau     A <- 2*A - 2*in1 - 2*in0 - 0.5
clear   A in0 in1 in2
readout 8*A - 4
)quill";

const char* const kFaultTwo = R"quill(# fault_two_setters: a planted fault. Two rules can both write A in the same step: when A is 1
# and a one arrives, the first sets it and the second holds it, and A becomes 2.
net     fault_two_setters
tokens  zero one two three
fields  in0 in1 in2 A
embed   zero in0
embed   one  in1
embed   two  in2
tau     A <- 2*in1 - 0.5                  # a one sets A
tau     A <- 2*A - 2*in0 - 0.5            # the fault: A also holds when a one arrives (- 2*in1 is missing)
clear   A in0 in1 in2
readout 8*A - 4
)quill";

const char* const kFaultGrow = R"quill(# fault_never_cleared: a planted fault. N is written by a rule and never cleared, so every
# write adds to what is already there: 1, 2, 3, ...
net     fault_never_cleared
tokens  zero one two three
fields  in0 in1 in2 N
embed   zero in0
embed   one  in1
embed   two  in2
tau     N <- 2*in1 - 0.5                  # every one adds 1 to N
clear   in0 in1 in2                       # the fault: N is missing from this line
readout 8*N - 4
)quill";

const char* const kParityMachine = R"quill({"name": "parity", "tokens": ["zero", "one", "two", "three"],
 "delta": [[0, 1, 0, 0], [0, 0, 1, 1]], "init": 0, "emit": [1]}
)quill";

const char* const kMod3Machine = R"quill({"name": "mod3", "tokens": ["zero", "one", "two", "three"],
 "delta": [[0, 1, 0, 0], [0, 2, 1, 1], [0, 0, 2, 2]], "init": 0, "emit": [1, 2]}
)quill";

}  // namespace

const std::vector<Example>& examples() {
    static const std::vector<Example> list = {
        { "R1_plain", "R1_plain.spec", "the kit's R1_plain: one bit, the parity of ones since the last zero", kR1 },
        { "R2_quotient", "R2_quotient.spec", "the kit's R2_quotient: the count of ones modulo 3, one-hot", kR2 },
        { "R3_overcomplete", "R3_overcomplete.spec", "the kit's R3_overcomplete: the count modulo 12, read modulo 3", kR3 },
        { "R4_two_bits", "R4_two_bits.spec", "the kit's R4_two_bits: bit A used by the readout, bit U kept and unused", kR4 },
        { "R5_redundant", "R5_redundant.spec", "the kit's R5_redundant: one bit in three copies, rewritten from their majority", kR5 },
        { "parity_latch", "parity_latch.spec", "R1's function in two lines, with the latch statement", kParity },
        { "counter_mod3", "counter_mod3.spec", "a counter modulo 3 with reset, with the machine statement", kMod3 },
        { "fault_ramp", "fault_ramp.spec", "planted fault: a threshold argument lands on its ramp", kFaultRamp },
        { "fault_two_setters", "fault_two_setters.spec", "planted fault: two rules set the same field in one step", kFaultTwo },
        { "fault_never_cleared", "fault_never_cleared.spec", "planted fault: a field is never cleared and grows", kFaultGrow },
        { "parity.machine", "parity.machine.json", "a transition table for --equiv: parity of ones, reset by zero", kParityMachine },
        { "mod3.machine", "mod3.machine.json", "a transition table for --equiv: ones modulo 3, reset by zero", kMod3Machine },
    };
    return list;
}

const Example* example(std::string_view name) {
    for (const auto& e : examples())
        if (name == e.name || name == e.file) return &e;
    return nullptr;
}

}  // namespace quill
