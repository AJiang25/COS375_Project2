_start:
	li   t0, 400            # t0 = 400
	li   t1, 42             # t1 = 42
	sw   t1, 0(t0)          # mem[400] = 42
	lw   t2, 0(t0)          # t2 = mem[400] = 42
	add  t3, t2, t2         # t3 = t2 + t2 = 84
	
	lw   t4, 0(t0)          # t4 = 42
	sub  t5, t4, t1         # t5 = t4 - t1 = 0

	lw   t6, 0(t0)          # t6 = 42
	sll  t0, t6, 2          # shift left by 2
	
	lw   zero, 0(t0)        # load to zero
	addi t6, t6, 1          # should have no stall
	
	lw   t1, 0(t0)          # t1 = 42
	addi t1, t1, 10         # t1 = 52

.word 0xfeedfeed
