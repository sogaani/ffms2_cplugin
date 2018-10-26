//  Copyright (c) 2010-2011 FFmpegSource Project
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy
//  of this software and associated documentation files (the "Software"), to deal
//  in the Software without restriction, including without limitation the rights
//  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//  copies of the Software, and to permit persons to whom the Software is
//  furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in
//  all copies or substantial portions of the Software.
//
//  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
//  THE SOFTWARE.

#include "avs_common.h"
#include "ff_filters.h"
#include <libavutil/common.h>
#include <libavutil/pixfmt.h>
#include <ffmscompat.h>

typedef struct
{
    int top;
    int bottom;
} frame_fields_t;

typedef struct
{
    AVS_FilterInfo *fi;
    FFMS_VideoSource *vid;
    int fps_num;
    int fps_den;
    int rff_mode;
    frame_fields_t *field_list;

    // env var names, because they're leaky otherwise
    char *var_name_vfr_time;
    char *var_name_pict_type;
} ffvideosource_filter_t;

static void AVSC_CC free_filter( AVS_FilterInfo *fi )
{
    ffvideosource_filter_t *filter = fi->user_data;
    FFMS_DestroyVideoSource( filter->vid );
    if( filter->field_list )
        free( filter->field_list );
    if( filter->var_name_vfr_time )
        free( filter->var_name_vfr_time );
    if( filter->var_name_pict_type )
        free( filter->var_name_pict_type );
    free( filter );
}

/* field: -1 = top, 0 = frame, 1 = bottom */
static void output_frame( AVS_FilterInfo *fi, AVS_VideoFrame *avs_frame, char field, const FFMS_Frame *ffms_frame )
{
    const static int planes[3] = { AVS_PLANAR_Y, AVS_PLANAR_U, AVS_PLANAR_V };
    uint8_t *dst[3];
    int dst_stride[3], plane = (avs_is_yuv( &fi->vi ) && !ffms_avs_lib.avs_is_y8( &fi->vi )) ? 3 : 1;
    fill_avs_frame_data( avs_frame, dst, dst_stride, 0, avs_is_rgb( &fi->vi ) );
    for( int i = 0; i < plane; i++ )
    {
        int height = ffms_avs_lib.avs_get_height_p( avs_frame, planes[i] ) / (1<<!!field);
        uint8_t *src = ffms_frame->Data[i];
        int src_stride = ffms_frame->Linesize[i];
        if( field == 1 ) // bottom
        {
            src += src_stride;
            dst[i] += dst_stride[i];
        }
        dst_stride[i] *= 1<<!!field;
        src_stride *= 1<<!!field;
        ffms_avs_lib.avs_bit_blt( fi->env, dst[i], dst_stride[i], src,
            src_stride, ffms_avs_lib.avs_get_row_size_p( avs_frame, planes[i] ), height );
    }
}

static AVS_VideoFrame * AVSC_CC get_frame( AVS_FilterInfo *fi, int n )
{
    ffvideosource_filter_t *filter = fi->user_data;
    n = FFMIN( FFMAX( n, 0 ), fi->vi.num_frames - 1 );

    init_ErrorInfo( ei );

    AVS_VideoFrame *dst = ffms_avs_lib.avs_new_video_frame_a( fi->env, &fi->vi, AVS_FRAME_ALIGN );
    if( filter->rff_mode > 0 )
    {
        const FFMS_Frame *frame = FFMS_GetFrame( filter->vid, FFMIN( filter->field_list[n].top, filter->field_list[n].bottom ), &ei );
        if( !frame )
            fi->error = ffms_avs_sprintf( "FFVideoSource: %s", ei.Buffer );
        if( filter->field_list[n].top == filter->field_list[n].bottom)
            output_frame( fi, dst, 0, frame );
        else
        {
            char bff = FFMIN( filter->field_list[n].top, filter->field_list[n].bottom ) == filter->field_list[n].bottom;
            bff = (!!bff)*2-1; /* 0,1 -> -1,1 */
            output_frame( fi, dst, bff, frame );
            frame = FFMS_GetFrame( filter->vid, FFMAX( filter->field_list[n].top, filter->field_list[n].bottom ), &ei );
            if( !frame )
                fi->error = ffms_avs_sprintf( "FFVideoSource: %s", ei.Buffer );
            output_frame( fi, dst, -bff, frame );
        }
    }
    else
    {
        const FFMS_Frame *frame;
        if( filter->fps_num > 0 && filter->fps_den > 0 )
            frame = FFMS_GetFrameByTime( filter->vid, FFMS_GetVideoProperties( filter->vid )->FirstTime
                + (double)(n * (int64_t)filter->fps_den) / filter->fps_num, &ei );
        else
        {
            frame = FFMS_GetFrame( filter->vid, n, &ei );
            FFMS_Track *track = FFMS_GetTrackFromVideo( filter->vid );
            const FFMS_TrackTimeBase *timebase = FFMS_GetTimeBase( track );
            ffms_avs_lib.avs_set_var( fi->env, filter->var_name_vfr_time,
                avs_new_value_int( (double)FFMS_GetFrameInfo( track, n )->PTS * timebase->Num / timebase->Den ) );
        }

        if( !frame )
            fi->error = ffms_avs_sprintf( "FFVideoSource: %s", ei.Buffer );

        ffms_avs_lib.avs_set_var( fi->env, filter->var_name_pict_type, avs_new_value_int( frame->PictType ) );
        output_frame( fi, dst, 0, frame );
    }

    return dst;
}

static int AVSC_CC get_parity( AVS_FilterInfo *fi, int n )
{
    return fi->vi.image_type == AVS_IT_TFF;
}

static int AVSC_CC get_audio( AVS_FilterInfo *fi, void *buf, INT64 start, INT64 count )
{
    return 0;
}

static int AVSC_CC set_cache_hints( AVS_FilterInfo *fi, int cachehints, int frame_range )
{
    return 0;
}

static AVS_Value init_output_format( ffvideosource_filter_t *filter, int dst_width, int dst_height,
    const char *resizer_name, const char *csp_name, const char *var_prefix )
{
    init_ErrorInfo( ei );

    const FFMS_VideoProperties *vidp = FFMS_GetVideoProperties( filter->vid );
    const FFMS_Frame *frame = FFMS_GetFrame( filter->vid, 0, &ei );
    if( !frame )
        return avs_new_value_error( ffms_avs_sprintf( "FFVideoSource: %s", ei.Buffer ) );

    int pix_fmts[45];
    pix_fmts[ 0 ] = AV_PIX_FMT_YUV420P;
    pix_fmts[ 1 ] = AV_PIX_FMT_YUYV422;
    pix_fmts[ 2 ] = AV_PIX_FMT_BGRA;
    pix_fmts[ 3 ] = AV_PIX_FMT_BGR24;
    pix_fmts[ 4 ] = AV_PIX_FMT_YUV422P;
    pix_fmts[ 5 ] = AV_PIX_FMT_YUV444P;
    pix_fmts[ 6 ] = AV_PIX_FMT_YUV411P;
    pix_fmts[ 7 ] = AV_PIX_FMT_GRAY8;
    pix_fmts[ 8 ] = AV_PIX_FMT_GRAY10;
    pix_fmts[ 9 ] = AV_PIX_FMT_GRAY12;
//    pix_fmts[ 9 ] = AV_PIX_FMT_GRAY14;
    pix_fmts[ 10 ] = AV_PIX_FMT_GRAY16;
    pix_fmts[ 11 ] = AV_PIX_FMT_BGRA64;
    pix_fmts[ 12 ] = AV_PIX_FMT_BGR48;
    pix_fmts[ 13 ] = AV_PIX_FMT_YUV420P10;
    pix_fmts[ 14 ] = AV_PIX_FMT_YUV422P10;
    pix_fmts[ 15 ] = AV_PIX_FMT_YUV444P10;
    pix_fmts[ 16 ] = AV_PIX_FMT_YUV420P12;
    pix_fmts[ 17 ] = AV_PIX_FMT_YUV422P12;
    pix_fmts[ 18 ] = AV_PIX_FMT_YUV444P12;
    pix_fmts[ 19 ] = AV_PIX_FMT_YUV420P14;
    pix_fmts[ 20 ] = AV_PIX_FMT_YUV422P14;
    pix_fmts[ 21 ] = AV_PIX_FMT_YUV444P14;
    pix_fmts[ 22 ] = AV_PIX_FMT_YUV420P16;
    pix_fmts[ 23 ] = AV_PIX_FMT_YUV422P16;
    pix_fmts[ 24 ] = AV_PIX_FMT_YUV444P16;
    pix_fmts[ 25 ] = AV_PIX_FMT_YUVA420P;
    pix_fmts[ 26 ] = AV_PIX_FMT_YUVA422P;
    pix_fmts[ 27 ] = AV_PIX_FMT_YUVA444P;
    pix_fmts[ 28 ] = AV_PIX_FMT_YUVA420P10;
    pix_fmts[ 29 ] = AV_PIX_FMT_YUVA422P10;
    pix_fmts[ 30 ] = AV_PIX_FMT_YUVA444P10;
//    pix_fmts[ 31 ] = AV_PIX_FMT_YUVA420P12;
//    pix_fmts[ 32 ] = AV_PIX_FMT_YUVA422P12;
//    pix_fmts[ 33 ] = AV_PIX_FMT_YUVA444P12;
//    pix_fmts[ 34 ] = AV_PIX_FMT_YUVA420P14;
//    pix_fmts[ 35 ] = AV_PIX_FMT_YUVA422P14;
//    pix_fmts[ 36 ] = AV_PIX_FMT_YUVA444P14;
    pix_fmts[ 31 ] = AV_PIX_FMT_YUVA420P16;
    pix_fmts[ 32 ] = AV_PIX_FMT_YUVA422P16;
    pix_fmts[ 33 ] = AV_PIX_FMT_YUVA444P16;
    pix_fmts[ 34 ] = AV_PIX_FMT_GBRP;
    pix_fmts[ 35 ] = AV_PIX_FMT_GBRP10;
    pix_fmts[ 36 ] = AV_PIX_FMT_GBRP12;
    pix_fmts[ 37 ] = AV_PIX_FMT_GBRP14;
    pix_fmts[ 38 ] = AV_PIX_FMT_GBRP16;
    pix_fmts[ 39 ] = AV_PIX_FMT_GBRAP;
    pix_fmts[ 40 ] = AV_PIX_FMT_GBRAP10;
    pix_fmts[ 41 ] = AV_PIX_FMT_GBRAP12;
//    pix_fmts[ 42 ] = AV_PIX_FMT_GBRAP14;
    pix_fmts[ 43 ] = AV_PIX_FMT_GBRAP16;
    pix_fmts[ 44 ] = -1;

    // AV_PIX_FMT_NV21 is misused as a return value different to the defined ones in the function
    enum AVPixelFormat dst_pix_fmt = ffms_avs_lib.csp_name_to_pix_fmt( csp_name, AV_PIX_FMT_NV21 );
    if( dst_pix_fmt == AV_PIX_FMT_NONE )
        return avs_new_value_error( "FFVideoSource: Invalid colorspace name specified" );

    if( dst_pix_fmt != AV_PIX_FMT_NV21 )
    {
        pix_fmts[ 0 ] = dst_pix_fmt;
        pix_fmts[ 1 ] = -1;
    }

    if( dst_width <= 0 )
        dst_width = frame->EncodedWidth;

    if( dst_height <= 0)
        dst_height = frame->EncodedHeight;

    int resizer = resizer_name_to_swscale_name( resizer_name );
    if( !resizer )
        return avs_new_value_error( "FFVideoSource: Invalid resizer name specified" );

    if( FFMS_SetOutputFormatV2( filter->vid, pix_fmts, dst_width, dst_height, resizer, &ei ) )
        return avs_new_value_error( "FFVideoSource: No suitable output format found" );

    frame = FFMS_GetFrame( filter->vid, 0, &ei );

    pix_fmts[ 0 ] = frame->ConvertedPixelFormat;
    pix_fmts[ 1 ] = -1;

    // This trick is required to first get the "best" default format and then set only that format as the output
    if( FFMS_SetOutputFormatV2( filter->vid, pix_fmts, dst_width, dst_height, resizer, &ei ) )
        return avs_new_value_error( "FFVideoSource: No suitable output format found" );

    frame = FFMS_GetFrame( filter->vid, 0, &ei );

    enum AVPixelFormat pix_fmt = frame->ConvertedPixelFormat;

    if( pix_fmt == AV_PIX_FMT_YUVJ420P || pix_fmt == AV_PIX_FMT_YUV420P )
        filter->fi->vi.pixel_type = AVS_CS_I420;
    else if( pix_fmt == AV_PIX_FMT_YUYV422 )
        filter->fi->vi.pixel_type = AVS_CS_YUY2;
    else if( pix_fmt == AV_PIX_FMT_BGRA )
        filter->fi->vi.pixel_type = AVS_CS_BGR32;
    else if( pix_fmt == AV_PIX_FMT_BGR24 )
        filter->fi->vi.pixel_type = AVS_CS_BGR24;
    else if( pix_fmt == AV_PIX_FMT_YUVJ422P || pix_fmt == AV_PIX_FMT_YUV422P )
        filter->fi->vi.pixel_type = AVS_CS_YV16;
    else if( pix_fmt == AV_PIX_FMT_YUVJ444P || pix_fmt == AV_PIX_FMT_YUV444P )
        filter->fi->vi.pixel_type = AVS_CS_YV24;
    else if( pix_fmt == AV_PIX_FMT_GRAY8 )
        filter->fi->vi.pixel_type = AVS_CS_Y8;
    else if( pix_fmt == AV_PIX_FMT_GRAY10 )
        filter->fi->vi.pixel_type = AVS_CS_Y10;
    else if( pix_fmt == AV_PIX_FMT_GRAY12 )
        filter->fi->vi.pixel_type = AVS_CS_Y12;
//    else if( pix_fmt == AV_PIX_FMT_GRAY14 )
//        filter->fi->vi.pixel_type = AVS_CS_Y14;
    else if( pix_fmt == AV_PIX_FMT_GRAY16 )
        filter->fi->vi.pixel_type = AVS_CS_Y16;
    else if( pix_fmt == AV_PIX_FMT_YUV411P )
        filter->fi->vi.pixel_type = AVS_CS_YV411;
    else if( pix_fmt == AV_PIX_FMT_BGRA64 )
        filter->fi->vi.pixel_type = AVS_CS_BGR64;
    else if( pix_fmt == AV_PIX_FMT_BGR48 )
        filter->fi->vi.pixel_type = AVS_CS_BGR48;
    else if( pix_fmt == AV_PIX_FMT_YUV420P10 )
        filter->fi->vi.pixel_type = AVS_CS_YUV420P10;
    else if( pix_fmt == AV_PIX_FMT_YUV422P10 )
        filter->fi->vi.pixel_type = AVS_CS_YUV422P10;
    else if( pix_fmt == AV_PIX_FMT_YUV444P10 )
        filter->fi->vi.pixel_type = AVS_CS_YUV444P10;
    else if( pix_fmt == AV_PIX_FMT_YUV420P12 )
        filter->fi->vi.pixel_type = AVS_CS_YUV420P12;
    else if( pix_fmt == AV_PIX_FMT_YUV422P12 )
        filter->fi->vi.pixel_type = AVS_CS_YUV422P12;
    else if( pix_fmt == AV_PIX_FMT_YUV444P12 )
        filter->fi->vi.pixel_type = AVS_CS_YUV444P12;
    else if( pix_fmt == AV_PIX_FMT_YUV420P14 )
        filter->fi->vi.pixel_type = AVS_CS_YUV420P14;
    else if( pix_fmt == AV_PIX_FMT_YUV422P14 )
        filter->fi->vi.pixel_type = AVS_CS_YUV422P14;
    else if( pix_fmt == AV_PIX_FMT_YUV444P14 )
        filter->fi->vi.pixel_type = AVS_CS_YUV444P14;
    else if( pix_fmt == AV_PIX_FMT_YUV420P16 )
        filter->fi->vi.pixel_type = AVS_CS_YUV420P16;
    else if( pix_fmt == AV_PIX_FMT_YUV422P16 )
        filter->fi->vi.pixel_type = AVS_CS_YUV422P16;
    else if( pix_fmt == AV_PIX_FMT_YUV444P16 )
        filter->fi->vi.pixel_type = AVS_CS_YUV444P16;
    else if( pix_fmt == AV_PIX_FMT_YUVA420P )
        filter->fi->vi.pixel_type = AVS_CS_YUVA420;
    else if( pix_fmt == AV_PIX_FMT_YUVA422P )
        filter->fi->vi.pixel_type = AVS_CS_YUVA422;
    else if( pix_fmt == AV_PIX_FMT_YUVA444P )
        filter->fi->vi.pixel_type = AVS_CS_YUVA444;
    else if( pix_fmt == AV_PIX_FMT_YUVA420P10 )
        filter->fi->vi.pixel_type = AVS_CS_YUVA420P10;
    else if( pix_fmt == AV_PIX_FMT_YUVA422P10 )
        filter->fi->vi.pixel_type = AVS_CS_YUVA422P10;
    else if( pix_fmt == AV_PIX_FMT_YUVA444P10 )
        filter->fi->vi.pixel_type = AVS_CS_YUVA444P10;
//    else if( pix_fmt == AV_PIX_FMT_YUVA420P12 )
//        filter->fi->vi.pixel_type = AVS_CS_YUVA420P12;
//    else if( pix_fmt == AV_PIX_FMT_YUVA422P12 )
//        filter->fi->vi.pixel_type = AVS_CS_YUVA422P12;
//    else if( pix_fmt == AV_PIX_FMT_YUVA444P12 )
//        filter->fi->vi.pixel_type = AVS_CS_YUVA444P12;
//    else if( pix_fmt == AV_PIX_FMT_YUVA420P14 )
//        filter->fi->vi.pixel_type = AVS_CS_YUVA420P14;
//    else if( pix_fmt == AV_PIX_FMT_YUVA422P14 )
//        filter->fi->vi.pixel_type = AVS_CS_YUVA422P14;
//    else if( pix_fmt == AV_PIX_FMT_YUVA444P14 )
//        filter->fi->vi.pixel_type = AVS_CS_YUVA444P14;
    else if( pix_fmt == AV_PIX_FMT_YUVA420P16 )
        filter->fi->vi.pixel_type = AVS_CS_YUVA420P16;
    else if( pix_fmt == AV_PIX_FMT_YUVA422P16 )
        filter->fi->vi.pixel_type = AVS_CS_YUVA422P16;
    else if( pix_fmt == AV_PIX_FMT_YUVA444P16 )
        filter->fi->vi.pixel_type = AVS_CS_YUVA444P16;
    else if( pix_fmt == AV_PIX_FMT_GBRP )
        filter->fi->vi.pixel_type = AVS_CS_RGBP;
    else if( pix_fmt == AV_PIX_FMT_GBRP10 )
        filter->fi->vi.pixel_type = AVS_CS_RGBP10;
    else if( pix_fmt == AV_PIX_FMT_GBRP12 )
        filter->fi->vi.pixel_type = AVS_CS_RGBP12;
    else if( pix_fmt == AV_PIX_FMT_GBRP14 )
        filter->fi->vi.pixel_type = AVS_CS_RGBP14;
    else if( pix_fmt == AV_PIX_FMT_GBRP16 )
        filter->fi->vi.pixel_type = AVS_CS_RGBP16;
    else if( pix_fmt == AV_PIX_FMT_GBRAP )
        filter->fi->vi.pixel_type = AVS_CS_RGBAP;
    else if( pix_fmt == AV_PIX_FMT_GBRAP10 )
        filter->fi->vi.pixel_type = AVS_CS_RGBAP10;
    else if( pix_fmt == AV_PIX_FMT_GBRAP12 )
        filter->fi->vi.pixel_type = AVS_CS_RGBAP12;
//    else if( pix_fmt == AV_PIX_FMT_GBRAP14 )
//        filter->fi->vi.pixel_type = AVS_CS_RGBAP14;
    else if( pix_fmt == AV_PIX_FMT_GBRAP16 )
        filter->fi->vi.pixel_type = AVS_CS_RGBAP16;
    else
        return avs_new_value_error( "FFVideoSource: No suitable output format found" );

    if( filter->rff_mode > 0 && dst_height != frame->EncodedHeight )
        return avs_new_value_error( "FFVideoSource: Vertical scaling not allowed in RFF mode" );

    if( filter->rff_mode > 0 && dst_pix_fmt != AV_PIX_FMT_NV21 )
        return avs_new_value_error( "FFVideoSource: Only the default output colorspace can be used in RFF mode" );

    // Set color information
    char buf[512] = {0};
    ffms_avs_sprintf2( buf, sizeof(buf), "%sFFCOLOR_SPACE", var_prefix );
    ffms_avs_lib.avs_set_var( filter->fi->env, buf, avs_new_value_int( frame->ColorSpace ) );
    ffms_avs_sprintf2( buf, sizeof(buf), "%sFFCOLOR_RANGE", var_prefix );
    ffms_avs_lib.avs_set_var( filter->fi->env, buf, avs_new_value_int( frame->ColorRange ) );

    if( vidp->TopFieldFirst )
        filter->fi->vi.image_type = AVS_IT_TFF;
    else
        filter->fi->vi.image_type = AVS_IT_BFF;

    filter->fi->vi.width  = frame->ScaledWidth;
    filter->fi->vi.height = frame->ScaledHeight;

    int pixel_type = filter->fi->vi.pixel_type;

    // Crop to obey avisynth's width/height requirements
    if( pixel_type == AVS_CS_I420 )
    {
        filter->fi->vi.height &= ~1;
        filter->fi->vi.width  &= ~1;
    }
    else if( pixel_type == AVS_CS_YUY2 || pixel_type == AVS_CS_YV16 )
        filter->fi->vi.width &= ~1;
    else if( pixel_type == AVS_CS_YV411 )
        filter->fi->vi.width &= ~3;

    if( filter->rff_mode > 0 )
        filter->fi->vi.height &= ~1;
    return avs_new_value_int( 0 );
}

AVS_Value FFVideoSource_create( AVS_ScriptEnvironment *env, const char *src, int track,
    FFMS_Index *index, int fps_num, int fps_den, int threads, int seek_mode,
    int rff_mode, int width, int height, const char *resizer_name, const char *csp_name,
    const char *var_prefix )
{
    ffvideosource_filter_t *filter = calloc( 1, sizeof(ffvideosource_filter_t) );
    if( !filter )
        return avs_void;

    filter->fps_num = fps_num;
    filter->fps_den = fps_den;
    filter->rff_mode = rff_mode;

    AVS_Clip *clip = ffms_avs_lib.avs_new_c_filter( env, &filter->fi, avs_void, 0 );
    if( !clip )
    {
        filter = NULL;
        free( filter );
        return avs_void;
    }
    memset( &filter->fi->vi, 0, sizeof(AVS_VideoInfo) );

    init_ErrorInfo( ei );

    filter->vid = FFMS_CreateVideoSource( src, track, index, threads, seek_mode, &ei );
    if( !filter->vid )
        return avs_new_value_error( ffms_avs_sprintf( "FFVideoSource: %s", ei.Buffer ) );

    AVS_Value result = init_output_format( filter, width, height, resizer_name, csp_name,
        var_prefix );
    if( avs_is_error( result ) )
    {
        FFMS_DestroyVideoSource( filter->vid );
        filter = NULL;
        free( filter );
        return result;
    }

    const FFMS_VideoProperties *vidp = FFMS_GetVideoProperties( filter->vid );

    if( rff_mode > 0 )
    {
        // This part assumes things, and so should you

        FFMS_Track *vtrack = FFMS_GetTrackFromVideo( filter->vid );

        if( FFMS_GetFrameInfo( vtrack, 0 )->RepeatPict < 0 )
        {
            FFMS_DestroyVideoSource( filter->vid );
            return avs_new_value_error( "FFVideoSource: No RFF flags present" );
        }

        int repeat_min = FFMS_GetFrameInfo( vtrack, 0 )->RepeatPict;
        int num_fields = 0;

        for( int i = 0; i < vidp->NumFrames; i++ )
        {
            int repeat_pict = FFMS_GetFrameInfo( vtrack, i )->RepeatPict;
            num_fields += repeat_pict + 1;
            repeat_min = FFMIN( repeat_min, repeat_pict );
        }

        for( int i = 0; i < vidp->NumFrames; i++ )
        {
            int repeat_pict = FFMS_GetFrameInfo( vtrack, i )->RepeatPict;

            if( ((repeat_pict + 1) * 2) % (repeat_min + 1) )
            {
                FFMS_DestroyVideoSource( filter->vid );
                return avs_new_value_error( "FFVideoSource: Unsupported RFF flag pattern" );
            }
        }

        filter->fi->vi.fps_denominator = vidp->RFFDenominator * (repeat_min + 1);
        filter->fi->vi.fps_numerator = vidp->RFFNumerator;
        filter->fi->vi.num_frames = (num_fields + repeat_min) / (repeat_min + 1);

        int dest_field = 0;
        filter->field_list = malloc( filter->fi->vi.num_frames * sizeof(frame_fields_t) );
        if( !filter->field_list )
            return avs_new_value_error( "FFVideoSource: Memory allocation failure" );

        for( int i = 0; i < vidp->NumFrames; i++ )
        {
            int repeat_pict = FFMS_GetFrameInfo( vtrack, i )->RepeatPict;
            int repeat_fields = ((repeat_pict + 1) * 2) / (repeat_min + 1);

            for( int j = 0; j < repeat_fields; j++ )
            {
                if( (dest_field + (vidp->TopFieldFirst ? 0 : 1)) & 1 )
                    filter->field_list[dest_field / 2].top = i;
                else
                    filter->field_list[dest_field / 2].bottom = i;
                dest_field++;
            }
        }

        if( rff_mode == 2 )
        {
            int field_list_size = filter->fi->vi.num_frames;
            filter->fi->vi.num_frames = filter->fi->vi.num_frames * 4 / 5;
            filter->fi->vi.fps_denominator *= 5;
            filter->fi->vi.fps_numerator *= 4;

            int output_frames = 0;

            for( int i = 0; i < filter->fi->vi.num_frames / 4; i++ )
            {
                char has_dropped = 0;

                filter->field_list[output_frames].top = filter->field_list[i * 5].top;
                filter->field_list[output_frames].bottom = filter->field_list[i * 5].top;
                output_frames++;

                for( int j = 1; j < 5; j++ )
                {
                    if( !has_dropped && filter->field_list[i * 5 + j - 1].top == filter->field_list[i * 5 + j].top )
                    {
                        has_dropped = 1;
                        continue;
                    }

                    filter->field_list[output_frames].top = filter->field_list[i * 5 + j].top;
                    filter->field_list[output_frames].bottom = filter->field_list[i * 5 + j].top;
                    output_frames++;
                }

                if( !has_dropped )
                    output_frames--;
            }

            if( output_frames > 0 )
                for( int i = output_frames - 1; i < field_list_size; i++ )
                {
                     filter->field_list[i].top = filter->field_list[output_frames - 1].top;
                     filter->field_list[i].bottom = filter->field_list[output_frames - 1].top;
                }

            filter->field_list = realloc( filter->field_list, filter->fi->vi.num_frames * sizeof(frame_fields_t) );
            if( !filter->field_list )
                return avs_new_value_error( "FFVideoSource: Memory allocation failure" );
        }
    }
    else
    {
        if( fps_num > 0 && fps_den > 0 )
        {
            filter->fi->vi.fps_denominator = fps_den;
            filter->fi->vi.fps_numerator = fps_num;
            if( vidp->NumFrames > 1 )
            {
                filter->fi->vi.num_frames = (vidp->LastTime - vidp->FirstTime) * (1 + 1. / (vidp->NumFrames - 1)) * fps_num / fps_den + 0.5;
                if( filter->fi->vi.num_frames < 1 )
                    filter->fi->vi.num_frames = 1;
            }
            else
                filter->fi->vi.num_frames = 1;
        }
        else
        {
            filter->fi->vi.fps_denominator = vidp->FPSDenominator;
            filter->fi->vi.fps_numerator = vidp->FPSNumerator;
            filter->fi->vi.num_frames = vidp->NumFrames;
        }
    }

    // Set AR variables
    char buf[512] = {0};
    ffms_avs_sprintf2( buf, sizeof(buf), "%sFFSAR_NUM", var_prefix );
    ffms_avs_lib.avs_set_var( env, buf, avs_new_value_int( vidp->SARNum ) );
    ffms_avs_sprintf2( buf, sizeof(buf), "%sFFSAR_DEN", var_prefix );
    ffms_avs_lib.avs_set_var( env, buf, avs_new_value_int( vidp->SARDen ) );
    if( vidp->SARNum > 0 && vidp->SARDen > 0 )
    {
        ffms_avs_sprintf2( buf, sizeof(buf), "%sFFSAR", var_prefix );
        ffms_avs_lib.avs_set_var( env, buf, avs_new_value_float( vidp->SARNum / (double)vidp->SARDen ) );
    }

    // Set crop variables
    ffms_avs_sprintf2( buf, sizeof(buf), "%sFFCROP_LEFT", var_prefix );
    ffms_avs_lib.avs_set_var( env, buf, avs_new_value_int( vidp->CropLeft ) );
    ffms_avs_sprintf2( buf, sizeof(buf), "%sFFCROP_RIGHT", var_prefix );
    ffms_avs_lib.avs_set_var( env, buf,  avs_new_value_int( vidp->CropRight ) );
    ffms_avs_sprintf2( buf, sizeof(buf), "%sFFCROP_TOP", var_prefix );
    ffms_avs_lib.avs_set_var( env, buf, avs_new_value_int( vidp->CropTop ) );
    ffms_avs_sprintf2( buf, sizeof(buf), "%sFFCROP_BOTTOM", var_prefix );
    ffms_avs_lib.avs_set_var( env, buf, avs_new_value_int( vidp->CropBottom ) );

    ffms_avs_lib.avs_set_global_var( env, "FFVAR_PREFIX", avs_new_value_string( var_prefix ) );

    filter->var_name_vfr_time = ffms_avs_sprintf( "%sFFVFR_TIME", var_prefix );
    filter->var_name_pict_type = ffms_avs_sprintf( "%sFFPICT_TYPE", var_prefix );

    filter->fi->free_filter     = free_filter;
    filter->fi->get_frame       = get_frame;
    filter->fi->set_cache_hints = set_cache_hints;
    filter->fi->get_audio       = get_audio;
    filter->fi->get_parity      = get_parity;
    filter->fi->user_data       = filter;
    return clip_val( clip );
}
