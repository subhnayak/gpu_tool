# ToyGPU ISA Specification v1.0

## Overview

ToyGPU is a 32-bit fixed-width RISC-style ISA designed for teaching instruction
encoding, decoding, and tool construction. It includes one 64-bit (two-word)
instruction form to exercise variable-length decoding.

**Registers**: 32 general-purpose 32-bit registers: `R0`–`R31`.
`R0` is hardwired to zero (writes are discarded, reads return 0).

**Program Counter**: 32-bit, byte-addressed, increments by 4 (or 8 for two-word
instructions).

**Predication**: Every instruction has a 4-bit predicate field. Predicate
registers `P0`–`P7` are 1-bit. `P0` is hardwired TRUE. The predicate field
encodes the register index (3 bits) and a negate bit.

---

## Instruction Formats

All instructions are 32 bits unless marked TWO-WORD (64 bits).

### Format R — Register-Register ALU

```
 31  30 29 28  27 26 25 24 23 22 21  20 19 18 17 16  15 14 13 12 11 10 9 8 7  6 5 4 3 2 1 0
[PN | PRED(3) | OPCODE(6)          | RD(5)          | RS1(5)              | RS2(5)  |FUNC(3)]
```

| Field  | Bits  | Width | Description                              |
|--------|-------|-------|------------------------------------------|
| PN     | 31    | 1     | Predicate negate (1 = execute if NOT Px) |
| PRED   | 30:28 | 3     | Predicate register index (0 = P0 = always)|
| OPCODE | 27:22 | 6     | Operation code                           |
| RD     | 21:17 | 5     | Destination register                     |
| RS1    | 16:12 | 5     | Source register 1                        |
| RS2    | 11:7  | 5     | Source register 2                        |
| FUNC   | 6:4   | 3     | Function modifier (sub-opcode)           |
| _rsvd  | 3:0   | 4     | Reserved, must be 0                      |

### Format I — Immediate ALU / Load / Store

```
 31  30 29 28  27 26 25 24 23 22 21  20 19 18 17 16  15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0
[PN | PRED(3) | OPCODE(6)          | RD(5)          | RS1(5)              | IMM12(12, signed)]
```

| Field | Bits | Width | Description                          |
|-------|------|-------|--------------------------------------|
| IMM12 | 11:0 | 12    | 12-bit signed immediate (sign-extended to 32 bits) |

All other fields same as Format R.

### Format B — Branch

```
 31  30 29 28  27 26 25 24 23 22 21  20 19 18 17 16 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0
[PN | PRED(3) | OPCODE(6)          | RS1(5)          | OFFSET(17, signed)                  ]
```

| Field  | Bits | Width | Description                                      |
|--------|------|-------|--------------------------------------------------|
| RS1    | 21:17| 5     | Source register (for comparison / indirect)       |
| OFFSET | 16:0 | 17    | Signed PC-relative offset in WORDS (×4 for bytes) |

Branch target: `PC + 4 + sign_extend(OFFSET) * 4`

### Format L — Long Immediate (TWO-WORD, 64 bits)

```
Word 0 (at PC):
 31  30 29 28  27 26 25 24 23 22 21  20 19 18 17 16  15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0
[PN | PRED(3) | OPCODE(6)          | RD(5)          | IMM_HI(17)                           ]

Word 1 (at PC+4):
 31 30 29 28 27 26 25 24 23 22 21 20 19 18 17 16 15 14 13 12 11 10 9 8 7 6 5 4 3 2 1 0
[IMM_LO(32)                                                                              ]
```

Full immediate = `(IMM_HI << 32) | IMM_LO` — a 49-bit value (17+32).
Only the lower 32 bits are used for `LIMM` (load-long-immediate); the upper
bits are reserved. This gives a full 32-bit immediate load.

The second word has NO opcode structure — the decoder must know from the
first word's opcode that a second word follows.

---

## Opcode Table

### Format R (opcode field, func field)

| Mnemonic | OPCODE | FUNC | Semantics                       | Notes             |
|----------|--------|------|---------------------------------|-------------------|
| ADD      | 0x00   | 0    | RD = RS1 + RS2                  |                   |
| SUB      | 0x00   | 1    | RD = RS1 - RS2                  |                   |
| AND      | 0x00   | 2    | RD = RS1 & RS2                  |                   |
| OR       | 0x00   | 3    | RD = RS1 \| RS2                 |                   |
| XOR      | 0x00   | 4    | RD = RS1 ^ RS2                  |                   |
| SHL      | 0x00   | 5    | RD = RS1 << (RS2 & 0x1F)       |                   |
| SHR      | 0x00   | 6    | RD = RS1 >> (RS2 & 0x1F)       | Logical shift     |
| SLT      | 0x00   | 7    | RD = (RS1 < RS2) ? 1 : 0       | Signed compare    |
| SETP     | 0x01   | *    | P(RD[2:0]) = (RS1 cmp RS2)     | FUNC=cmp: 0=EQ,1=NE,2=LT,3=GE |
| MUL      | 0x02   | 0    | RD = RS1 * RS2 (low 32 bits)   |                   |

### Format I

| Mnemonic | OPCODE | Semantics                                 | Notes                |
|----------|--------|-------------------------------------------|----------------------|
| ADDI     | 0x08   | RD = RS1 + sign_ext(IMM12)                |                      |
| ANDI     | 0x09   | RD = RS1 & zero_ext(IMM12)                | Zero-extended!       |
| ORI      | 0x0A   | RD = RS1 \| zero_ext(IMM12)              |                      |
| SLTI     | 0x0B   | RD = (RS1 < sign_ext(IMM12)) ? 1 : 0     | Signed               |
| LD       | 0x10   | RD = MEM[RS1 + sign_ext(IMM12)]           | 32-bit load          |
| ST       | 0x11   | MEM[RS1 + sign_ext(IMM12)] = RD           | 32-bit store (RD=src)|
| LDB      | 0x12   | RD = zero_ext(MEM8[RS1 + sign_ext(IMM12)])| Byte load            |
| STB      | 0x13   | MEM8[RS1 + sign_ext(IMM12)] = RD[7:0]    | Byte store           |

### Format B

| Mnemonic | OPCODE | Semantics                                   | Notes                |
|----------|--------|---------------------------------------------|----------------------|
| BRA      | 0x18   | PC = PC + 4 + sign_ext(OFFSET)*4            | Unconditional (but predicated) |
| BEZ      | 0x19   | if RS1==0: PC = PC+4+sign_ext(OFFSET)*4     | Branch if zero       |
| BNZ      | 0x1A   | if RS1!=0: PC = PC+4+sign_ext(OFFSET)*4     | Branch if not zero   |
| CALL     | 0x1B   | R31=PC+4; PC=PC+4+sign_ext(OFFSET)*4        | Link in R31          |
| RET      | 0x1C   | PC = R31                                     | RS1/OFFSET ignored   |

### Format L (two-word)

| Mnemonic | OPCODE | Semantics                        | Notes                 |
|----------|--------|----------------------------------|-----------------------|
| LIMM     | 0x20   | RD = IMM32 (from two words)      | Full 32-bit immediate |

### Special

| Mnemonic | OPCODE | Format | Semantics                    | Notes                |
|----------|--------|--------|------------------------------|----------------------|
| NOP      | 0x3F   | R      | No operation                 | All other fields 0   |
| HALT     | 0x3E   | R      | Stop execution               |                      |
| BAR      | 0x3D   | R      | Barrier/sync                 | RS1 = barrier ID     |

---

## Predication

The 4-bit predicate field `{PN, PRED[2:0]}` works as follows:

- `PRED=0`: P0 = always true → instruction always executes (PN is ignored)
- `PRED=N, PN=0`: execute if P(N) is true
- `PRED=N, PN=1`: execute if P(N) is false

Assembly syntax: `@P3 ADD R1, R2, R3` or `@!P3 ADD R1, R2, R3`.

No predicate prefix means `@P0` (always execute).

---

## Legality Rules

1. `R0` as destination is legal but the write is discarded.
2. Reserved bits must be zero; decoders should ignore them for forward compat.
3. `SETP` uses `RD[2:0]` as the predicate register index; `RD[4:3]` must be 0.
4. Branch offsets that target odd byte addresses are illegal (but encoder won't
   generate them since offset is in words).
5. `LIMM`'s second word may have any bit pattern — it is pure data.
6. A `LIMM` at the very last word of the binary (no room for word 2) is
   an illegal encoding; the decoder should emit `.unknown`.

---

## Encoding Examples

**`ADD R1, R2, R3`** (no predicate = @P0):
```
PN=0, PRED=000, OPCODE=000000, RD=00001, RS1=00010, RS2=00011, FUNC=000, rsvd=0000
= 0x00_04_41_80  →  0000 0000 0000 0100 0100 0001 1000 0000
Let me recalculate:
Bit 31: 0 (PN)
Bits 30-28: 000 (PRED=P0)
Bits 27-22: 000000 (OPCODE=0x00)
Bits 21-17: 00001 (RD=R1)
Bits 16-12: 00010 (RS1=R2)
Bits 11-7:  00011 (RS2=R3)
Bits 6-4:   000 (FUNC=ADD)
Bits 3-0:   0000
= 0b 0_000_000000_00001_00010_00011_000_0000
= 0x00022180
```

**`@!P2 ADDI R5, R6, -1`**:
```
PN=1, PRED=010, OPCODE=001000, RD=00101, RS1=00110, RS2/IMM12=111111111111
= 0b 1_010_001000_00101_00110_111111111111
= 0xA20ACFFF  (verify with implementation)
```
