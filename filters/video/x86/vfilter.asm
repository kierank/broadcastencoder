%include "x86util.asm"

SECTION .rodata

align 32
scale: times 4 dd 511
shift: dd 11

align 32
two: times 8 dw 2
three: times 8 dw 3

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

;
; obe_downsample_chroma_row_field( uint16_t *src, uint16_t *dst, int width, int stride )
;

; %1 * 3
; %2 + 2
%macro DOWNSAMPLE_chroma_row_inner 2
    pmullw  m2, m0, [%1+r2]
    paddw   m3, m1, [%2+r2]
    paddw   m2, m3
    psrlw   m2, 2
%endmacro

%macro DOWNSAMPLE_chroma_row 1
cglobal downsample_chroma_row_%1, 4, 5, 4
    mova m0, [three]
    mova m1, [two]
    add       r0, r2
    add       r1, r2
    lea       r4, [r0+2*r3]
    neg       r2
.loop

%ifidn %1, top
    DOWNSAMPLE_chroma_row_inner r0, r4
%else
    DOWNSAMPLE_chroma_row_inner r4, r0
%endif

    mova      [r1+r2], m2

    add       r2, mmsize
    jl        .loop
    REP_RET
%endmacro

INIT_XMM sse2
DOWNSAMPLE_chroma_row top
DOWNSAMPLE_chroma_row bottom

INIT_XMM avx
DOWNSAMPLE_chroma_row top
DOWNSAMPLE_chroma_row bottom
