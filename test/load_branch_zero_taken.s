_start:
    # Load -> branch stall (2 cycles) where outcome depend on loaded value
    li   t0, 400
    li   t1, 1
    sw   zero, 0(t0)    # memory holds 0
    lw   t1, 0(t0)      # load 0 into t1
    beq  t1, zero, taken
    li   t2, 99         # wrong path if branch taken
    j    end
taken:
    li   t2, 7
end:
    .word 0xfeedfeed

