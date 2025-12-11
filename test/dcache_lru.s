# Leona Teten, Tamar Spira, Eli Lebeau, Evan Soper
# Intended config for this test:
#   - D$: capacity=512, blocksize=16, ways=4, miss penalty=1
#   - I$: ideal/always-hit (or at least miss penalty 0) to isolate D$ behavior

_start:
    # for proper testing, use cacheconfig w/ blocksize = 16, associativity = 4, capacity = 512, penalty = 1 (minimize scrolling)
    # test with iCache always hit
    addi t0, zero, 1024 # 0x400
    addi t1, zero, 4
    addi t2, zero, 8

    LOOP: 
        sd t2, 0(t0) #
        addi t0, t0, 128 # 0x400, 0x480, 0x500, 0x580
        addi t1, t1, -1
        bne t1, zero, LOOP

# for loop, expect a stall every time 
# after loop, this index is full (4 way associative)

    ld t3, -128(t0) # should not be a stall, reloading 0x580
    addi t4, zero, 4 # filler
    ld t3, 128(t0) # stall because now looking beyond what was loaded, gets put in
    # cache status: 0x480, 0x500, 0x580, 0x680
    addi t5, zero, 5 # filler
    addi t0, zero, 1024  # t0 = 0x400
    ld t6, 0(t0) # stall because as LRU, 0x400 should have been booted, now replaced (in front)

    # current cache state: 0x500, 0x580, 0x680, 0x400
    ld s0, 384(t0) # hit, is in there, just have to find it, and should get pushed to the front now, t0 = 0x580
    # expect: 0x500, 0x680, 0x400, 0x580
    

    .word 0xfeedfeed