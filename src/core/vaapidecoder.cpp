#include <stdio.h>
#include <string.h>
extern "C" {
#include <libavutil/hwcontext.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#define DEFAULT_SURFACES 20
static const char *dev_name = "/dev/dri/renderD128";
static AVBufferRef *vaapi_device_ctx = NULL;

void vaapi_decoder_uninit(AVCodecContext *ctx)
{
    if (vaapi_device_ctx) {
        av_buffer_unref(&vaapi_device_ctx);
    }
    return;
}

int vaapi_decoder_init(AVCodecContext *ctx)
{
    int err = 0;

    if ((err = av_hwdevice_ctx_create(&vaapi_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                      dev_name, NULL, 0)) < 0) {
        fprintf(stderr, "Failed to create specified HW device.\n");
        return err;
    }
    ctx->hw_device_ctx = av_buffer_ref(vaapi_device_ctx);

    return err;
}

enum AVPixelFormat get_vaapi_format(AVCodecContext *ctx,
                                           const enum AVPixelFormat *pix_fmts)
{
    const enum AVPixelFormat *p;

    for (p = pix_fmts; *p != AV_PIX_FMT_NONE; p++) {
        if (*p == AV_PIX_FMT_VAAPI)
            return *p;
    }

    fprintf(stderr, "Unable to decode this file using VA-API.\n");
    return AV_PIX_FMT_NONE;
}

int retrieve_data(AVFrame *dst, AVFrame *src)
{
    int err;

    if ((err = av_hwframe_transfer_data(dst, src, 0)) < 0) {
        fprintf(stderr, "Failed to transfer data to output frame: %d.\n", err);
        return -1;
    }

    return 0;
}
