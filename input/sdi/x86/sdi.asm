%include "x86util.asm"

SECTION_RODATA 32

; for AVX2 version only
v210_luma_permute: dd 0,1,2,4,5,6,7,7  ; 32-byte alignment required
v210_chroma_shuf2: db 0,1,2,3,4,5,8,9,10,11,12,13,-1,-1,-1,-1
v210_luma_shuf_avx2: db 0,1,4,5,6,7,8,9,12,13,14,15,-1,-1,-1,-1
v210_chroma_shuf_avx2: db 0,1,4,5,10,11,-1,-1,2,3,8,9,12,13,-1,-1

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
.loop:
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
cglobal v210_planar_unpack_%1, 5, 5, 6 + 2 * cpuflag(avx2), src, y, u, v, w
    movsxdifnidn wq, wd
    lea    yq, [yq+2*wq]
    add    uq, wq
    add    vq, wq
    neg    wq

    VBROADCASTI128   m3, [v210_mult]

%if cpuflag(avx2)
    VBROADCASTI128   m4, [v210_luma_shuf_avx2]
    VBROADCASTI128   m5, [v210_chroma_shuf_avx2]
    mova             m6, [v210_luma_permute]
    VBROADCASTI128   m7, [v210_chroma_shuf2]
%else
    VBROADCASTI128   m4, [v210_luma_shuf]
    VBROADCASTI128   m5, [v210_chroma_shuf]
%endif

.loop:
%ifidn %1, unaligned
    movu   m0, [srcq]  ; yB v5 yA  u5 y9 v4  y8 u4 y7  v3 y6 u3  y5 v2 y4  u2 y3 v1  y2 u1 y1  v0 y0 u0
%else
    mova   m0, [srcq]
%endif

    pmullw m1, m0, m3
    pslld  m0, 12
    psrlw  m1, 6                       ; yB yA u5 v4 y8 y7 v3 u3 y5 y4 u2 v1 y2 y1 v0 u0
    psrld  m0, 22                      ; 00 v5 00 y9 00 u4 00 y6 00 v2 00 y3 00 u1 00 y0

%if cpuflag(avx2)
    vpblendd m2, m1, m0, 0x55          ; yB yA 00 y9 y8 y7 00 y6 y5 y4 00 y3 y2 y1 00 y0
    pshufb m2, m4                      ; 00 00 yB yA y9 y8 y7 y6 00 00 y5 y4 y3 y2 y1 y0
    vpermd m2, m6, m2                  ; 00 00 00 00 yB yA y9 y8 y7 y6 y5 y4 y3 y2 y1 y0
    movu   [yq+2*wq], m2

    vpblendd m1, m1, m0, 0xaa          ; 00 v5 u5 v4 00 u4 v3 u3 00 v2 u2 v1 00 u1 v0 u0
    pshufb m1, m5                      ; 00 v5 v4 v3 00 u5 u4 u3 00 v2 v1 v0 00 u2 u1 u0
    vpermq m1, m1, 0xd8                ; 00 v5 v4 v3 00 v2 v1 v0 00 u5 u4 u3 00 u2 u1 u0
    pshufb m1, m7                      ; 00 00 v5 v4 v3 v2 v1 v0 00 00 u5 u4 u3 u2 u1 u0

    movu   [uq+wq], xm1
    vextracti128 [vq+wq], m1, 1
%else
    shufps m2, m1, m0, 0x8d            ; 00 y9 00 y6 yB yA y8 y7 00 y3 00 y0 y5 y4 y2 y1
    pshufb m2, m4                      ; 00 00 yB yA y9 y8 y7 y6 00 00 y5 y4 y3 y2 y1 y0
    movu   [yq+2*wq], m2

    shufps m1, m0, 0xd8                ; 00 v5 00 u4 u5 v4 v3 u3 00 v2 00 u1 u2 v1 v0 u0
    pshufb m1, m5                      ; 00 v5 v4 v3 00 u5 u4 u3 00 v2 v1 v0 00 u2 u1 u0

    movq   [uq+wq], m1
    movhps [vq+wq], m1
%endif

    add srcq, mmsize
    add wq, (mmsize*3)/8
    jl  .loop

    REP_RET
%endmacro

INIT_XMM ssse3
v210_planar_unpack unaligned
INIT_XMM avx
v210_planar_unpack unaligned
INIT_YMM avx2
v210_planar_unpack unaligned

INIT_XMM ssse3
v210_planar_unpack aligned
INIT_XMM avx
v210_planar_unpack aligned
INIT_YMM avx2
v210_planar_unpack aligned

INIT_ZMM avx512icl

cglobal v210_planar_unpack, 5, 5, 6, src, y, u, v, w
    movsxdifnidn wq, wd
    lea    yq, [yq+2*wq]
    add    uq, wq
    add    vq, wq
    neg    wq

    kmovw k1, [kmask]   ; odd dword mask
    kmovw k2, [kmask+2] ; even dword mask

    VBROADCASTI128 m0, [shift]
    mova           m1, [perm_y]
    mova           m2, [perm_uv]

    .loop:
        movu    m3, [srcq]
        vpsllvw m4, m3, m0
        pslld   m5, m3, 12
        psrlw   m4, 6
        psrld   m5, 22

        vpblendmd m3{k1}, m4, m5
        vpermb    m3, m1, m3 ; could use vpcompressw
        movu      [yq+2*wq], m3

        vpblendmd     m5{k2}, m4, m5
        vpermb        m5, m2, m5
        movu          [uq+wq], ym5
        vextracti32x8 [vq+wq], zm5, 1

        add srcq, mmsize
        add wq, (mmsize*3)/8
    jl  .loop
RET

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
    mova   m2, [rel v210_nv20_mask]
    mova   m3, [rel v210_nv20_luma_shuf]
    mova   m4, [rel v210_nv20_chroma_shuf]
    mova   m5, [rel v210_nv20_mult] ; also functions as vpermd index for avx2
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
