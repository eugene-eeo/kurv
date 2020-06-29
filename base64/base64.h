#ifndef BASE64_H
#define BASE64_H

#include <stddef.h>
#include <stdint.h>

size_t b64_encoded_size(size_t length);
size_t b64_decoded_size(const uint8_t input[], size_t input_size);
int b64_validate(const uint8_t input[], const size_t input_size);

// Direct interface
void b64_encode(uint8_t output[],
                const uint8_t input[], size_t input_size);


void b64_decode(uint8_t output[],
                const uint8_t input[], const size_t input_size);

// Incremental interface
typedef struct {
    uint8_t buf[3];
    size_t  bufsize;
} b64_encode_ctx;

void   b64_encode_init(b64_encode_ctx *ctx);
size_t b64_encode_update_size(size_t bufsize);
size_t b64_encode_update(b64_encode_ctx *ctx,
                         uint8_t out[],
                         const uint8_t buf[], size_t bufsize);
size_t b64_encode_final(b64_encode_ctx *ctx,
                        uint8_t out[]);
#endif
