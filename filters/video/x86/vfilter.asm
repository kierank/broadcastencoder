%include "x86util.asm"

SECTION_RODATA 64

two:   times 32 dw 2
three: times 32 dw 3
db_3_1: times 32 db 3, 1
scale: times 32 dw 511

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
            movu m0, [srcq+2*widthq]        ;  0..7  |  8..15 | 16..23 | 24..31
            movu m1, [srcq+2*widthq+mmsize] ; 32..39 | 40..47 | 48..55 | 56..63

            %if cpuflag(avx512icl)
                vshufi32x4 m3, m0, m1, q2020 ; 0..7  | 16..23 | 32..39 | 48..55
                vshufi32x4 m4, m0, m1, q3131 ; 8..15 | 24..31 | 40..47 | 56..63
                SWAP 0,3
                SWAP 1,4
            %elif cpuflag(avx2)
                SBUTTERFLY dqqq, 0,1,6
            %endif

            punpcklwd m3, m0, m2            ;  0..3  | 16..19 | 32..35 | 48..51
            punpcklwd m4, m1, m2            ;  8..11 | 24..27 | 40..43 | 56..59
            punpckhwd m0, m2                ;  4..7  | 20..23 | 36..39 | 52..55
            punpckhwd m1, m2                ; 12..15 | 28..31 | 44..47 | 60..63

            pmaddwd   m3, m5
            pmaddwd   m4, m5
            pmaddwd   m0, m5
            pmaddwd   m1, m5

            psrld     m3, 11
            psrld     m4, 11
            psrld     m0, 11
            psrld     m1, 11

            %if cpuflag(sse4)
                packusdw m3, m0              ; 0..7  | 16..23 | 32..39 | 48..55
                packusdw m4, m1              ; 8..15 | 24..31 | 40..47 | 56..63
            %else
                packssdw m3, m0
                packssdw m4, m1
            %endif
            packuswb  m3, m4                 ; 0..15 | 16..31 | 32..47 | 48..63
            movu      [dstq+widthq], m3

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
INIT_ZMM avx512icl
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
cglobal downsample_chroma_fields_%1, 6, 8, 4 + 4*(%1==8), src, src_stride, dst, dst_stride, width, height
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
        movu      m2, [srcq+widthq]
        movu      m5, [r6+widthq]

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
        movu      [dstq+widthq], m3
        add       widthq, mmsize
    jl        .loop1

    NEXT_field

    .loop2:
        ; bottom field
        movu      m2, [srcq+widthq]
        movu      m5, [r6+widthq]

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
        movu      [dstq+widthq], m3
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

%macro DOWNSAMPLE_chroma_fields_new8 0

cglobal downsample_chroma_fields_8, 6, 8, 5, src, src_stride, dst, dst_stride, width, height
    mova m0, [db_3_1]
    mova m1, [two]
    add  srcq, widthq
    add  dstq, widthq
    neg  widthq
    mov  r7, widthq
    lea  r6, [srcq+2*src_strideq]

    .loop1:
        ; top field
        movu      m2, [srcq+widthq] ; a0..a15 | a16..a31
        movu      m4, [r6+widthq]   ; b0..b15 | b16..b31

        punpcklbw m3, m2, m4 ; a0 b0 .. a7 b7   | a16 b16 .. a23 b23
        punpckhbw m2, m4     ; a8 b8 .. a15 b15 | a24 b24 .. a31 b31

        pmaddubsw m3, m0 ; c0 .. c7  | c16 .. c23
        pmaddubsw m2, m0 ; c8 .. c15 | c24 .. c31

        paddw     m3, m1
        paddw     m2, m1

        psrlw     m2, 2
        psrlw     m3, 2

        packuswb  m3, m2
        movu      [dstq+widthq], m3
        add       widthq, mmsize
    jl        .loop1

    NEXT_field

    .loop2:
        ; bottom field
        movu      m2, [srcq+widthq]
        movu      m4, [r6+widthq]
        punpcklbw m3, m4, m2 ; different interleave order
        punpckhbw m4, m4, m2
        SWAP 2,4
        pmaddubsw m3, m0
        pmaddubsw m2, m0
        paddw     m3, m1
        paddw     m2, m1
        psrlw     m2, 2
        psrlw     m3, 2
        packuswb  m3, m2
        movu      [dstq+widthq], m3
        add       widthq, mmsize
    jl        .loop2

    mov       widthq, r7
    add       dstq, dst_strideq
    add       srcq, src_strideq
    lea       srcq, [srcq+2*src_strideq]
    lea       r6, [srcq+2*src_strideq]

    sub       heightq, 2
    jg        .loop1
RET

%endmacro

INIT_XMM sse2
DOWNSAMPLE_chroma_fields 8
DOWNSAMPLE_chroma_fields 10

INIT_XMM ssse3
DOWNSAMPLE_chroma_fields_new8

INIT_XMM avx
DOWNSAMPLE_chroma_fields_new8
DOWNSAMPLE_chroma_fields 10

INIT_YMM avx2
DOWNSAMPLE_chroma_fields_new8
DOWNSAMPLE_chroma_fields 10

INIT_ZMM avx512icl
DOWNSAMPLE_chroma_fields_new8
DOWNSAMPLE_chroma_fields 10
