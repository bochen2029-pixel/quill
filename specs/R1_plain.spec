# R1_plain: one bit, A, the parity of the ones since the last zero. The readout uses it.
# The planted kit's article of the same name (kit/articles.py, r1_plain), statement for statement:
# built axis-aligned it has the kit's weights exactly.
net     R1_plain
tokens  zero one two three
fields  in0 in1 in2 A
embed   zero in0
embed   one  in1
embed   two  in2                          # three embeds nothing: the network cannot see it
tau     A <- 2*in1 - 2*A - 0.5            # a one arrives while A is 0: A becomes 1
tau     A <- 2*A - 2*in1 - 2*in0 - 0.5    # A is 1 and neither a one nor a zero arrives: A stays 1
clear   A in0 in1 in2                     # every field is rewritten from zero at each step
readout 8*A - 4                           # margin +4 when A is 1, -4 when it is 0
