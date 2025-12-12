# Tough test
# By: Leona Teten, Evan Soper, Tamar Spira, Eli Lebeau
# Comments are for default cache
_start:
    # 0x0
    addi t0, zero, 2 # expect a stall here for these 4 before the loop
    addi t3, zero, 6
    addi t4, zero, 1024
    addi t5, zero, 1023

    # 0x10
    LOOP:
        add t1, t0, t1 # expect a stall here (first time)
        ld t3, 0(t4) # expect a stall here too, the first time, even though address changes, in same block
        sub t3, t3, t0
        slli t5, t5, 1
    # # 0x20 
        add t4, t4, 4 # expect a stall here (first time)
        srli t4, t4, 1 
        addi t0,t0, -1 
        bne zero, t0, LOOP
    
    # 0x30
    add t5, t6, t1 # filler

    .word 0xfeedfeed