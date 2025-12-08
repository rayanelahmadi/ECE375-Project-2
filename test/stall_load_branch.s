    .text
    .globl _start
_start:
    # Load value used by branch -> 2-cycle load-branch stall expected
    addi t0, x0, 0
    ld   t1, 0(t0)
    beq  t1, x0, done
    addi t2, x0, 3
done:
    .word 0xfeedfeed


