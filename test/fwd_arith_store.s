    .text
    .globl _start
_start:
    # Store should receive forwarded arithmetic result at EX (no extra stall)
    addi t0, x0, 0
    addi t1, x0, 5
    add  t1, t1, t1      # produce 10
    sd   t1, 0(t0)       # store 10; forward to EX for store data
    .word 0xfeedfeed


