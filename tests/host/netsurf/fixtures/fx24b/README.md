# FX-24B authored raster fixtures

These files depict synthetic 1600x1200 quadrants, authored for CiukiOS tests.
They contain no third-party artwork. Test fixture license: MIT.

PNG and lossless WebP: opaque red, green and blue quadrants, with a transparent
yellow bottom-right quadrant. `adam7.png` contains the same pixels in raw
Adam7 passes. `lossy.webp` is the opaque RGB equivalent (quality 95).
`large.gif` has opaque red/green/blue/yellow quadrants and is interlaced.

`animated.gif` starts with those opaque quadrants, then overlays transparent
partial frames at source coordinates (17,31)-(1301,901),
(901,601)-(1501,1101), and (301,201)-(501,401). Their disposal methods are
restore previous, restore background, and retain. Delays are 100 ms. These deliberately off-grid rectangles exercise
sample-lattice rounding, transparency and both restore paths.

The small `.samples` files contain independently decoded straight RGBA pixels
at source coordinates x=40,140,...,1540 and y=40,140,...,1140, in row order,
then frame order. GIF frames append 18 samples immediately inside/outside the
partial-frame boundaries (coordinates listed in `large_image_test.c`), covering
rounding and disposal at both tested sample factors. They were generated with Pillow 13.0.0; PNG interlacing
was generated with ImageMagick. Runtime tests need neither tool. Decoder
tests allow eight levels of RGB error for WebP resampling and loss. Transparent
pixels compare alpha, since hidden RGB values need not survive GIF encoding.
