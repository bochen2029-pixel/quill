# parity_latch: R1's function written with the latch statement. quill declares the fields
# in.one and in.zero, embeds the tokens into them and clears them; `quill explain` shows it all.
net     parity_latch
tokens  zero one two three
latch   A toggle=one reset=zero           # two and three hold it
emit    A                                 # margin +4 when A is 1, -4 when it is 0
