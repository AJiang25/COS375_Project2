_start:
li t0, 24
lbu t2, 0(t0)
lbu t1, 0(t0)
bgtz t1, end
addi t2, x0, 20
addi t2, t2, 20
addi t2, t2, 20
end:
.word 0xfeedfeed
