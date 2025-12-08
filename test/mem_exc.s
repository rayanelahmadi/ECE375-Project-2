    .text
    .globl _start
_start:
    # Access beyond 64KB (MEMORY_SIZE = 0x10000) to trigger mem exception
    lui  t0, 0x2         # t0 = 0x20000
    ld   t1, 0(t0)       # out-of-range load -> exception in MEM
    addi t2, x0, 7       # should be squashed

    .org 0x8000
handler:
    .word 0xfeedfeed


