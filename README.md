## Videoplayer for ESP32

This is a proof of concept videoplayer which plays raw RGB565 and MJPG video files from the SD card. You can convert any video file to to both formats with ffmpeg. Currently videos are played without sound.

![Big Buck Bunny on TTGO T4](https://appelsiini.net/img/2020/bbb-cover-1.jpg)

You can also see how it works in [Vimeo](https://vimeo.com/409435420).

### Configure and compile

```
$ git clone git@github.com:tuupola/esp_video.git --recursive
$ cd esp_video
$ cp sdkconfig.ttgo-t4-v13 sdkconfig
$ make -j8 flash
```

If you have some other board or display run menuconfig yourself.

```
$ git clone git@github.com:tuupola/esp_video.git --recursive
$ cd esp_video
$ make menuconfig
$ make -j8 flash
```

### Download and convert video

I used [Big Buck Bunny](https://peach.blender.org/download/) in the examples. Converted video should be copied to a FAT formatted SD card. Note that by default ESP-IDF does not support long filenames. Either enable them from `menuconfig` or use short 8.3 filenames.

```
$ wget https://download.blender.org/peach/bigbuckbunny_movies/BigBuckBunny_320x180.mp4 -O bbb.mp4
$ ffmpeg -i BigBuckBunny_320x180.mp4 -f rawvideo -pix_fmt rgb565be -vcodec rawvideo bbb24.raw
```

The original video is 24 fps. With SPI interface the SD card reading speed seems to be the bottleneck. You can create 12 fps raw video version with the following.

```
$ ffmpeg -i BigBuckBunny_320x180.mp4 -f rawvideo -pix_fmt rgb565be -vcodec  rawvideo -r 12 bbb12.raw
```
The above example assumes that the source video's size is such that it wiil fit within your devices' dimensions,e.g. 320x240. If it isn't, add the parameters '-vf scale=320:-1' which will resize the output video width to 320 and maintain the aspect ratio.

With motion jpeg video ESP32 itself is the bottleneck. With my testing I
was able to decode at approximately 8 fps. You can create a 8 fps MJPG video with the following.

```
$ ffmpeg -i BigBuckBunny_320x180.mp4 -r 8 -an -f mjpeg -q:v 1 -pix_fmt yuvj420p -vcodec mjpeg -force_duplicated_matrix 1 -huffman 0 bbb08.mjp
```

There is a reasonable size difference between raw and motion jpeg files.

```
$ du -h bbb12.raw
798M	bbb12.raw

$ du -h bbb12.mjp
117M	bbb12.mjp

$ du -h bbb08.mjp
 80M	bbb08.mjp
```

Clicking on the middle button displays a list of files. The currently selected file is highlighed in magenta. the other two (eft and right) buttons change the selected file by moving the selection up or down. Clicking on the center button while the list is displayed displays that file.

## Big Buck Bunny

Copyright (C) 2008 Blender Foundation | peach.blender.org<br>
Some Rights Reserved. Creative Commons Attribution 3.0 license.<br>
http://www.bigbuckbunny.org/

### Display JPG Images
The program can also display JPG images. However, there are no provisions to resize the image to fit the screen so only a portion will be displayed. Also, the jpg decoding logic cannot handle files that utilize an interlace scheme. The solution to this is to use the [ImageMagick](https://imagemagick.org/index.php) utility to convert them:

```
convert image.jpg -interlace none -resize 320x240 new_image.jpg
```

In this case, the image will be resized to fit within a 320x240 rectangle with no interlace. Note that some clipping may occur if the resulting image's height is greater than 210 (240 less the height of the info bar).So, t prevent clipping, resize the image to 320x210.

