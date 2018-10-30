#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>

void vaapi_decoder_uninit(AVCodecContext *ctx);
enum AVPixelFormat get_vaapi_format(AVCodecContext *ctx,
                                       const enum AVPixelFormat *pix_fmts);
int vaapi_decoder_init(AVCodecContext *ctx);
int retrieve_data(AVFrame *dst, AVFrame *src);
