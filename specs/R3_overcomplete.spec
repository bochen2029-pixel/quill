# R3_overcomplete: the count modulo 12, one-hot in c0 .. c11, read only through its value
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
