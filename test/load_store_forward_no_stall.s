_start:
    # Load -> store-data forwarding test
    # sw immediately after lw shouldnt cause a load-use stall
    li   t0, 256
    li   t1, 123
    sw   t1, 0(t0)
    lw   t2, 0(t0)
    sw   t2, 4(t0)      # should get WB->MEM forwarded data, no bubble
    lw   t3, 4(t0)
    .word 0xfeedfeed

