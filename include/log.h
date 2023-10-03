#ifndef __LOG_H__
#define __LOG_H__

#include <stdio.h>
#include <libgen.h>

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* end of __cplusplus */

#define log_info(fmt,...) do { \
    fprintf(stderr,"[INFO][%s(%d)] "fmt"\n", basename(__FILE__), __LINE__, ##__VA_ARGS__); \
} while(0)

#define log_warn(fmt,...) do { \
    fprintf(stderr,"[WARN][%s(%d)] "fmt"\n", basename(__FILE__), __LINE__, ##__VA_ARGS__); \
} while(0)

#define log_err(fmt, ...) do { \
    fprintf(stderr,"[ERROR][%s(%d)] "fmt"\n", basename(__FILE__), __LINE__, ##__VA_ARGS__); \
} while(0)

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* end of __cplusplus */

#endif //__LOG_H__
