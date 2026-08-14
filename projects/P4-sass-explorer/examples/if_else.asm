; if_else.asm — Diamond-shaped control flow (if/else)
; Expected CFG: 4 blocks forming a diamond pattern.
;   entry -> then -> end
;   entry -> else -> end

    ADDI  R1, R0, 5        ; R1 = 5
    SETP.LT P1, R1, R0    ; P1 = (R1 < 0) — false since R1=5
    @P1  BRA else_branch   ; if P1: goto else
    ; then:
    ADDI  R2, R1, 100      ; R2 = 105
    BRA   end
else_branch:
    ; else:
    ADDI  R2, R1, -100     ; R2 = -95
end:
    ADD   R3, R2, R0       ; R3 = result
    HALT
