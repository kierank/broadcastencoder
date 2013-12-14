%include "x86util.asm"

SECTION_RODATA

v210_mask: times 4 dd 0x3ff
v210_mult: dw 64,4,64,4,64,4,64,4
v210_luma_shuf: db 8,9,0,1,2,3,12,13,4,5,6,7,-1,-1,-1,-1
v210_chroma_shuf: db 0,1,8,9,6,7,-1,-1,2,3,4,5,12,13,-1,-1

v210_nv20_mask: times 4 dq 0xc00ffc003ff003ff
v210_nv20_luma_shuf: times 2 db 1,2,4,5,6,7,9,10,12,13,14,15,12,13,14,15
v210_nv20_chroma_shuf: times 2 db 0,1,2,3,5,6,8,9,10,11,13,14,10,11,13,14
; vpermd indices {0,1,2,4,5,7,_,_} merged in the 3 lsb of each dword to save a register
v210_nv20_mult: dw 0x2000,0x7fff,0x0801,0x2000,0x7ffa,0x0800,0x7ffc,0x0800
                dw 0x1ffd,0x7fff,0x07ff,0x2000,0x7fff,0x0800,0x7fff,0x0800

SECTION .text

; downscale_line( uint16_t *src, uint8_t *dst, int lines );

%macro DOWNSCALE_line 0

cglobal downscale_line, 3,3,2
    imul r2d, 1440
.loop
    mova   m0, [r0]
    mova   m1, [r0+mmsize]
    psrlw  m0, 2
    psrlw  m1, 2
    packuswb m0, m1
    mova   [r1], m0

    lea    r0, [r0+2*mmsize]
    lea    r1, [r1+mmsize]
    sub    r2d, mmsize
    jg   .loop
    REP_RET
%endmacro

INIT_MMX mmx
DOWNSCALE_line
INIT_XMM sse2
DOWNSCALE_line

%macro v210_planar_unpack 1

; v210_planar_unpack(const uint32_t *src, uint16_t *y, uint16_t *u, uint16_t *v, int width)
cglobal v210_planar_unpack_%1, 5, 5, 7
    movsxdifnidn r4, r4d
    lea    r1, [r1+2*r4]
    add    r2, r4
    add    r3, r4
    neg    r4

    mova   m3, [v210_mult]
    mova   m4, [v210_mask]
    mova   m5, [v210_luma_shuf]
    mova   m6, [v210_chroma_shuf]
.loop
%ifidn %1, unaligned
    movu   m0, [r0]
%else
    mova   m0, [r0]
%endif

    pmullw m1, m0, m3
    psrld  m0, 10
    psrlw  m1, 6  ; u0 v0 y1 y2 v1 u2 y4 y5
    pand   m0, m4 ; y0 __ u1 __ y3 __ v2 __

    shufps m2, m1, m0, 0x8d ; y1 y2 y4 y5 y0 __ y3 __
    pshufb m2, m5 ; y0 y1 y2 y3 y4 y5 __ __
    movu   [r1+2*r4], m2

    shufps m1, m0, 0xd8 ; u0 v0 v1 u2 u1 __ v2 __
    pshufb m1, m6 ; u0 u1 u2 __ v0 v1 v2 __
    movq   [r2+r4], m1
    movhps [r3+r4], m1

    add r0, mmsize
    add r4, 6
    jl  .loop

    REP_RET
%endmacro

INIT_XMM ssse3
v210_planar_unpack unaligned
INIT_XMM avx
v210_planar_unpack unaligned

INIT_XMM ssse3
v210_planar_unpack aligned
INIT_XMM avx
v210_planar_unpack aligned


%macro PLANE_DEINTERLEAVE_V210 0
;-----------------------------------------------------------------------------
; void obe_v210_line_to_nv20( uint16_t *dsty, intptr_t i_dsty,
;                             uint16_t *dstc, intptr_t i_dstc,
;                             uint32_t *src, intptr_t i_src, intptr_t w, intptr_t h )
;-----------------------------------------------------------------------------
%if ARCH_X86_64
cglobal v210_line_to_nv20, 8,10,7
%define src   r8
%define org_w r9
%define h     r7d
%else
cglobal v210_line_to_nv20, 7,7,7
%define src   r4m
%define org_w r6m
%define h     dword r7m
%endif
    add    r6, r6
    add    r0, r6
    add    r2, r6
    neg    r6
    mov   src, r4
    mov org_w, r6
    mova   m2, [v210_nv20_mask]
    mova   m3, [v210_nv20_luma_shuf]
    mova   m4, [v210_nv20_chroma_shuf]
    mova   m5, [v210_nv20_mult] ; also functions as vpermd index for avx2
    pshufd m6, m5, q1102

ALIGN 16
.loop:
    movu   m1, [r4]
    pandn  m0, m2, m1
    pand   m1, m2
    pshufb m0, m3
    pshufb m1, m4
    pmulhrsw m0, m5 ; y0 y1 y2 y3 y4 y5 __ __
    pmulhrsw m1, m6 ; u0 v0 u1 v1 u2 v2 __ __
%if mmsize == 32
    vpermd m0, m5, m0
    vpermd m1, m5, m1
%endif
    movu [r0+r6], m0
    movu [r2+r6], m1
    add    r4, mmsize
    add    r6, 3*mmsize/4
    jl .loop
    add    r0, r1
    add    r2, r3
    add   src, r5
    mov    r4, src
    mov    r6, org_w
    dec     h
    jg .loop
    RET
%endmacro ; PLANE_DEINTERLEAVE_V210

INIT_XMM ssse3
PLANE_DEINTERLEAVE_V210
INIT_XMM avx
PLANE_DEINTERLEAVE_V210
;INIT_YMM avx2
;PLANE_DEINTERLEAVE_V210
