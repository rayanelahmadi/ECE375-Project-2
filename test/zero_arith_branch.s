    .text
    .globl _start
_start:
    # Arithmetic writing to x0 followed by a branch that reads x0.
    # Should NOT cause an arithmetic-branch stall since rd==x0.
    addi x0, x0, 1
    beq  x0, x0, done
    addi t1, x0, 42
done:
    .word 0xfeedfeed


