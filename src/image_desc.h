#ifndef IMAGE_DESC_H
#define IMAGE_DESC_H

/* The running application's own image (#24). */
#include "fwimg.h"

#include <stdint.h>

extern const struct fwimg_desc image_desc;

/* Bytes from FWIMG_APP_BASE to the end of the image (the .bin's size). */
uint32_t image_size(void);

#endif
