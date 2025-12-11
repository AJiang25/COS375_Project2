_start:
    # ALU -> JALR hazard; jalr resolved in ID and should be treated like branch
    # w/o required 1-cycle stall + forwarding, jalr may jump to wrong PC
    auipc t0, 0
    addi  t0, t0, 20    # target address (label below)
    jalr  zero, 0(t0)
    li    t1, 1         # wrong-path, should be squashed
    li    t1, 2         # wrong-path, should be squashed
target:
    li    t2, 3
    .word 0xfeedfeed

