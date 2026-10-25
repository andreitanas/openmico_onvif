#ifndef UVC_HTTPS_H
#define UVC_HTTPS_H

#include <stddef.h>
#include <stdint.h>

struct form_field {
    const char *name, *value;
};

// POST a multipart/form-data body to an https:// URL: the given fields plus
// a file part named "payload" (what Protect's snapshot upload expects).
// Returns the HTTP status, or -1.
int https_post_file(const char *url, const struct form_field *fields, int nfields,
                    const char *filename, const uint8_t *data, size_t len);

#endif
