%include "x86inc.asm"
%include "x86util.asm"

SECTION .rodata

align 32
scale: times 4 dd 511
shift: dd 11

SECTION .text

;
; obe_dither_row_10_to_8( uint16_t *src, uint8_t *dst, const uint16_t *dithers, int width, int stride )
;

%macro DITHER_row 1

cglobal dither_row_10_to_8_%1, 5, 5
    mova      m2, [r2]
    mova      m3, [scale]
    movd      m4, [shift]
    pxor      m5, m5
.loop
    mova      m0, [r0]
    paddw     m0, m2

    punpcklwd m1, m0, m5
    punpckhwd m0, m5
    pmulld    m1, m3
    pmulld    m0, m3
    psrld     m1, m4
    psrld     m0, m4

    packusdw  m1, m0
    packuswb  m1, m5

    movq      [r1], m1

    add       r0, mmsize
    add       r1, 8

    sub       r4d, 8
    jg        .loop
    REP_RET
%endmacro

INIT_XMM
DITHER_row sse4
INIT_AVX
DITHER_row avx
