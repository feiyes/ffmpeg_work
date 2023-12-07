#ifndef __LOG_H__
#define __LOG_H__

#include <stdio.h>
#include <libgen.h>

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* end of __cplusplus */

#define log_dbg(fmt,...) do { \
    fprintf(stderr,"\033[0;34m[DEBUG][%s(%d)] "fmt"\033[0m\n", basename(__FILE__), __LINE__, ##__VA_ARGS__); \
} while(0)

#define log_info(fmt,...) do { \
    fprintf(stderr,"\033[0;32m[INFO][%s(%d)] "fmt"\033[0m\n", basename(__FILE__), __LINE__, ##__VA_ARGS__); \
} while(0)

#define log_warn(fmt,...) do { \
    fprintf(stderr,"\033[0;33m[WARN][%s(%d)] "fmt"\033[0;39m\n", basename(__FILE__), __LINE__, ##__VA_ARGS__); \
} while(0)

#define log_err(fmt, ...) do { \
    fprintf(stderr,"\033[0;31m[ERROR][%s(%d)] "fmt"\033[0m\n", basename(__FILE__), __LINE__, ##__VA_ARGS__); \
} while(0)

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* end of __cplusplus */

#endif //__LOG_H__
