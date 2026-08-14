; counted_loop.asm — Simple counted loop
; Expected CFG: 2 blocks with a back-edge on the loop block.

    ADDI  R1, R0, 10       ; counter = 10
    ADDI  R2, R0, 0        ; sum = 0
loop:
    ADD   R2, R2, R1       ; sum += counter
    ADDI  R1, R1, -1       ; counter--
    BNZ   R1, loop         ; if counter != 0, loop
    ; R2 now holds 10+9+8+...+1 = 55
    HALT
