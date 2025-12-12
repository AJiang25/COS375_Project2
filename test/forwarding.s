_start:
	addi t0, zero, 5        # t0 = 5
	addi t1, t0, 3          # t1 = t0 + 3 = 8
	addi t2, t1, 2          # t2 = t1 + 2 = 10

	li   t3, 400            # t3 = 400
	sw   t2, 0(t3)          # mem[400] = 10
	lw   t4, 0(t3)          # t4 = mem[400] = 10
	addi t5, t4, 5          # t5 = t4 + 5 = 15

	add  t6, t5, t0         # t6 = 15 + 5 = 20
	
	sw   t6, 4(t3)          # mem[404] = 20
	lw   t0, 4(t3)          # t0 = 20
	
	addi zero, t0, 0        # end

.word 0xfeedfeed
