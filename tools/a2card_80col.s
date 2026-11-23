; SPDX-License-Identifier: MPL-2.0
;
; tools/a2card_80col.s - the boot sector of a second 5.25" test disk of the Apple II card's host test
; (tools/a2card_test.sh, docs/20-apple-ii-card.md): our own 6502 code for Aurora65's own test ROM (loaded at $0800,
; run at $0801), for an enhanced IIe.
;
; Why (23 November 2026): on a one-bit screen the card draws 80 columns from the text cells in Aurora65's font at 6 x 8
; (480 x 192: 560 dots do not fit the SE's screen), and Aurora65's test ROM shows only 40 columns. This turns the IIe's
; 80 columns on (80COL, 80STORE), clears the screen in both memories, writes a line into row 2 a character at a time,
; the even columns into auxiliary memory (PAGE2 with 80STORE) and the odd ones into main memory, as the IIe's 80-column
; firmware does, inverse from column 40 on, and waits in a jump to itself (Aurora65's idle).

        .cpu 6502
PTR     = $F4

        .org $0800
        .byte 1
        sta $C001               ; 80STORE on
        sta $C00D               ; 80COL on
        lda $C051               ; text
        lda $C054               ; PAGE2 off: main memory
        jsr clear
        lda $C055               ; PAGE2 on: auxiliary memory
        jsr clear
        lda $C054
        ldx #0
line:   lda msg,x
        beq done
        txa
        lsr                     ; the column's byte: column / 2 ...
        tay
        bcs odd                 ; ... in main memory for an odd column, auxiliary for an even one
        lda $C055
        bcc put
odd:    lda $C054
put:    lda msg,x
        cpx #40
        bcc normal
        and #$3F                ; inverse from column 40 ($00-$3F: the inverse letters)
        bcs store
normal: ora #$80
store:  sta $0500,y             ; row 2
        lda $C054
        inx
        bne line
done:   jmp done                ; a jump to itself: idle

; the text page ($0400-$07FF) with spaces
clear:  lda #$00
        sta PTR
        lda #$04
        sta PTR + 1
        ldy #0
        lda #$A0
cl1:    sta (PTR),y
        iny
        bne cl1
        inc PTR + 1
        ldx PTR + 1
        cpx #$08
        bne cl1
        rts

msg:    .byte "AURORA68 APPLE II CARD: 80 COLUMNS FROM THE CELLS, 6 X 8 ON ONE BIT", 0
        .assert * <= $0900, "one sector"
