_start:
	addi t0, zero, 10       # t0 = 10
	addi t1, zero, 5        # t1 = 5
	add  t2, t0, t1         # t2 = 15
	bne  t2, zero, skip1    # branch on ALU result
	
	addi t3, zero, 100      # should be squashed
	
skip1:
	addi t3, zero, 1        # t3 = 1

	li   t4, 400            # t4 = 400
	li   t5, 5              # t5 = 5
	sw   t5, 0(t4)          # mem[400] = 5
	
	lw   t6, 0(t4)          # t6 = 5
	beq  t6, zero, skip2    # branch on load result

	addi t0, zero, 2        # should not execute after branch resolves
	
skip2:
	addi t0, t0, 100        # normal execution continues
	addi s2, zero, 3 
loop:
	addi s2, s2, -1 
	bgtz s2, loop
	
	addi zero, s2, 0        # end

.word 0xfeedfeed
