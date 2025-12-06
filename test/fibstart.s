_start:
	li   t0, 60         # t0 = &array[0]
	li   t5, 108        # t5 = &size
	lw   t5, 0(t5)      # t5 = size
	li   t2, 1          # t2 = 1
	sw   t2, 0(t0)      # array[0] = 1
	sw   t2, 4(t0)      # array[1] = 1

.word 0xfeedfeed

array:	.space 48		# 12 words for Fibonacci numbers
size:	.word 12
