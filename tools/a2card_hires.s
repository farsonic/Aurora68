; SPDX-License-Identifier: MPL-2.0
;
; tools/a2card_hires.s - the boot sector of a 5.25" test disk of the Apple II card's host test (tools/a2card_test.sh,
; docs/20-apple-ii-card.md): our own 6502 code, assembled with Aurora65's tools/a65asm.py, for Aurora65's own test
; ROM, whose Disk II boot (rom/a65test.s boot525) loads physical sector 0 of track 0 at $0800 and runs it at $0801.
;
; Why (23 November 2026): Aurora65's test ROM and disks show only 40-column text, and the card's one-bit picture of
; hires at 1:1 (each Apple pixel one Mac pixel, the owner's choice) wants a picture whose every pixel is known. This
; fills hires page 1 by pages: the first half of each 8-line group's lines ($2000-$2FFF) with $55 and $2A in turn
; (pixels on and off in every other column, across byte boundaries: 1010101 0101010), the second half ($3000-$3FFF)
; with $7F (all on), so the picture is bands of 4 lines of stripes and 4 lines of white; then mixed hires with text,
; "HIRES 1:1" in the text window's first row, and the ROM's STOP (prints STOPPED, beeps, echoes keys) below it.

        .cpu 6502
PRINT   = $F803
STOP    = $F813
CH      = $24
CV      = $25
BASL    = $28
PTR     = $F4

        .org $0800
        .byte 1                 ; sectors to load (the P5 PROM's convention)
        lda #$00
        sta PTR
        lda #$20
        sta PTR + 1
        ldy #$00
fill:   lda PTR + 1
        cmp #$30
        bcs white
        tya
        and #$01
        beq even
        lda #$2A
        bne put
even:   lda #$55
        bne put
white:  lda #$7F
put:    sta (PTR),y
        iny
        bne fill
        inc PTR + 1
        lda PTR + 1
        cmp #$40
        bne fill
        lda $C050               ; graphics
        lda $C057               ; hires
        lda $C053               ; mixed: text in rows 20-23
        lda $C054               ; page 1
        lda #20                 ; the cursor to row 20 ($0650), column 0
        sta CV
        lda #$50
        sta BASL
        lda #$06
        sta BASL + 1
        lda #0
        sta CH
        lda #<msg
        ldy #>msg
        jsr PRINT
        jmp STOP
msg:    .hibyte "HIRES 1:1", 13, 0
        .assert * <= $0900, "one sector"
