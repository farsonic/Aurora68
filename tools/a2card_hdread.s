; SPDX-License-Identifier: MPL-2.0
;
; tools/a2card_hdread.s - block 0 of a hard disk test volume of the Apple II card's host test (tools/a2card_test.sh,
; docs/20-apple-ii-card.md "Disks from the Mac"): our own 6502 code, assembled with Aurora65's tools/a65asm.py and put
; into a volume made by Aurora65's tools/mkdisk.py (po --boot). A block device's firmware loads it at $0800 and runs it
; at $0801 with X = the slot x 16 (Aurora65's rom/bootblk.s, whose calls this copies).
;
; Why (24 November 2026): a hard disk image on the Mac is read on demand, 32 blocks at a time, and the Apple waits in
; its block card's firmware while a piece is fetched. Aurora65's boot block reads three blocks; this reads 2048 in a row
; from block 64 (1 MB: 64 pieces, each one's first read waiting for the Mac), checks mkdisk.py's pattern in each ("A65B"
; and its number), then writes 96 blocks from 4096 on ("A65X" and the number: three pieces' worth, written back to the
; Mac's file by the card) and reads them back. It says READ OK and WRITE OK, or BAD and the block, then the ROM's STOP.

        .cpu 6502
PRINT   = $F803
PRHEX   = $F806
CROUT   = $F809
STOP    = $F813
CMD     = $42
UNIT    = $43
BUFP    = $44
BLK     = $46
ENTRY   = $FC
LEFT    = $FA                   ; blocks still to do
RBUF    = $1000

        .org $0800
        .byte 1
        stx UNIT
        txa
        lsr
        lsr
        lsr
        lsr
        ora #$C0
        sta ENTRY + 1
        sta $FF
        lda #$FF
        sta $FE
        ldy #$00
        lda ($FE),y             ; $CnFF: the driver entry's low byte
        sta ENTRY
        lda #<RBUF
        sta BUFP
        lda #>RBUF
        sta BUFP + 1
; read blocks 64-2111
        lda #64
        sta BLK
        lda #0
        sta BLK + 1
        lda #<2048
        sta LEFT
        lda #>2048
        sta LEFT + 1
rd:     lda #1
        jsr call
        bcs bad1
        ldy #$03
rd1:    lda RBUF,y
        cmp bmagic,y
        bne bad1
        dey
        bpl rd1
        jsr numok
        bne bad1
        jsr next
        bne rd
        jmp rddone
bad1:   jmp bad                 ; (out of a branch's reach)
rddone:
        lda #<rdok
        ldy #>rdok
        jsr PRINT
; write blocks 4096-4191 with "A65X" and the number, then read them back
        lda #0
        sta BLK
        lda #$10
        sta BLK + 1
        lda #96
        sta LEFT
        lda #0
        sta LEFT + 1
wr:     ldy #$00
wr1:    tya
        eor BLK
        sta RBUF,y
        sta RBUF + 256,y
        iny
        bne wr1
        ldy #$03
wr2:    lda xmagic,y
        sta RBUF,y
        dey
        bpl wr2
        lda BLK
        sta RBUF + 4
        lda BLK + 1
        sta RBUF + 5
        lda #2
        jsr call
        bcs bad1
        jsr next
        bne wr
        lda #0
        sta BLK
        lda #$10
        sta BLK + 1
        lda #96
        sta LEFT
vr:     lda #0
        sta RBUF
        lda #1
        jsr call
        bcs bad
        ldy #$03
vr1:    lda RBUF,y
        cmp xmagic,y
        bne bad
        dey
        bpl vr1
        jsr numok
        bne bad
        jsr next
        bne vr
        lda #<wrok
        ldy #>wrok
        jsr PRINT
        jmp STOP
bad:    lda #<badm
        ldy #>badm
        jsr PRINT
        lda BLK + 1
        jsr PRHEX
        lda BLK
        jsr PRHEX
        jsr CROUT
        jmp STOP
; the block's number in bytes 4-5 of what was read? Z set when it is
numok:  lda RBUF + 4
        cmp BLK
        bne nk
        lda RBUF + 5
        cmp BLK + 1
nk:     rts
; the next block; Z set when none is left
next:   inc BLK
        bne nx1
        inc BLK + 1
nx1:    lda LEFT
        bne nx2
        dec LEFT + 1
nx2:    dec LEFT
        lda LEFT
        ora LEFT + 1
        rts
call:   sta CMD
        ldx UNIT
        jmp (ENTRY)

bmagic: .byte "A65B"
xmagic: .byte "A65X"
rdok:   .hibyte "HD READ 2048 OK", 13, 0
wrok:   .hibyte "HD WRITE 96 OK", 13, 0
badm:   .hibyte "HD BAD ", 0
