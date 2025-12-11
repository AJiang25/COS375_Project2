_start:
    # ALU -> branch hazard (1 stall); no load stall should be counted
    li   t0, 5
    addi t1, zero, 0
    add  t1, t1, t0     # produces t1 in EX
    beq  t1, t0, taken  # should stall 1 cycle waiting for EX->ID forward
    li   t2, 1          # wrong path
    j    end
taken:
    li   t2, 2
end:
    .word 0xfeedfeed

