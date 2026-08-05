/*
    minitiff.c
    Small dependency-free TIFF decoder.

    C99
    No external dependencies.

    Supported:
      - Classic TIFF (not BigTIFF)
      - II / MM byte order
      - 8-bit samples
      - Grayscale
      - Grayscale + alpha
      - RGB
      - RGBA
      - Palette color
      - Strips
      - Uncompressed
      - PackBits
      - LZW
      - Predictor 2 (horizontal)
      - Multiple IFDs/pages

    Output:
      Always RGBA8.

    Example:

        TIFF_Image *img = tiff_load_file("test.tif", 0);

        if (img) {
            printf("%u x %u\n", img->width, img->height);

            // img->pixels contains width * height * 4 bytes
            // RGBA8.

            tiff_free(img);
        }

    This implementation deliberately does not try to be a complete
    TIFF implementation.
*/
#ifndef _MINITFF_H
#define _MINITFF_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#if defined(_MSC_VER) && _MSC_VER < 1600
typedef   signed char    minitiff_int8;
typedef unsigned char    minitiff_uint8;
typedef unsigned short   minitiff_uint16;
typedef unsigned int     minitiff_uint32;
#else
#include <stdint.h>
typedef int8_t           minitiff_int8;
typedef uint8_t          minitiff_uint8;
typedef uint16_t         minitiff_uint16;
typedef uint32_t         minitiff_uint32;
#endif

#ifndef SIZE_MAX
#define SIZE_MAX ((size_t)(-1))
#endif

/* for test code */
#ifdef TIFF_TEST
#define MINITIFF_IMPLEMENTATION
#endif
/* ------------------------------------------------------------------------- */
/* Public API                                                               */
/* ------------------------------------------------------------------------- */

typedef struct TIFF_Image {
    minitiff_uint32 width;
    minitiff_uint32 height;

    /* Always 4. */
    minitiff_uint32 channels;

    /* Always 8. */
    minitiff_uint32 bits_per_channel;

    /* width * height * 4 bytes. */
    minitiff_uint8 *pixels;
} TIFF_Image;


/*
    Load page_index from TIFF data.

    page_index == 0 means first IFD/page.
*/
TIFF_Image *tiff_load(const void *data, size_t size, unsigned page_index);

/*
    Load a TIFF file from disk.
*/
TIFF_Image *tiff_load_file(const char *filename, unsigned page_index);

/*
    Free image.
*/
void tiff_free(TIFF_Image *image);


#ifdef MINITIFF_IMPLEMENTATION
/* ------------------------------------------------------------------------- */
/* Internal helpers                                                         */
/* ------------------------------------------------------------------------- */

typedef struct TIFF_Context {
    const minitiff_uint8 *data;
    size_t size;

    int little_endian;

    minitiff_uint32 ifd_offset;
} TIFF_Context;


typedef struct TIFF_Entry {
    minitiff_uint16 tag;
    minitiff_uint16 type;
    minitiff_uint32 count;
    minitiff_uint32 value_offset;
} TIFF_Entry;


typedef struct TIFF_Page {
    minitiff_uint32 width;
    minitiff_uint32 height;

    minitiff_uint16 compression;
    minitiff_uint16 photometric;

    minitiff_uint16 samples_per_pixel;

    minitiff_uint16 bits_per_sample[4];
    minitiff_uint16 bits_count;

    minitiff_uint16 planar_config;

    minitiff_uint16 predictor;

    minitiff_uint16 extra_samples[4];
    minitiff_uint16 extra_count;

    minitiff_uint32 *strip_offsets;
    minitiff_uint32 *strip_byte_counts;
    minitiff_uint32 strip_count;

    minitiff_uint16 *color_map;
    minitiff_uint32 color_map_count;
} TIFF_Page;


/* ------------------------------------------------------------------------- */
/* Safe integer helpers                                                      */
/* ------------------------------------------------------------------------- */

static int tiff_mul_size(size_t a, size_t b, size_t *out)
{
    if (b != 0 && a > SIZE_MAX / b)
        return 0;

    *out = a * b;
    return 1;
}


static int tiff_add_size(size_t a, size_t b, size_t *out)
{
    if (a > SIZE_MAX - b)
        return 0;

    *out = a + b;
    return 1;
}


static int tiff_range_ok(const TIFF_Context *t,
                         minitiff_uint32 offset,
                         size_t length)
{
    size_t end;

    if (!tiff_add_size((size_t)offset, length, &end))
        return 0;

    return end <= t->size;
}


/* ------------------------------------------------------------------------- */
/* Endian helpers                                                            */
/* ------------------------------------------------------------------------- */

static minitiff_uint16 tiff_u16(const TIFF_Context *t, const minitiff_uint8 *p)
{
    if (t->little_endian) {
        return (minitiff_uint16)p[0] |
               ((minitiff_uint16)p[1] << 8);
    }

    return ((minitiff_uint16)p[0] << 8) |
           (minitiff_uint16)p[1];
}


static minitiff_uint32 tiff_u32(const TIFF_Context *t, const minitiff_uint8 *p)
{
    if (t->little_endian) {
        return (minitiff_uint32)p[0] |
               ((minitiff_uint32)p[1] << 8) |
               ((minitiff_uint32)p[2] << 16) |
               ((minitiff_uint32)p[3] << 24);
    }

    return ((minitiff_uint32)p[0] << 24) |
           ((minitiff_uint32)p[1] << 16) |
           ((minitiff_uint32)p[2] << 8) |
           (minitiff_uint32)p[3];
}


/* ------------------------------------------------------------------------- */
/* TIFF type sizes                                                           */
/* ------------------------------------------------------------------------- */

static size_t tiff_type_size(minitiff_uint16 type)
{
    switch (type) {
    case 1:  /* BYTE */
    case 2:  /* ASCII */
    case 6:  /* SBYTE */
    case 7:  /* UNDEFINED */
        return 1;

    case 3:  /* SHORT */
    case 8:  /* SSHORT */
        return 2;

    case 4:  /* LONG */
    case 9:  /* SLONG */
    case 11: /* FLOAT */
        return 4;

    case 5:  /* RATIONAL */
    case 10: /* SRATIONAL */
    case 12: /* DOUBLE */
        return 8;

    default:
        return 0;
    }
}


/* ------------------------------------------------------------------------- */
/* Entry data access                                                         */
/* ------------------------------------------------------------------------- */

/*
    Return pointer to the actual value bytes of an IFD entry.
*/
static const minitiff_uint8 *tiff_entry_data(const TIFF_Context *t,
                                      const TIFF_Entry *e,
                                      size_t *total_size)
{
    size_t type_size;
    size_t size;

    type_size = tiff_type_size(e->type);
    if (!type_size)
        return NULL;

    if (!tiff_mul_size(type_size, e->count, &size))
        return NULL;

    *total_size = size;

    if (size <= 4) {
        /*
            The value is stored directly in the 4-byte value field.
        */
        if (!tiff_range_ok(t, e->value_offset, 4))
            return NULL;

        /*
            value_offset points to the field itself in this case.
            Caller needs the address of that field, not a newly
            interpreted integer.
        */
        return NULL;
    }

    if (!tiff_range_ok(t, e->value_offset, size))
        return NULL;

    return t->data + e->value_offset;
}


/*
    Get an integer value from an entry.

    Works for BYTE, SHORT and LONG.
*/
static int tiff_entry_get_u32(const TIFF_Context *t,
                              const TIFF_Entry *e,
                              minitiff_uint32 index,
                              minitiff_uint32 *result)
{
    size_t type_size;
    size_t total;
    const minitiff_uint8 *p;

    if (index >= e->count)
        return 0;

    type_size = tiff_type_size(e->type);
    if (!type_size)
        return 0;

    if (!tiff_mul_size(type_size, e->count, &total))
        return 0;

    if (total <= 4) {
        /*
            Locate the four bytes containing the inline value.
            The value field begins at the entry's value_offset member.
        */
        p = (const minitiff_uint8 *)&e->value_offset;

        /*
            e->value_offset itself has already been decoded using the
            file's endian order, so we cannot use its memory representation
            directly for big-endian files.

            Construct the original 4-byte field instead.
        */
        {
            minitiff_uint8 raw[4];

            if (t->little_endian) {
                raw[0] = (minitiff_uint8)(e->value_offset);
                raw[1] = (minitiff_uint8)(e->value_offset >> 8);
                raw[2] = (minitiff_uint8)(e->value_offset >> 16);
                raw[3] = (minitiff_uint8)(e->value_offset >> 24);
            } else {
                raw[0] = (minitiff_uint8)(e->value_offset >> 24);
                raw[1] = (minitiff_uint8)(e->value_offset >> 16);
                raw[2] = (minitiff_uint8)(e->value_offset >> 8);
                raw[3] = (minitiff_uint8)e->value_offset;
            }

            p = raw;

            switch (e->type) {
            case 1:
                *result = p[index];
                return 1;

            case 3:
                *result = tiff_u16(t, p + index * 2);
                return 1;

            case 4:
                *result = tiff_u32(t, p + index * 4);
                return 1;

            default:
                return 0;
            }
        }
    }

    if (!tiff_range_ok(t, e->value_offset, total))
        return 0;

    p = t->data + e->value_offset;

    switch (e->type) {
    case 1:
        *result = p[index];
        return 1;

    case 3:
        *result = tiff_u16(t, p + index * 2);
        return 1;

    case 4:
        *result = tiff_u32(t, p + index * 4);
        return 1;

    default:
        return 0;
    }
}


/* ------------------------------------------------------------------------- */
/* IFD handling                                                              */
/* ------------------------------------------------------------------------- */

static int tiff_read_entry(const TIFF_Context *t,
                           const minitiff_uint8 *p,
                           TIFF_Entry *e)
{
    e->tag          = tiff_u16(t, p + 0);
    e->type         = tiff_u16(t, p + 2);
    e->count        = tiff_u32(t, p + 4);
    e->value_offset = tiff_u32(t, p + 8);

    return 1;
}


static int tiff_find_ifd(const TIFF_Context *t,
                         unsigned page_index,
                         minitiff_uint32 *ifd_result)
{
    minitiff_uint32 ifd;
    unsigned page;

    ifd = t->ifd_offset;

    for (page = 0; page < page_index; ++page) {
        minitiff_uint16 count;
        size_t bytes;
        size_t next_pos;

        if (!tiff_range_ok(t, ifd, 2))
            return 0;

        count = tiff_u16(t, t->data + ifd);

        if (!tiff_mul_size((size_t)count, 12, &bytes))
            return 0;

        if (!tiff_add_size((size_t)ifd, 2, &next_pos))
            return 0;

        if (!tiff_add_size(next_pos, bytes, &next_pos))
            return 0;

        if (!tiff_add_size(next_pos, 4, &next_pos))
            return 0;

        if (next_pos > t->size)
            return 0;

        ifd = tiff_u32(t, t->data + ifd + 2 + bytes);

        if (ifd == 0)
            return 0;
    }

    *ifd_result = ifd;
    return 1;
}


/* ------------------------------------------------------------------------- */
/* Page parsing                                                              */
/* ------------------------------------------------------------------------- */

static void tiff_page_free(TIFF_Page *p)
{
    free(p->strip_offsets);
    free(p->strip_byte_counts);
    free(p->color_map);

    memset(p, 0, sizeof(*p));
}


static int tiff_load_u32_array(const TIFF_Context *t,
                               const TIFF_Entry *e,
                               minitiff_uint32 **out,
                               minitiff_uint32 *count)
{
    minitiff_uint32 *a;
    minitiff_uint32 i;

    if (e->type != 3 && e->type != 4)
        return 0;

    if (e->count == 0 || e->count > SIZE_MAX / sizeof(minitiff_uint32))
        return 0;

    a = (minitiff_uint32 *)malloc((size_t)e->count * sizeof(minitiff_uint32));
    if (!a)
        return 0;

    for (i = 0; i < e->count; ++i) {
        if (!tiff_entry_get_u32(t, e, i, &a[i])) {
            free(a);
            return 0;
        }
    }

    *out = a;
    *count = e->count;
    return 1;
}


static int tiff_load_u16_array(const TIFF_Context *t,
                               const TIFF_Entry *e,
                               minitiff_uint16 **out,
                               minitiff_uint32 *count)
{
    minitiff_uint16 *a;
    minitiff_uint32 i;
    minitiff_uint32 v;

    if (e->type != 3)
        return 0;

    if (e->count == 0 ||
        (size_t)e->count > SIZE_MAX / sizeof(minitiff_uint16))
        return 0;

    a = (minitiff_uint16 *)malloc((size_t)e->count *
                           sizeof(minitiff_uint16));

    if (!a)
        return 0;

    for (i = 0; i < e->count; ++i) {
        if (!tiff_entry_get_u32(t, e, i, &v) ||
            v > 65535) {
            free(a);
            return 0;
        }

        a[i] = (minitiff_uint16)v;
    }

    *out = a;
    *count = e->count;
    return 1;
}


static int tiff_parse_page(const TIFF_Context *t,
                           minitiff_uint32 ifd_offset,
                           TIFF_Page *page)
{
    minitiff_uint16 count;
    minitiff_uint16 i;

    memset(page, 0, sizeof(*page));

    page->compression = 1;
    page->planar_config = 1;
    page->predictor = 1;

    if (!tiff_range_ok(t, ifd_offset, 2))
        return 0;

    count = tiff_u16(t, t->data + ifd_offset);

    if (!tiff_range_ok(t,
                       ifd_offset + 2,
                       (size_t)count * 12 + 4))
        return 0;

    for (i = 0; i < count; ++i) {
        TIFF_Entry e;
        minitiff_uint32 v;

        tiff_read_entry(t,
                        t->data + ifd_offset + 2 + (size_t)i * 12,
                        &e);

        switch (e.tag) {

        case 256: /* ImageWidth */
            if (!tiff_entry_get_u32(t, &e, 0, &v))
                return 0;
            page->width = v;
            break;

        case 257: /* ImageLength */
            if (!tiff_entry_get_u32(t, &e, 0, &v))
                return 0;
            page->height = v;
            break;

        case 258: /* BitsPerSample */
            if (e.count > 4)
                return 0;

            page->bits_count = (minitiff_uint16)e.count;

            {
                minitiff_uint32 j;

                for (j = 0; j < e.count; ++j) {
                    if (!tiff_entry_get_u32(t, &e, j, &v) ||
                        v > 65535)
                        return 0;

                    page->bits_per_sample[j] = (minitiff_uint16)v;
                }
            }
            break;

        case 259: /* Compression */
            if (!tiff_entry_get_u32(t, &e, 0, &v))
                return 0;
            page->compression = (minitiff_uint16)v;
            break;

        case 262: /* PhotometricInterpretation */
            if (!tiff_entry_get_u32(t, &e, 0, &v))
                return 0;
            page->photometric = (minitiff_uint16)v;
            break;

        case 273: /* StripOffsets */
            if (!tiff_load_u32_array(t, &e,
                                     &page->strip_offsets,
                                     &page->strip_count))
                return 0;
            break;

        case 277: /* SamplesPerPixel */
            if (!tiff_entry_get_u32(t, &e, 0, &v))
                return 0;
            page->samples_per_pixel = (minitiff_uint16)v;
            break;

        case 278: /* RowsPerStrip */
            /* Not actually needed because strip byte counts are used. */
            break;

        case 279: /* StripByteCounts */
            {
                minitiff_uint32 n;

                if (!tiff_load_u32_array(t, &e,
                                         &page->strip_byte_counts,
                                         &n))
                    return 0;

                /*
                    StripOffsets and StripByteCounts should have
                    matching counts.
                */
                if (page->strip_count != 0 &&
                    page->strip_count != n)
                    return 0;

                page->strip_count = n;
            }
            break;

        case 284: /* PlanarConfiguration */
            if (!tiff_entry_get_u32(t, &e, 0, &v))
                return 0;
            page->planar_config = (minitiff_uint16)v;
            break;

        case 317: /* Predictor */
            if (!tiff_entry_get_u32(t, &e, 0, &v))
                return 0;
            page->predictor = (minitiff_uint16)v;
            break;

        case 320: /* ColorMap */
            if (!tiff_load_u16_array(t, &e,
                                     &page->color_map,
                                     &page->color_map_count))
                return 0;
            break;

        case 338: /* ExtraSamples */
            if (e.count > 4)
                return 0;

            page->extra_count = (minitiff_uint16)e.count;

            {
                minitiff_uint32 j;

                for (j = 0; j < e.count; ++j) {
                    if (!tiff_entry_get_u32(t, &e, j, &v) ||
                        v > 65535)
                        return 0;

                    page->extra_samples[j] = (minitiff_uint16)v;
                }
            }
            break;

        default:
            break;
        }
    }

    /*
        Defaults mandated/commonly used by baseline TIFF.
    */
    if (page->samples_per_pixel == 0) {
        if (page->photometric == 2)
            page->samples_per_pixel = 3;
        else
            page->samples_per_pixel = 1;
    }

    if (page->bits_count == 0) {
        page->bits_count = page->samples_per_pixel;

        /*
            In practice BitsPerSample should exist. This fallback is
            intentionally conservative.
        */
        if (page->bits_count > 4)
            return 0;

        {
            minitiff_uint16 j;

            for (j = 0; j < page->bits_count; ++j)
                page->bits_per_sample[j] = 8;
        }
    }

    if (page->width == 0 ||
        page->height == 0 ||
        page->strip_count == 0 ||
        !page->strip_offsets ||
        !page->strip_byte_counts)
        return 0;

    if (page->samples_per_pixel == 0 ||
        page->samples_per_pixel > 4)
        return 0;

    if (page->bits_count != 1 &&
        page->bits_count != page->samples_per_pixel)
        return 0;

    {
        minitiff_uint16 j;

        for (j = 0; j < page->bits_count; ++j) {
            if (page->bits_per_sample[j] != 8)
                return 0;
        }
    }

    if (page->planar_config != 1)
        return 0;

    if (page->photometric != 0 &&
        page->photometric != 1 &&
        page->photometric != 2 &&
        page->photometric != 3)
        return 0;

    if (page->compression != 1 &&
        page->compression != 5 &&
        page->compression != 32773)
        return 0;

    if (page->predictor != 1 &&
        page->predictor != 2)
        return 0;

    if (page->photometric == 3 && !page->color_map)
        return 0;

    return 1;
}


/* ------------------------------------------------------------------------- */
/* PackBits decoder                                                           */
/* ------------------------------------------------------------------------- */

static int tiff_packbits_decode(const minitiff_uint8 *src,
                                size_t src_size,
                                minitiff_uint8 *dst,
                                size_t dst_size)
{
    size_t si = 0;
    size_t di = 0;

    while (si < src_size && di < dst_size) {
        minitiff_int8 n = (minitiff_int8)src[si++];

        if (n >= 0) {
            size_t count = (size_t)n + 1;

            if (si > src_size - count)
                return 0;

            if (di > dst_size - count)
                return 0;

            memcpy(dst + di, src + si, count);

            si += count;
            di += count;
        }
        else if (n >= -127) {
            size_t count = (size_t)(1 - n);

            if (si >= src_size)
                return 0;

            if (di > dst_size - count)
                return 0;

            memset(dst + di, src[si], count);

            ++si;
            di += count;
        }
        else {
            /*
                -128 is a NOP.
            */
        }
    }

    return di == dst_size;
}


/* ------------------------------------------------------------------------- */
/* TIFF LZW decoder                                                          */
/* ------------------------------------------------------------------------- */

typedef struct TIFF_LZW {
    int prefix[4096];
    minitiff_uint8 suffix[4096];
    minitiff_uint8 stack[4096];

    int code_size;
    int next_code;
    int old_code;

    int clear_code;
    int end_code;

    minitiff_uint32 bit_buffer;
    int bit_count;
} TIFF_LZW;


/*
    TIFF LZW is MSB-first.

    This is different from the LSB-first packing used by GIF LZW.
*/
static int tiff_lzw_get_code(TIFF_LZW *lzw,
                             const minitiff_uint8 *src,
                             size_t src_size,
                             size_t *pos)
{
    while (lzw->bit_count < lzw->code_size) {
        if (*pos >= src_size)
            return -1;

        lzw->bit_buffer =
            (lzw->bit_buffer << 8) | src[(*pos)++];

        lzw->bit_count += 8;
    }

    {
        minitiff_uint32 mask =
            (1u << lzw->code_size) - 1u;

        int shift =
            lzw->bit_count - lzw->code_size;

        int code =
            (int)((lzw->bit_buffer >> shift) & mask);

        lzw->bit_count -= lzw->code_size;

        if (lzw->bit_count == 0) {
            lzw->bit_buffer = 0;
        }
        else {
            lzw->bit_buffer &=
                (1u << lzw->bit_count) - 1u;
        }

        return code;
    }
}


static int tiff_lzw_decode(const minitiff_uint8 *src,
                           size_t src_size,
                           minitiff_uint8 *dst,
                           size_t dst_size)
{
    TIFF_LZW lzw;

    size_t pos = 0;
    size_t out = 0;

    int first_char = -1;

    memset(&lzw, 0, sizeof(lzw));

    lzw.clear_code = 256;
    lzw.end_code   = 257;

    lzw.code_size = 9;
    lzw.next_code = 258;
    lzw.old_code  = -1;

    while (out < dst_size) {
        int code;

        code = tiff_lzw_get_code(&lzw,
                                 src,
                                 src_size,
                                 &pos);

        if (code < 0)
            return 0;

        /*
            CLEAR resets the dictionary.
        */
        if (code == lzw.clear_code) {
            lzw.code_size = 9;
            lzw.next_code = 258;
            lzw.old_code = -1;
            lzw.bit_buffer = 0;
            lzw.bit_count = 0;
            first_char = -1;
            continue;
        }

        if (code == lzw.end_code)
            break;

        /*
            First code after CLEAR.
        */
        if (lzw.old_code < 0) {
            if (code >= 256)
                return 0;

            if (out >= dst_size)
                return 0;

            dst[out++] = (minitiff_uint8)code;

            first_char = code;
            lzw.old_code = code;

            continue;
        }

        {
            int cur = code;
            int stack_count = 0;

            /*
                Special KwKwK case.
            */
            if (code == lzw.next_code) {
                if (first_char < 0)
                    return 0;

                if (stack_count >= 4096)
                    return 0;

                lzw.stack[stack_count++] =
                    (minitiff_uint8)first_char;

                cur = lzw.old_code;
            }
            else if (code > lzw.next_code) {
                return 0;
            }

            /*
                Walk dictionary backwards.
            */
            while (cur >= 256) {
                if (cur < 258 || cur >= 4096)
                    return 0;

                if (stack_count >= 4096)
                    return 0;

                lzw.stack[stack_count++] =
                    lzw.suffix[cur];

                cur = lzw.prefix[cur];
            }

            if (cur < 0 || cur > 255)
                return 0;

            first_char = cur;

            if (stack_count >= 4096)
                return 0;

            lzw.stack[stack_count++] =
                (minitiff_uint8)cur;

            /*
                Emit string in forward order.
            */
            while (stack_count > 0) {
                if (out >= dst_size)
                    return 0;

                dst[out++] =
                    lzw.stack[--stack_count];
            }

            /*
                Add:
                    old_string + first_character
                to dictionary.
            */
            if (lzw.next_code < 4096) {
                lzw.prefix[lzw.next_code] =
                    lzw.old_code;

                lzw.suffix[lzw.next_code] =
                    (minitiff_uint8)first_char;

                ++lzw.next_code;

                /*
                    TIFF uses "early change".

                    When the next free code reaches
                    2^code_size - 1, increase the code width.
                */
                if (lzw.next_code ==
                    ((1 << lzw.code_size) - 1) &&
                    lzw.code_size < 12) {

                    ++lzw.code_size;
                }
            }

            lzw.old_code = code;
        }
    }

    return out == dst_size;
}


/* ------------------------------------------------------------------------- */
/* Strip decompression                                                       */
/* ------------------------------------------------------------------------- */

static int tiff_decode_strip(const TIFF_Context *t,
                             const TIFF_Page *page,
                             minitiff_uint32 strip,
                             minitiff_uint8 *dst,
                             size_t dst_size)
{
    minitiff_uint32 offset;
    minitiff_uint32 count;

    if (strip >= page->strip_count)
        return 0;

    offset = page->strip_offsets[strip];
    count = page->strip_byte_counts[strip];

    if (!tiff_range_ok(t, offset, count))
        return 0;

    switch (page->compression) {

    case 1:
        if ((size_t)count != dst_size)
            return 0;

        memcpy(dst, t->data + offset, dst_size);
        return 1;

    case 32773:
        return tiff_packbits_decode(t->data + offset,
                                     count,
                                     dst,
                                     dst_size);

    case 5:
        return tiff_lzw_decode(t->data + offset,
                               count,
                               dst,
                               dst_size);

    default:
        return 0;
    }
}


/* ------------------------------------------------------------------------- */
/* Predictor                                                                 */
/* ------------------------------------------------------------------------- */

static void tiff_predictor_horizontal(minitiff_uint8 *data,
                                      minitiff_uint32 width,
                                      minitiff_uint32 rows,
                                      minitiff_uint32 channels)
{
    minitiff_uint32 y;

    for (y = 0; y < rows; ++y) {
        minitiff_uint8 *row =
            data + (size_t)y * width * channels;

        minitiff_uint32 x;

        for (x = channels; x < width * channels; ++x)
            row[x] = (minitiff_uint8)(row[x] + row[x - channels]);
    }
}


/* ------------------------------------------------------------------------- */
/* Pixel conversion                                                          */
/* ------------------------------------------------------------------------- */

static minitiff_uint8 tiff_palette_value(const TIFF_Page *p,
                                  minitiff_uint32 index,
                                  int channel)
{
    minitiff_uint32 n;

    if (!p->color_map)
        return 0;

    /*
        ColorMap consists of:
            Red[n]
            Green[n]
            Blue[n]

        TIFF palette entries are 16-bit.
    */
    n = p->color_map_count / 3;

    if (index >= n)
        return 0;

    {
        minitiff_uint16 v =
            p->color_map[channel * n + index];

        /*
            TIFF palette values are normally 0..65535.
            Convert to 8 bit by taking the high byte.
        */
        return (minitiff_uint8)(v >> 8);
    }
}


static void tiff_put_pixel(TIFF_Image *img,
                           minitiff_uint32 x,
                           minitiff_uint32 y,
                           minitiff_uint8 r,
                           minitiff_uint8 g,
                           minitiff_uint8 b,
                           minitiff_uint8 a)
{
    size_t index =
        ((size_t)y * img->width + x) * 4;

    img->pixels[index + 0] = r;
    img->pixels[index + 1] = g;
    img->pixels[index + 2] = b;
    img->pixels[index + 3] = a;
}


static int tiff_convert_pixels(const TIFF_Page *p,
                               const minitiff_uint8 *raw,
                               TIFF_Image *img)
{
    minitiff_uint32 x, y;
    minitiff_uint32 channels = p->samples_per_pixel;

    for (y = 0; y < p->height; ++y) {
        const minitiff_uint8 *row =
            raw + (size_t)y * p->width * channels;

        for (x = 0; x < p->width; ++x) {
            const minitiff_uint8 *s =
                row + (size_t)x * channels;

            minitiff_uint8 r, g, b, a;

            switch (p->photometric) {

            case 0:
                /*
                    WhiteIsZero grayscale.
                */
                r = g = b = (minitiff_uint8)(255 - s[0]);
                a = 255;

                if (channels >= 2)
                    a = s[1];

                break;

            case 1:
                /*
                    BlackIsZero grayscale.
                */
                r = g = b = s[0];
                a = 255;

                if (channels >= 2)
                    a = s[1];

                break;

            case 2:
                /*
                    RGB.
                */
                if (channels < 3)
                    return 0;

                r = s[0];
                g = s[1];
                b = s[2];

                a = 255;

                if (channels >= 4)
                    a = s[3];

                break;

            case 3:
                /*
                    Palette.
                */
                r = tiff_palette_value(p, s[0], 0);
                g = tiff_palette_value(p, s[0], 1);
                b = tiff_palette_value(p, s[0], 2);
                a = 255;

                if (channels >= 2)
                    a = s[1];

                break;

            default:
                return 0;
            }

            tiff_put_pixel(img, x, y, r, g, b, a);
        }
    }

    return 1;
}


/* ------------------------------------------------------------------------- */
/* Main image decoder                                                        */
/* ------------------------------------------------------------------------- */

static TIFF_Image *tiff_decode_page(const TIFF_Context *t,
                                    const TIFF_Page *p)
{
    TIFF_Image *img = NULL;

    minitiff_uint8 *raw = NULL;

    size_t bytes_per_row;
    size_t raw_size;

    minitiff_uint32 y;
    minitiff_uint32 strip;
    minitiff_uint32 rows_per_strip;

    if (!tiff_mul_size((size_t)p->width,
                       (size_t)p->samples_per_pixel,
                       &bytes_per_row))
        return NULL;

    if (!tiff_mul_size(bytes_per_row,
                       (size_t)p->height,
                       &raw_size))
        return NULL;

    /*
        Limit allocations to SIZE_MAX and also make sure width*height
        can be represented in a normal allocation.
    */
    if (raw_size == 0)
        return NULL;

    raw = (minitiff_uint8 *)malloc(raw_size);
    if (!raw)
        return NULL;

    /*
        TIFF does not require RowsPerStrip to be retained after parsing
        if we calculate the expected strip size from the total image
        and strip count.

        This works for normal sequential strips, but TIFF technically
        permits arbitrary strip organization. For a small decoder,
        use an even distribution based on strip count.
    */
    rows_per_strip =
        (p->height + p->strip_count - 1) /
        p->strip_count;

    if (rows_per_strip == 0)
        rows_per_strip = 1;

    /*
        Decode each strip.
    */
    y = 0;

    for (strip = 0;
         strip < p->strip_count && y < p->height;
         ++strip) {

        minitiff_uint32 rows;
        size_t strip_size;
        minitiff_uint8 *dest;

        rows = p->height - y;

        if (rows > rows_per_strip)
            rows = rows_per_strip;

        if (!tiff_mul_size(bytes_per_row,
                           rows,
                           &strip_size)) {
            free(raw);
            return NULL;
        }

        dest = raw + (size_t)y * bytes_per_row;

        if (!tiff_decode_strip(t,
                               p,
                               strip,
                               dest,
                               strip_size)) {
            free(raw);
            return NULL;
        }

        y += rows;
    }

    if (y != p->height) {
        free(raw);
        return NULL;
    }

    if (p->predictor == 2) {
        tiff_predictor_horizontal(raw,
                                  p->width,
                                  p->height,
                                  p->samples_per_pixel);
    }

    img = (TIFF_Image *)calloc(1, sizeof(TIFF_Image));

    if (!img) {
        free(raw);
        return NULL;
    }

    img->width = p->width;
    img->height = p->height;
    img->channels = 4;
    img->bits_per_channel = 8;

    {
        size_t pixel_count;
        size_t pixel_bytes;

        if (!tiff_mul_size((size_t)p->width,
                           (size_t)p->height,
                           &pixel_count) ||
            !tiff_mul_size(pixel_count,
                           4,
                           &pixel_bytes)) {
            free(raw);
            free(img);
            return NULL;
        }

        img->pixels = (minitiff_uint8 *)malloc(pixel_bytes);

        if (!img->pixels) {
            free(raw);
            free(img);
            return NULL;
        }
    }

    if (!tiff_convert_pixels(p, raw, img)) {
        free(raw);
        tiff_free(img);
        return NULL;
    }

    free(raw);

    return img;
}


/* ------------------------------------------------------------------------- */
/* Public tiff_load                                                          */
/* ------------------------------------------------------------------------- */

TIFF_Image *tiff_load(const void *data,
                      size_t size,
                      unsigned page_index)
{
    TIFF_Context t;
    minitiff_uint16 version;
    minitiff_uint32 ifd;
    TIFF_Page page;
    TIFF_Image *result;

    if (!data || size < 8)
        return NULL;

    memset(&t, 0, sizeof(t));

    t.data = (const minitiff_uint8 *)data;
    t.size = size;

    /*
        Byte order.
    */
    if (t.data[0] == 'I' && t.data[1] == 'I') {
        t.little_endian = 1;
    }
    else if (t.data[0] == 'M' && t.data[1] == 'M') {
        t.little_endian = 0;
    }
    else {
        return NULL;
    }

    /*
        TIFF magic number = 42.
    */
    version = tiff_u16(&t, t.data + 2);

    if (version != 42)
        return NULL;

    t.ifd_offset = tiff_u32(&t, t.data + 4);

    if (t.ifd_offset == 0)
        return NULL;

    if (!tiff_find_ifd(&t,
                       page_index,
                       &ifd))
        return NULL;

    if (!tiff_parse_page(&t,
                         ifd,
                         &page))
        return NULL;

    result = tiff_decode_page(&t, &page);

    tiff_page_free(&page);

    return result;
}


/* ------------------------------------------------------------------------- */
/* Public tiff_load_file                                                     */
/* ------------------------------------------------------------------------- */

TIFF_Image *tiff_load_file(const char *filename,
                           unsigned page_index)
{
    FILE *f;
    long file_size_long;
    size_t file_size;
    minitiff_uint8 *data;
    TIFF_Image *image;

    if (!filename)
        return NULL;

    f = fopen(filename, "rb");

    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    file_size_long = ftell(f);

    if (file_size_long < 0) {
        fclose(f);
        return NULL;
    }

    file_size = (size_t)file_size_long;

    if (file_size == 0) {
        fclose(f);
        return NULL;
    }

    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }

    data = (minitiff_uint8 *)malloc(file_size);

    if (!data) {
        fclose(f);
        return NULL;
    }

    if (fread(data, 1, file_size, f) != file_size) {
        free(data);
        fclose(f);
        return NULL;
    }

    fclose(f);

    image = tiff_load(data,
                      file_size,
                      page_index);

    free(data);

    return image;
}


/* ------------------------------------------------------------------------- */
/* Public tiff_free                                                          */
/* ------------------------------------------------------------------------- */

void tiff_free(TIFF_Image *image)
{
    if (!image)
        return;

    free(image->pixels);
    free(image);
}


/* ------------------------------------------------------------------------- */
/* Optional test program                                                     */
/* ------------------------------------------------------------------------- */

#ifdef TIFF_TEST

int main(int argc, char **argv)
{
    TIFF_Image *img;

    if (argc < 2) {
        fprintf(stderr,
                "usage: %s file.tif [page]\n",
                argv[0]);
        return 1;
    }

    img = tiff_load_file(argv[1],
                         argc >= 3 ?
                         (unsigned)atoi(argv[2]) :
                         0);

    if (!img) {
        fprintf(stderr, "TIFF decode failed\n");
        return 1;
    }

    printf("width  = %u\n", img->width);
    printf("height = %u\n", img->height);
    printf("format = RGBA8\n");

    tiff_free(img);

    return 0;
}

#endif

#endif /* MINITIFF_IMPLEMENTATION */

#endif /* _MINITFF_H */
