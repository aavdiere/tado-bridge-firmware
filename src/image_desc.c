/* The application's image descriptor (#24, src/fwimg.h): placed at
 * FWIMG_APP_BASE + FWIMG_DESC_OFFSET by cmake/sections.ld, right after the
 * vector table. The bootloader starts the application only if this is
 * there; its image_end gives the image's length, for the application to
 * save itself into the previous update slot. */
#include "image_desc.h"

extern const uint8_t _image_start[], _image_end[]; /* cmake/sections.ld */

#ifndef FW_VERSION
#error "FW_VERSION not defined (cmake/CMakeLists.txt)"
#endif

__attribute__((used, section(".image_desc"))) const struct fwimg_desc image_desc = {
    .magic      = FWIMG_DESC_MAGIC,
    .version    = FWIMG_DESC_VERSION,
    .len        = FWIMG_DESC_LEN,
    .image_end  = (uint32_t)_image_end,
    .fw_version = FW_VERSION, /* informational: the build option */
};

uint32_t image_size(void) {
    return image_desc.image_end - (uint32_t)_image_start;
}
