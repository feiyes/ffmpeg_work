CC			= gcc
BUILD_DIR 	= ./build
OBJ_DIR		= $(BUILD_DIR)/obj/
BIN_DIR		= $(BUILD_DIR)/bin/
SRC_DIR		= ./
SRCS		= $(wildcard *.c */*.c)
OBJS		= $(patsubst %.c, %.o, $(SRCS))
OBJS	   := $(addprefix $(OBJ_DIR), $(OBJS))
CFLAGS		= -g -std=c99 -I../../../include/
#LDFLAGS   += -lavutil -lavformat -lavcodec -lavutil -lswscale -lswresample -lavfilter -lSDL2
NAME		= $(wildcard *.c)
TARGET		= transcode

FFMPEG_LIBS="libavdevice libavformat libavfilter libavcodec libswscale libavutil libswresample "
CFLAGS+=$(shell pkg-config --cflags $(FFMPEG_LIBS))

ifneq ($(BUILD_DIR),)
# Attempt to create a output directory.
$(shell [ -d ${BIN_DIR} ] || mkdir -p ${BIN_DIR})
$(shell [ -d ${OBJ_DIR} ] || mkdir -p ${OBJ_DIR})
endif

all: $(BIN_DIR)$(TARGET)

$(BIN_DIR)$(TARGET):	$(OBJS)
	@$(CC) $^ $(LDFLAGS) -o $@
	@echo Generating $(BIN_DIR)$(TARGET) done.

$(OBJ_DIR)%.o:	$(SRC_DIR)%.c
	@echo Compiling $< ......
	@$(CC) -o $@ -c $< $(CFLAGS)

clean:
	rm -rf $(BUILD_DIR)
