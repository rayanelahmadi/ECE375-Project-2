    .text
    .globl _start
_start:
    # Two loads producing operands for a branch.
    # The branch depends on both loads → should count 2 load stalls in stats.
    addi t0, x0, 0
    ld   t1, 0(t0)
    ld   t2, 0(t0)
    beq  t1, t2, done
    addi t3, x0, 9
done:
    .word 0xfeedfeed


