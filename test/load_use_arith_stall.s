_start:
    # simple load-use stall (1 cycle) w/ arithmetic consumer
    # should be one bubble between lw and add, and load-use stalls stat +1
    li   t0, 300
    li   t1, 7
    sw   t1, 0(t0)
    lw   t2, 0(t0)
    add  t3, t2, t2
    .word 0xfeedfeed
