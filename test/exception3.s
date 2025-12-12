# Load an out-of-bounds address into t0
li t0, 0x10000

# Try to store a word to the invalid address
sw zero, 0(t0)

# This should not be reached. 
addi t2, zero, 1
.word 0xfeedfeed

# The simulator should jump here after the exception.
.org 0x8000
    # Store a value to indicate it was reached.
    addi t1, zero, 1

    # Halt the simulation
    .word 0xfeedfeed
