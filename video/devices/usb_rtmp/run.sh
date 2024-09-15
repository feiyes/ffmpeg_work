#!/bin/sh

cmd="${1}" 
case ${cmd} in 
    uvc)
        sh -c 'sudo ./usb_rtmp uvc flv libx264 /dev/video0 rtmp://192.168.174.128:1936/live/test'
        ;;  
    vod)
        #ffmpeg -re -i video/h264/cuc_ieschool.h264 -vcodec copy -f flv -y rtmp://192.168.174.128:1935/live/test
        #sh -c './usb_rtmp vod flv  libx264 ../../../stream/video/h264/test.264 rtmp://192.168.174.128:1935/live/test'
        sh -c './usb_rtmp vod rtsp libx265 ../../../stream/video/h264/test.264 rtsp://192.168.174.128:8554/test'
        #sh -c './usb_rtmp vod rtsp ../../../stream/audio/WavinFlag.aac rtsp://192.168.174.128:8554/test'
        ;; 
    enc)
        #sh -c './usb_rtmp enc hevc   libx265 ../../../stream/video/h264/cuc_ieschool.h264 test.h265'
        #sh -c './usb_rtmp enc h264   libx264 ../../../stream/video/h264/cuc_ieschool.h264 test.h264'
        #sh -c './usb_rtmp enc flv    libx264 ../../../stream/video/h264/cuc_ieschool.h264 test.flv'
        #sh -c './usb_rtmp enc mp4    libx264 ../../../stream/video/h264/cuc_ieschool.h264 test.mp4'
        sh -c './usb_rtmp enc mpegts libx264 ../../../stream/video/h264/cuc_ieschool.h264 test.ts'
        ;;
   *)  
       echo "`basename ${0}`:usage: [uvc] | [vod] | [enc]"
      exit 1
      ;; 
esac
