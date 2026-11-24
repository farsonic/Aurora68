; SPDX-License-Identifier: MPL-2.0
;
; tools/a2card_print.s - the boot sector of a 5.25" test disk for the Apple II card in the testbed (docs/20-apple-ii-card.md,
; "Drawn over another window"): our own 6502 code for Aurora65's own test ROM (loaded at $0800, run at $0801).
;
; Why (24 November 2026): on the owner's SE a Finder window dragged over the Apple II window while the Apple was busy at
; its BASIC prompt kept a rectangle of the Apple's screen. Aurora65's test ROM has no BASIC; this prints a numbered line
; for ever through the ROM's own print, hex and new-line routines, so the screen scrolls and every picture the card
; draws is the whole window, as when the owner typed and the Apple answered. Assembled with Aurora65's tools/a65asm.py
; and padded to 143,360 bytes as tools/a2card_test.sh's mkdsk does.

        .cpu 6502
PRINT   = $F803                 ; Aurora65's test ROM: the string at A (low) Y (high), 0-terminated
PRHEX   = $F806                 ; A as two hex digits
CROUT   = $F809                 ; a new line (the screen scrolls at the bottom)
N       = $E0                   ; the line number (the ROM uses $F0-$FB)

        .org $0800
        .byte 1
        lda #0
        sta N
        sta N + 1
loop:   lda #<msg
        ldy #>msg
        jsr PRINT
        lda N + 1
        jsr PRHEX
        lda N
        jsr PRHEX
        jsr CROUT
        inc N
        bne loop
        inc N + 1
        jmp loop
msg:    .byte "]PRINT ", 34, "HELLO", 34, ": HELLO, LINE $", 0
