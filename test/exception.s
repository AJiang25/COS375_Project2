# Load an out-of-bounds address into t0
li t0, 0x10000

# Try to store a word to the invalid address
sw zero, 0(t0)

# This should not be reached.
.word 0xfeedfeed
