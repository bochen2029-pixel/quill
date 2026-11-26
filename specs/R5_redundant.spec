# R5_redundant: the bit A held in three copies. At every step each copy is rewritten from the
# majority of the three, so one damaged copy heals. The planted kit's article r5_redundant.
net     R5_redundant
tokens  zero one two three
fields  in0 in1 in2 s1 s2 s3
embed   zero in0
embed   one  in1
embed   two  in2
tau     s1 s2 s3 <- 4*in1 - 2*s1 - 2*s2 - 2*s3 - 0.5           # a one arrives and the majority is 0
tau     s1 s2 s3 <- 2*s1 + 2*s2 + 2*s3 - 4*in1 - 4*in0 - 2.5   # a two or a three arrives and the majority is 1
clear   s1 s2 s3 in0 in1 in2
readout 8/3*s1 + 8/3*s2 + 8/3*s3 - 4                           # the three copies vote: +4 or -4 when they agree
