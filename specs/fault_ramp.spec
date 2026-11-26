# fault_ramp: a planted fault. The first rule has weights 1 where it needs 2, so when a one
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
