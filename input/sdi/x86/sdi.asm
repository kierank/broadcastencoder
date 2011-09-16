%include "x86inc.asm"
%include "x86util.asm"

SECTION .text

; downscale_line( uint16_t *src, uint8_t *dst, int lines );

%macro DOWNSCALE_line 1

cglobal downscale_line_%1, 3,3
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

INIT_MMX
DOWNSCALE_line mmx
INIT_XMM
DOWNSCALE_line sse2
