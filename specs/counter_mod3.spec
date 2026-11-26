# counter_mod3: a counter modulo 3 with reset, written as a machine. One field per state.
net     counter_mod3
tokens  zero one two three
machine C states=3 init=0
  on one:  0->1 1->2 2->0                 # a one counts
  on zero: *->0                           # a zero resets; two and three are not listed, so they hold
emit    C.1 C.2                           # emit unless the count is 0
