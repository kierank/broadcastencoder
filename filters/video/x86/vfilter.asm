%include "x86util.asm"

SECTION .rodata

align 32
scale: times 4 dd 511
shift: dd 11

align 32
two:   times 8 dw 2
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
; obe_downsample_chroma_fields_BITS( void *src_ptr, int src_stride, void *dst_ptr, int dst_stride, int width, int height )
;

%macro NEXT_field 0
    mov       r4, org_w
    add       r0, r1
    add       r6, r1
    add       r2, r3
%endmacro

%macro DOWNSAMPLE_chroma_fields 1
%ifidn %1, 8
cglobal downsample_chroma_fields_%1, 6, 8, 8
%else
cglobal downsample_chroma_fields_%1, 6, 8, 4
%endif
%define h     r5d
%define org_w r7
    mova      m0, [three]
    mova      m1, [two]
%ifidn %1, 10
    shl       r4d, 1
%endif
    add       r0, r4
    add       r2, r4
    neg       r4
    mov       org_w, r4
    lea       r6, [r0+2*r1]

%ifidn %1, 8
    pxor      m7, m7

.loop1
    ; top field
    mova      m2, [r0+r4]
    mova      m5, [r6+r4]

    punpcklbw m3, m2, m7
    punpckhbw m2, m7
    punpcklbw m4, m5, m7
    punpckhbw m5, m7

    pmullw    m2, m0
    pmullw    m3, m0
    paddw     m4, m1
    paddw     m5, m1
    paddw     m2, m5
    paddw     m3, m4
    psrlw     m2, 2
    psrlw     m3, 2

    packuswb  m3, m2
    mova      [r2+r4], m3
    add       r4, mmsize
    jl        .loop1

    NEXT_field

.loop2
    ; bottom field
    mova      m2, [r0+r4]
    mova      m5, [r6+r4]

    punpcklbw m3, m2, m7
    punpckhbw m2, m7
    punpcklbw m4, m5, m7
    punpckhbw m5, m7

    pmullw    m4, m0
    pmullw    m5, m0
    paddw     m2, m1
    paddw     m3, m1
    paddw     m2, m5
    paddw     m3, m4
    psrlw     m2, 2
    psrlw     m3, 2

    packuswb  m3, m2
    mova      [r2+r4], m3
    add       r4, mmsize
    jl        .loop2

%else

.loop1
    ; top field
    pmullw    m2, m0, [r0+r4]
    paddw     m3, m1, [r6+r4]
    paddw     m2, m3
    psrlw     m2, 2

    mova      [r2+r4], m2
    add       r4, mmsize
    jl        .loop1

    NEXT_field

.loop2
    ; bottom field
    pmullw    m2, m0, [r6+r4]
    paddw     m3, m1, [r0+r4]
    paddw     m2, m3
    psrlw     m2, 2

    mova      [r2+r4], m2
    add       r4, mmsize
    jl        .loop2
%endif
    mov       r4, org_w
    add       r2, r3
    add       r0, r1
    lea       r0, [r0+2*r1]
    lea       r6, [r0+2*r1]

    sub       h, 2
    jg        .loop1
    REP_RET
%endmacro

INIT_XMM sse2
DOWNSAMPLE_chroma_fields 8
DOWNSAMPLE_chroma_fields 10

INIT_XMM avx
DOWNSAMPLE_chroma_fields 8
DOWNSAMPLE_chroma_fields 10
