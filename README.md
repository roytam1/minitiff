# MiniTIFF

MiniTIFF is a small, human-readable TIFF decoder written in **C89**.

It is designed in the spirit of `stb_image`: compact, easy to read, and easy to embed without requiring a large TIFF library.

## Features

- Classic TIFF, little-endian (`II`) and big-endian (`MM`)
- Single-page and multi-page TIFF
- Page-count API
- Strip-based and tiled TIFF
- Contiguous and separate planar configurations
- 1-, 2-, 4-, 8-, and 16-bit unsigned samples
- Grayscale, palette, RGB, RGBA, and CMYK
- TIFF Orientation handling
- Horizontal Predictor (`Predictor = 2`)
- Uncompressed, LZW, CCITT Group 3/4, and PackBits
- Optional JPEG through `stb_image`
- Optional Deflate / Adobe Deflate through `stb_image`'s internal zlib decoder
- JPEG-in-TIFF `JPEGTables` support
- Basic generic TIFF tag access
- No mandatory third-party dependencies
- C89-compatible implementation

Decoded images are returned as **8-bit RGBA**.

## Basic API

```c
MiniTIFF_Image *image;

image = tiff_load_file("image.tif", 0);

if (image) {
    printf("%lu x %lu\n",
           image->width,
           image->height);

    /* image->pixels contains RGBA8 pixels */

    tiff_free(image);
}
```

The main API is:

```c
MiniTIFF_Image *tiff_load(
    const void *data,
    size_t size,
    unsigned page);

MiniTIFF_Image *tiff_load_file(
    const char *filename,
    unsigned page);

void tiff_free(MiniTIFF_Image *image);
```

## Page count

For multi-page TIFFs:

```c
int tiff_get_page_count(
    const void *data,
    size_t size);

int tiff_get_page_count_file(
    const char *filename);
```

Example:

```c
int pages;
int i;

pages = tiff_get_page_count_file("document.tif");

for (i = 0; i < pages; ++i) {
    MiniTIFF_Image *image;

    image = tiff_load_file("document.tif", (unsigned)i);

    if (image) {
        /* process page */
        tiff_free(image);
    }
}
```

The count is the number of top-level IFDs. For ordinary multi-page TIFF documents, this corresponds to the number of pages.

## Validation

```c
int tiff_is_valid(
    const void *data,
    size_t size);

int tiff_is_valid_file(
    const char *filename);
```

## Metadata

MiniTIFF provides simple access to TIFF tags.

For example, `ImageWidth` (tag 256):

```c
unsigned long width;

if (tiff_get_tag_u32(data, size, 0, 256, 0, &width))
    printf("width = %lu\n", width);
```

Read a string tag:

```c
char description[256];

if (tiff_get_tag_string(
        data, size, 0, 270,
        description, sizeof(description))) {
    printf("%s\n", description);
}
```

Raw tag data:

```c
unsigned char buffer[1024];
size_t required;

if (tiff_get_tag_data(
        data, size, 0, 347,
        buffer, sizeof(buffer), &required)) {
    /* raw tag data */
}
```

## Optional stb_image support

Define `MINITIFF_USE_STB_IMAGE` to use `stb_image.h` for JPEG decoding.

```c
#define STB_IMAGE_IMPLEMENTATION
#define MINITIFF_USE_STB_IMAGE

#include "stb_image.h"
#include "minitiff_v4_c89.c"
```

MiniTIFF supports JPEG-in-TIFF files using TIFF compression 6/7, including files whose JPEG tables are stored separately in `JPEGTables` (tag 347).

## Optional Deflate support

Define:

```c
#define MINITIFF_USE_STB_IMAGE
#define MINITIFF_USE_STB_ZLIB
```

MiniTIFF can then reuse stb_image's internal zlib decoder for TIFF Deflate (8) and Adobe Deflate (32946).

`MINITIFF_USE_STB_ZLIB` requires `MINITIFF_USE_STB_IMAGE`.

This avoids a mandatory external zlib dependency. The trade-off is that stb_image's zlib decoder is an internal, non-public API, so the bridge may need adjustment if stb_image changes its internals.

## CMYK

CMYK TIFFs (`PhotometricInterpretation = 5`) use a simple dependency-free multiplicative conversion to RGB.

For 8-bit samples:

```c
r = ((255 - c) * (255 - k)) / 255;
g = ((255 - m) * (255 - k)) / 255;
b = ((255 - y) * (255 - k)) / 255;
```

The 16-bit path uses the equivalent scaled calculation.

This is **not ICC color management**. Without an ICC profile, CMYK values do not uniquely specify a display RGB color, so different applications can produce different results.

## Build

MiniTIFF is C89-compatible:

```sh
cc -std=c89 -pedantic -Wall -Wextra minitiff_v4_c89.c
```

For the optional test program, if enabled by the source:

```sh
cc -std=c89 -pedantic -Wall -Wextra \
    -DMINITIFF_TEST \
    minitiff_v4_c89.c -o minitiff_test
```

## Limitations

MiniTIFF is intended to be a lightweight decoder, not a replacement for libtiff.

Not currently implemented as full features:

- BigTIFF
- SubIFDs / image pyramids
- ICC color management
- Full YCbCr conversion and subsampling
- Floating-point sample output
- Signed integer sample output
- TIFF encoding/writing
- Full metadata editing

## Design goals

1. C89 compatibility
2. No mandatory external dependencies
3. Small, readable source
4. Good compatibility with common real-world TIFF files
5. Optional stb_image integration
6. Easy to modify for embedded and legacy projects

## License

Public Domain. If a license is needed, WTFPL. And if a **proper** license is really needed, MIT.

If you distribute `stb_image.h` with MiniTIFF, retain the license and attribution required by the version of `stb_image.h` you use.

## Status

MiniTIFF is currently a TIFF **decoder**. The implementation and API may evolve as additional real-world TIFF files are tested.
