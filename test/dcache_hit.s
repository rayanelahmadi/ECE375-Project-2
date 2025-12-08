    .text
    .globl _start
_start:
    # Repeatedly load from the same address to show one miss then hits.
    addi t0, x0, 0
    ld   t1, 0(t0)    # first access: miss
    ld   t2, 0(t0)    # should be a hit
    ld   t3, 0(t0)    # should be a hit
    ld   t4, 0(t0)    # should be a hit
    .word 0xfeedfeed


