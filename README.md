# swaybg-lbm

A fork of [swaybg](https://github.com/swaywm/swaybg) with support for color-cycling LBM images.

Color-cycling refers to an animation technique in which the image pixels are static, but certain
ranges of the color palette can shift and vary from one frame to the next.

This technique was widely used in old PC games to make the most of the limited 256-color palette.
Recently, however, you're likely to have seen this technique on [Joseph Huckaby's web-based demo](http://www.effectgames.com/demos/canvascycle),
or the [Living Worlds mobile app](http://pixfabrik.com/livingworlds). Both of these feature the beautiful pixel art landscapes created by [Mark Ferrari](https://www.markferrari.com/).

This project allows you to use these scenes as a wallpaper on your Wayland desktop.

The artwork featured in the links above is under copyright, so the image files are not included here.

## Using

To diplay an animated LBM image, swaybg-lbm is invoked exactly the same way as swaybg with a few small differences:
* Aspect ratio of the source image is always preserved, and only integer scaling is supported. Therefore, the "Stretch" mode is not supported.
* "Fill" and "Fit" will scale the image up accordingly, but with a margin of up to 100px. In other words, a lower scale factor is preferred, if the image very nearly fits.

## TODOs
- [ ] GPU rendering
- [ ] Smooth cycling
- [ ] Time-of-day-based palette shifting

Refer to the upstream swaybg documentation for any general information regarding swaybg
