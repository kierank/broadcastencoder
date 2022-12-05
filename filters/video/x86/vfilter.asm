%include "x86util.asm"

SECTION_RODATA 32

two:   times 16 dw 2
three: times 16 dw 3
scale: times 16 dw 511

SECTION .text

cextern dithers

;
; obe_dither_plane_10_to_8( uint16_t *src, int src_stride, uint8_t *dst, int dst_stride, int width, int height )
;

%macro dither_plane 0

cglobal dither_plane_10_to_8, 6, 10, 6+cpuflag(avx2), src, src_stride, dst, dst_stride, width, height
    %define cur_row r6
    %define dither r7
    %define org_w r8
    mova      m5, [scale]
    lea       srcq, [srcq+2*widthq]
    add       dstq, widthq
    neg       widthq
    mov       org_w, widthq
    xor       cur_row, cur_row
    lea       r9, [rel dithers]

    .loop1:
        mov       dither, cur_row
        and       dither, 7
        shl       dither, 4
        VBROADCASTI128 m2, [r9 + dither]

        .loop2:
            mova m0, [srcq+2*widthq]        ;  0..15
            mova m1, [srcq+2*widthq+mmsize] ; 16..31

            punpcklwd m3, m0, m2 ;  0..3    8..11
            punpcklwd m4, m1, m2 ; 16..19  24..27
            punpckhwd m0, m2     ;  4..7   12..15
            punpckhwd m1, m2     ; 20..23  28..31

            pmaddwd   m3, m5
            pmaddwd   m4, m5
            pmaddwd   m0, m5
            pmaddwd   m1, m5

            psrld     m3, 11
            psrld     m4, 11
            psrld     m0, 11
            psrld     m1, 11

            packssdw  m3, m0 ;  0..3  8..11  4..7 12..15
            packssdw  m4, m1 ; 16..19 24..27 20..23 28..31
            %if cpuflag(avx2)
                SBUTTERFLY dqqq, 3, 4, 6
            %endif
            packuswb  m3, m4
            mova      [dstq+widthq], m3

            add       widthq, mmsize
        jl        .loop2

        add       srcq, src_strideq
        add       dstq, dst_strideq
        mov       widthq, org_w

        inc       cur_row
        cmp       cur_row, heightq
    jl        .loop1
REP_RET

%endmacro

INIT_XMM sse2
dither_plane
INIT_YMM avx2
dither_plane

;
; obe_downsample_chroma_fields_BITS( void *src_ptr, int src_stride, void *dst_ptr, int dst_stride, int width, int height )
;

%macro NEXT_field 0
    mov       widthq, org_w
    add       srcq, src_strideq
    add       r6, src_strideq
    add       dstq, dst_strideq
%endmacro

%macro DOWNSAMPLE_chroma_fields 1
%if %1 == 8
cglobal downsample_chroma_fields_%1, 6, 8, 8, src, src_stride, dst, dst_stride, width, height
%else
cglobal downsample_chroma_fields_%1, 6, 8, 4, src, src_stride, dst, dst_stride, width, height
%endif
    %define org_w r7
    mova      m0, [three]
    mova      m1, [two]
    %if %1 == 10
        add       widthd, widthd
    %endif
    add       srcq, widthq
    add       dstq, widthq
    neg       widthq
    mov       org_w, widthq
    lea       r6, [srcq+2*src_strideq]

    %if %1 == 8
    pxor      m7, m7

    .loop1:
        ; top field
        mova      m2, [srcq+widthq]
        mova      m5, [r6+widthq]

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
        mova      [dstq+widthq], m3
        add       widthq, mmsize
    jl        .loop1

    NEXT_field

    .loop2:
        ; bottom field
        mova      m2, [srcq+widthq]
        mova      m5, [r6+widthq]

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
        mova      [dstq+widthq], m3
        add       widthq, mmsize
    jl        .loop2

    %else

    .loop1:
        ; top field
        pmullw    m2, m0, [srcq+widthq]
        paddw     m3, m1, [r6+widthq]
        paddw     m2, m3
        psrlw     m2, 2

        mova      [dstq+widthq], m2
        add       widthq, mmsize
    jl        .loop1

    NEXT_field

    .loop2:
        ; bottom field
        pmullw    m2, m0, [r6+widthq]
        paddw     m3, m1, [srcq+widthq]
        paddw     m2, m3
        psrlw     m2, 2

        mova      [dstq+widthq], m2
        add       widthq, mmsize
    jl        .loop2
    %endif

    mov       widthq, org_w
    add       dstq, dst_strideq
    add       srcq, src_strideq
    lea       srcq, [srcq+2*src_strideq]
    lea       r6, [srcq+2*src_strideq]

    sub       heightq, 2
    jg        .loop1
REP_RET
%endmacro

INIT_XMM sse2
DOWNSAMPLE_chroma_fields 8
DOWNSAMPLE_chroma_fields 10

INIT_XMM avx
DOWNSAMPLE_chroma_fields 8
DOWNSAMPLE_chroma_fields 10

INIT_YMM avx2
DOWNSAMPLE_chroma_fields 8
DOWNSAMPLE_chroma_fields 10
