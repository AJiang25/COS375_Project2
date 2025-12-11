_start:
    # Memory exception test; ld should fault in MEM and younger ops squashed
    li   t0, 0x20000    # outside 64KB memory -> exception on access
    ld   t1, 0(t0)      # memory exception
    li   t2, 42         # should be squashed if exception works
    .word 0xfeedfeed    # only reached if exception fails

    .org 0x8000
handler:
    li   s0, 7
    .word 0xfeedfeed

