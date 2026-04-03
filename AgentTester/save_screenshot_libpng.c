#include "save_screenshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <png.h>

bool save_screenshot(const char *filename, uint32_t width, uint32_t height, const uint32_t *pixels)
{
    FILE *f = fopen(filename, "wb");
    if (!f) return false;

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) { fclose(f); return false; }

    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_write_struct(&png, NULL); fclose(f); return false; }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(f);
        return false;
    }

    png_init_io(png, f);
    png_set_IHDR(png, info, width, height, 8, PNG_COLOR_TYPE_RGB,
                 PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    uint8_t *row = malloc(width * 3);
    for (uint32_t y = 0; y < height; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t p = pixels[y * width + x];
            /* Input is RGBA: 0xRRGGBBAA */
            row[x * 3 + 0] = (p >> 24) & 0xFF;
            row[x * 3 + 1] = (p >> 16) & 0xFF;
            row[x * 3 + 2] = (p >> 8) & 0xFF;
        }
        png_write_row(png, row);
    }
    free(row);

    png_write_end(png, NULL);
    png_destroy_write_struct(&png, &info);
    fclose(f);
    return true;
}
