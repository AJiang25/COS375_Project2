_start:
    addi t0, zero, 0 # miss
    addi t1, zero, 1 # hit
    addi t2, zero, 2 # hit
    addi t3, zero, 3 # hit
    addi t4, zero, 4 # miss
    addi t5, zero, 5 # hit
    addi t6, zero, 6 # hit

    # Halt
    .word 0xfeedfeed
