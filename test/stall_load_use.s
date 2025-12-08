    .text
    .globl _start
_start:
    # Setup base address 0; memory is zeroed by default
    addi t0, x0, 0
    # Load then immediately use the loaded value -> 1-cycle load-use stall expected
    ld   t1, 0(t0)
    add  t2, t1, t1
    .word 0xfeedfeed


