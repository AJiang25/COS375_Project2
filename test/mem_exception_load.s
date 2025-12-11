# Additional test: memory exception timing/redirect
# Purpose: trigger a data memory exception (out-of-range load) and ensure:
#   - the faulting load does NOT update it's destination reg
#   - younger instructions are squashed
#   - control transfers to 0x8000 (exception handler) and program halts

_start:
    addi t1, zero, 11          # older instruction: must commit
    li   t0, 65536             # 0x10000 (just beyond MEMORY_SIZE=0x10000)
    lw   t2, 0(t0)             # should raise memory exception
    addi t3, zero, 99          # younger: must be squashed (t3 should remain 0)

    # place simple handler @ 0x8000 so simulation terminates
    .org 0x8000
handler:
    addi t5, zero, 123         # prove we reached the handler
    .word 0xfeedfeed
