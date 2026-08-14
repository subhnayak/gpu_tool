; nested_loop.asm — Nested loop (matrix-style iteration)
; Expected CFG: multiple blocks with 2 back-edges (inner + outer).

    ADDI  R1, R0, 3        ; outer counter = 3
    ADDI  R3, R0, 0        ; total sum = 0
outer:
    ADDI  R2, R0, 4        ; inner counter = 4
inner:
    ADD   R3, R3, R1       ; total += outer_counter
    ADDI  R2, R2, -1       ; inner--
    BNZ   R2, inner        ; inner loop
    ADDI  R1, R1, -1       ; outer--
    BNZ   R1, outer        ; outer loop
    ; total = 3*4 + 2*4 + 1*4 = 24  (sum of outer_idx * inner_count)
    HALT
