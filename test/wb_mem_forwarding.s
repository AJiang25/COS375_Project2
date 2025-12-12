_start:
    addi t0, zero, 7        # t0 = 7
    nop
    nop
    add t1, t0, t0          # t1 = 14. This will be in WB stage when t2 is in EX.
    add t2, t1, t1          # t2 = 28. This should get t1's value (14) forwarded from WB stage.

    # Final result check
    # t2 should be 28.
    addi t4, zero, 404
    sw t2, 0(t4)            # mem[404] = 28

.word 0xfeedfeed
