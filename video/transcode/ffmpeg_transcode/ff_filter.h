#ifndef __AV_FILTER_H__
#define __AV_FILTER_H__

#include "ff_transcode.h"

int ff_init_filters(TranscodeContext *transcode_context);

int ff_filter_encode_write_frame(AVFrame *frame, unsigned int stream_index, TranscodeContext *transcode_context);

#endif
