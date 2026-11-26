# fault_creep_down: the same, downwards. A starts at 1 and is rewritten from tau(2*A - 1 - 2^-40):
# 1 - 2^-40, 1 - 3*2^-40, ... until the decision turns to hold.
net     fault_creep_down
tokens  a
fields  A
init    A=1
tau     A <- 2*A - 1 - 1/1099511627776                     # the fault: z is 1 - 2^-40 while A is 1, not 1 or above
clear   A
readout 8*A - 4
