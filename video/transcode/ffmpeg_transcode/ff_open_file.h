#ifndef __OPEN_FILE_H__
#define __OPEN_FILE_H__

#include "ff_transcode.h"

int ff_open_input_file(const char *filename, TranscodeContext *transcode_context);

int ff_open_output_file(const char *filename, TranscodeContext *transcode_context);

 #endif
