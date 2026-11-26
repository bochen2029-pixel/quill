# fault_two_setters: a planted fault. Two rules can both write A in the same step: when A is 1
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
