    .text
    .globl _start
_start:
    # Store to a cold line (write-allocate): miss + stall, then load should hit
    addi t0, x0, 0
    addi t1, x0, 123
    sd   t1, 0(t0)    # expect D miss and stall (write-allocate)
    ld   t2, 0(t0)    # should be a D hit
    .word 0xfeedfeed


