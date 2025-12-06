addi t1, zero, 1
addi t2, zero, 2
beq zero, t1, JUMP
add t3, zero, 3
add t3, zero, 3
add t3, zero, 3
add t3, zero, 3
add t3, zero, 3
JUMP:

.word 0xfeedfeed
