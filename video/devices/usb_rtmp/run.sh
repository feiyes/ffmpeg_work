#!/bin/sh

cmd="${1}" 
case ${cmd} in 
    uvc)
        sh -c 'sudo ./usb_rtmp uvc flv /dev/video0 rtmp://192.168.174.128:1936/live/test'
        ;;  
    vod)
        #ffmpeg -re -i video/h264/cuc_ieschool.h264 -vcodec copy -f flv -y rtmp://192.168.56.130:1935/live/test
        sh -c './usb_rtmp vod flv /mnt/hgfs/Work/stream/h264/vehicle.h264 rtmp://192.168.174.128:1935/live/test'
        ;; 
    enc)
        sh -c './usb_rtmp enc flv ../../../stream/video/h264/cuc_ieschool.h264 test.flv'
        ;;
   *)  
      exit 1
      ;; 
esac
