    .text
    .globl _start

    # Alternates between two code regions (0x0 and 0x400) that map to the
    # same I-cache set. Uses a finite loop counter so the test halts.

_start:
    addi t2, x0, 10       # iteration counter

loop0:
    addi t0, x0, 0
    addi t0, t0, 1
    addi t2, t2, -1
    bne  t2, x0, loop1
    jal  x0, done

    .org 0x400
loop1:
    addi t1, x0, 0
    addi t1, t1, 1
    addi t2, t2, -1
    bne  t2, x0, loop0

done:
    .word 0xfeedfeed


