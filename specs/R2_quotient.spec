# R2_quotient: the count of ones since the last zero, modulo 3, one-hot in m0 m1 m2.
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
