#pragma once
#include <stddef.h>
#include <stdint.h>

// Decodes raw DEFLATE data. On success *out is malloc'd (caller frees) and 0 is returned.
int raw_inflate(const uint8_t *in, size_t inlen, uint8_t **out, size_t *outlen, size_t maxout);
