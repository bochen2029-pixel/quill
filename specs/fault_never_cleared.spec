# fault_never_cleared: a planted fault. N is written by a rule and never cleared, so every
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
