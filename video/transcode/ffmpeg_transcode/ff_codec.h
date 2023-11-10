#ifndef __AV_CODEC_H__
#define __AV_CODEC_H__

#include "ff_transcode.h"

int ff_flush_encoder(unsigned int stream_index, TranscodeContext *transcode_context);
int ff_encode_write_frame(unsigned int stream_index, int flush, StreamContext *stream_ctx, FilteringContext *filter_ctx, AVFormatContext *ofmt_ctx);

#endif
