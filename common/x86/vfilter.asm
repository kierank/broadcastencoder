%include "x86inc.asm"
%include "x86util.asm"

SECTION .text

;
; obe_scale_plane( uint16_t *src, int stride, int width, int height, int lshift, int rshift )
;

%macro SCALE_plane 1

cglobal scale_plane_%1, 6,6
    imul   r1d, r3m
    movd    m2, r4m
    movd    m3, r5m
.loop
    mova    m0, [r0]
    psllw   m1, m0, m2
    psrlw   m0, m3
    paddw   m0, m1
    mova   [r0], m0
    sub     r1d, mmsize
    jg .loop
    REP_RET
%endmacro

INIT_MMX
SCALE_plane mmxext
INIT_XMM
SCALE_plane sse2
INIT_AVX
SCALE_plane avx
