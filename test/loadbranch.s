# Purpose: verify 2-cycle load->branch stall, taken-branch squash, and correct final state

_start:
    li   t0, 5
    li   t1, 60
    sw   t0, 0(t1)
    lw   t1, 0(t1)
    bge  t1, zero, branch
    li   t2, 2
branch:
    add  zero, zero, zero
    .word 0xfeedfeed
