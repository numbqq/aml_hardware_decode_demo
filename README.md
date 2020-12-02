# Amlogic Video Hardware Deocder Demo

Decode and save decoded frames to file.

Usage:
```
ionplayer <filename> <width> <height> <fps> <format(1:mpeg4 2:h264)> [subformat for mpeg4]
```

Example:

H264 -> NV12:

```
# ./ionplayer bbb.264 1920 1080 60 2
```

Decoded file stored in `/tmp/yuv.yuv`.
