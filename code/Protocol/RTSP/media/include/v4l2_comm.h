
#ifndef V4L2_COMMON_H
#define V4L2_COMMON_H

extern "C" 
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

struct fmt_map 
{
    enum AVPixelFormat ff_fmt;
    enum AVCodecID codec_id;
    uint32 v4l2_fmt;
};

#ifdef __cplusplus
extern "C" {
#endif

extern const struct fmt_map ff_fmt_conversion_table[];

uint32 ff_fmt_ff2v4l(enum AVPixelFormat pix_fmt, enum AVCodecID codec_id);
enum AVPixelFormat ff_fmt_v4l2ff(uint32 v4l2_fmt, enum AVCodecID codec_id);
enum AVCodecID ff_fmt_v4l2codec(uint32 v4l2_fmt);

#ifdef __cplusplus
}
#endif

#endif /* V4L2_COMMON_H */

