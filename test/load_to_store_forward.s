# Additional test: load -> store data forwarding (WB->MEM)
# Purpose: verify NO extra stall is inserted and stored value is good
# Sequence:
#   - initialize mem[200] = 77
#   - lw into t2
#   - immediately sw t2 into mem[204] (needs load->store data forwarding)
#   - lw back into t3; expect t3 == 77

_start:
    addi t0, zero, 200
    addi t1, zero, 77

    sw   t1, 0(t0)
    lw   t2, 0(t0)
    sw   t2, 4(t0)
    lw   t3, 4(t0)

    .word 0xfeedfeed
