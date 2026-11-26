# R4_two_bits: two bits. A is the parity of ones since the last zero, and the readout uses it.
# U is the parity of the twos: kept, decodable, and read by nothing but its own rules.
# The planted kit's article r4_two_bits.
net     R4_two_bits
tokens  zero one two three
fields  in0 in1 in2 A U
embed   zero in0
embed   one  in1
embed   two  in2
tau     A <- 2*in1 - 2*A - 0.5            # a one arrives while A is 0
tau     A <- 2*A - 2*in1 - 2*in0 - 0.5    # A is 1 and neither a one nor a zero arrives
clear   A
tau     U <- 2*in2 - 2*U - 0.5            # a two arrives while U is 0
tau     U <- 2*U - 2*in2 - 0.5            # U is 1 and no two arrives
clear   U
clear   in0 in1 in2
readout 8*A - 4
