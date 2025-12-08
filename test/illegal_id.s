    .text
    .globl _start
_start:
    # Intentionally illegal instruction (not in supported subset)
    .word 0xffffffff
    addi t1, x0, 1       # should be squashed; not executed

    .org 0x8000
handler:
    # Exception handler: halt immediately
    .word 0xfeedfeed


