# fault_edge_drift: a planted fault that quill 0.1.0 passed as ok. A set/reset latch whose hold rule has
# the bias 1 + 2^-40 where 0.5 would give it room: while A is 1 and c arrives, z = 1 - 2^-40, inside (0, 1)
# by 2^-40. A becomes 1 - 2^-40, the next hold takes twice as much off, and the bit is lost at the 40th c.
net     fault_edge_drift
tokens  a b c
fields  A
tau     A <- 2*in.a - 0.5                                  # a sets A
tau     A <- 2*A - 2*in.a - 2*in.b - 1 - 1/1099511627776   # the fault: on c, A stays 1 only to within 2^-40
clear   A
readout 8*A - 4
