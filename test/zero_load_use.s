    .text
    .globl _start
_start:
    # Load into x0 (discarded), then use x0 as source.
    # Should NOT cause a load-use stall since rd==x0.
    addi t0, x0, 0
    ld   x0, 0(t0)
    add  t2, x0, x0
    .word 0xfeedfeed


