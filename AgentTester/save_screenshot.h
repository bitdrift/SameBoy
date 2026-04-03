#ifndef save_screenshot_h
#define save_screenshot_h

#include <stdint.h>
#include <stdbool.h>

/* Save RGBA pixel buffer to PNG file. Returns true on success. */
bool save_screenshot(const char *filename, uint32_t width, uint32_t height, const uint32_t *pixels);

#endif
