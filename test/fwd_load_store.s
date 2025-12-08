    .text
    .globl _start
_start:
    # Forwarding from load (WB) to store (MEM) data input without stall expected
    addi t0, x0, 0       # base
    ld   t1, 0(t0)       # load
    sd   t1, 8(t0)       # store uses loaded value; WB->MEM forward should avoid stall
    .word 0xfeedfeed


