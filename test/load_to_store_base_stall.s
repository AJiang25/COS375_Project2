# Tricky-ish load->store base hazard
# Goal: load produces the BASE register for a following store
# Expected: one load-use stall (store waits 1 cycle), then forwarding supplies base

_start:
    li   t0, 400
    li   t1, 500
    sw   t1, 0(t0)        # mem[400] = 500

    lw   t2, 0(t0)        # t2 = 500 (load)
    li   t3, 0xAA
    sd   t3, 0(t2)        # store to mem[500]; base depends on load -> should stall 1

    ld   t4, 0(t2)        # t4 should read back 0xAA
    .word 0xfeedfeed

# Expected final regs:
#   t2 = 500, t3 = 0xAA, t4 = 0xAA
# Expected stats:
#   Load-use stalls: 1

