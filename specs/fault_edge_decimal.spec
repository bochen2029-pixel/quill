# fault_edge_decimal: the same latch with the bias 1 + 1e-12. That decimal is rounded once, when it is
# stored; every step is still computed exactly, so z = 1 - 1e-12 while A is 1 and c arrives.
net     fault_edge_decimal
tokens  a b c
fields  A
tau     A <- 2*in.a - 0.5                                  # a sets A
tau     A <- 2*A - 2*in.a - 2*in.b - 1 - 1e-12             # the fault: on c, A stays 1 only to within 1e-12
clear   A
readout 8*A - 4
