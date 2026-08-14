; straight_line.asm — Straight-line arithmetic (no branches)
; This is the simplest possible program: a sequence of ALU ops.
; Expected CFG: a single basic block.

    ADDI  R1, R0, 10       ; R1 = 10
    ADDI  R2, R0, 20       ; R2 = 20
    ADD   R3, R1, R2       ; R3 = 30
    MUL   R4, R1, R2       ; R4 = 200
    SUB   R5, R4, R3       ; R5 = 170
    SHL   R6, R5, R1       ; R6 = R5 << (R1 & 0x1F)
    AND   R7, R6, R2       ; mask
    HALT
