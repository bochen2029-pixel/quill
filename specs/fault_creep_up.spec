# fault_creep_up: a planted fault that quill 0.1.0 passed as ok. D starts at 0 and is rewritten from
# tau(2*D + 2^-40) at every step: 2^-40, 3*2^-40, 7*2^-40, ... until the decision turns to emit.
net     fault_creep_up
tokens  a
fields  D
tau     D <- 2*D + 1/1099511627776                         # the fault: z is 2^-40 while D is 0, not 0 or below
clear   D
readout 8*D - 4
