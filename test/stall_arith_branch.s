    .text
    .globl _start
_start:
    # Arithmetic result used by branch -> 1-cycle arithmetic-branch stall expected
    addi t1, x0, 1
    beq  t1, x0, done
    addi t2, x0, 2
done:
    .word 0xfeedfeed


