%include "x86util.asm"

SECTION .rodata

align 32
scale: times 4 dd 511
shift: dd 11

SECTION .text

;
; obe_dither_row_10_to_8( uint16_t *src, uint8_t *dst, const uint16_t *dithers, int width, int stride )
;

%macro DITHER_row 0

cglobal dither_row_10_to_8, 5, 5, 8
    mova      m2, [r2]
    mova      m5, [scale]
    movd      m6, [shift]
    pxor      m7, m7
    lea       r0, [r0+2*r3]
    add       r1, r3
    neg       r3

.loop
    paddw     m0, m2, [r0+2*r3]
    paddw     m1, m2, [r0+2*r3+16]

    punpcklwd m3, m0, m7
    punpcklwd m4, m1, m7
    punpckhwd m0, m7
    punpckhwd m1, m7
    pmulld    m3, m5
    pmulld    m4, m5
    pmulld    m0, m5
    pmulld    m1, m5
    psrld     m3, m6
    psrld     m4, m6
    psrld     m0, m6
    psrld     m1, m6
    packusdw  m3, m0
    packusdw  m4, m1

    packuswb  m3, m4
    mova      [r1+r3], m3

    add       r3, mmsize
    jl        .loop
    REP_RET
%endmacro

INIT_XMM sse4
DITHER_row
INIT_XMM avx
DITHER_row
