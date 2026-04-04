#import <AppKit/AppKit.h>
#import "save_screenshot.h"

bool save_screenshot(const char *filename, uint32_t width, uint32_t height, const uint32_t *pixels)
{
    @autoreleasepool {
        /* Input is RGBA (R in high byte): 0xRRGGBBAA */
        /* Convert to ABGR for NSBitmapImageRep which expects RGBA byte order on little-endian */
        size_t pixel_count = width * height;
        uint32_t *converted = malloc(pixel_count * sizeof(uint32_t));
        if (!converted) return false;

        for (size_t i = 0; i < pixel_count; i++) {
            uint32_t p = pixels[i];
            uint8_t r = (p >> 24) & 0xFF;
            uint8_t g = (p >> 16) & 0xFF;
            uint8_t b = (p >> 8) & 0xFF;
            /* Store as RGBA byte order */
            converted[i] = (r) | (g << 8) | (b << 16) | (0xFF << 24);
        }

        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:(unsigned char **)&converted
                                                                        pixelsWide:width
                                                                        pixelsHigh:height
                                                                     bitsPerSample:8
                                                                   samplesPerPixel:3
                                                                          hasAlpha:false
                                                                          isPlanar:false
                                                                    colorSpaceName:NSDeviceRGBColorSpace
                                                                      bitmapFormat:0
                                                                       bytesPerRow:4 * width
                                                                      bitsPerPixel:32];
        NSData *data = [rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        bool result = [data writeToFile:@(filename) atomically:true];
        free(converted);
        return result;
    }
}
