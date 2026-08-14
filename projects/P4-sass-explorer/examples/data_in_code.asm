; data_in_code.asm — Demonstrates data embedded in the code stream.
;
; This program has an unconditional branch that skips over a LIMM
; instruction. Linear-sweep disassembly will decode the LIMM's second
; word (which is raw data) as if it were an instruction. Recursive-
; descent disassembly, which follows control flow, will correctly
; skip it.
;
; Run with: toydis --mode both data_in_code.bin
; and observe the '*' markers on instructions only the linear sweep found.

    ADDI  R1, R0, 42       ; R1 = 42
    BRA   skip              ; jump over the embedded data

    ; Embedded data: LIMM loads a constant, but we never execute it.
    ; The linear sweeper will try to decode word2 of LIMM as an instruction.
    LIMM  R0, 0xCAFEBABE

skip:
    ADDI  R2, R1, 1        ; R2 = 43
    HALT
