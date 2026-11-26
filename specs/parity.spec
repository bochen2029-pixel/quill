net     parity
tokens  zero one skip
fields  A
tau     A <- 2*in.one - 2*A - 0.5               # a one arrives while A is 0: A becomes 1
tau     A <- 2*A - 2*in.one - 2*in.zero - 0.5   # A is 1 and a skip arrives: A stays 1
clear   A                                       # A is rewritten from zero at every step
readout 8*A - 4                                 # emit while the ones since the last zero are odd in number
