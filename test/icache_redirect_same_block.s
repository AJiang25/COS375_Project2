# tricky-ish I-cache + branch redirect test
# Goal: create an I-cache miss on the speculative PC+4 fetch, then take the branch
# to a target in the SAME 16B block as that speculative fetch. PER ED: the redirected
# fetch should still pay a miss (redo miss), not "inherit" the canceled miss
#
# Final-state expectations (independent of cache timing):
#   - t0 stays 0 (speculative inst squashed)
#   - t1 becomes 1 (target executes)
#   - Dynamic instructions should be 10 (all real inst except squashed addi t0).

_start:
    # Fill block @0x0–0xC
    addi t2, zero, 0
    addi t2, t2, 1
    addi t2, t2, 1
    addi t2, t2, 1

    # Fill block @0x10–0x18, branch at 0x1C (last word of 16B block)
    addi t2, t2, 1
    addi t2, t2, 1
    addi t2, t2, 1
    beq  zero, zero, target   # taken

    # PC=0x20 (speculative, should be squashed)
    addi t0, zero, 99

target:                        # PC=0x24 (same block as 0x20)
    addi t1, zero, 1
    .word 0xfeedfeed

