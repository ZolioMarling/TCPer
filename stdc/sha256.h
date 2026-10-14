#ifndef _SHA256_H_
#define _SHA256_H_


#include <stdio.h>
#include <string.h>


#include "type.h"


#define SHA256_DIGEST_SIZE 32
#define SHA256_HEX_LENGTH 64


typedef struct SHA256Context {
    uint32 state[8];
    uint64 bit_length;
    byte block[64];
    usize block_length;
} SHA256Context;


typedef struct HMAC256Context {
    SHA256Context inner;
    byte outer_pad[64];
} HMAC256Context;


/**
 * @brief Initialize a `SHA-256` context.
 * @param ctx The `SHA-256` context to initialize.
**/
void sha256_init(SHA256Context *ctx);


/**
 * @brief Add data to a `SHA-256` calculation.
 * @param ctx The `SHA-256` context.
 * @param data The data to hash.
 * @param length The number of bytes in data.
**/
void sha256_update(SHA256Context *ctx, void *data, usize length);


/**
 * @brief Finish a `SHA-256` calculation and write its digest.
 * @param ctx The `SHA-256` context.
 * @param digest The output buffer of `SHA256_DIGEST_SIZE` bytes.
**/
void sha256_final(SHA256Context *ctx, byte digest[static SHA256_DIGEST_SIZE]);


/**
 * @brief Calculate the `SHA-256` hash of a NUL-terminated string.
 * @param input The input string.
 * @param hash The output buffer of `SHA256_HEX_LENGTH + 1` characters.
**/
void sha256_string(char *input, char hash[static SHA256_HEX_LENGTH + 1]);


/**
 * @brief Calculate the `SHA-256` hash of an open file.
 * @param file The file to read from its current position.
 * @param hash The output buffer of `SHA256_HEX_LENGTH + 1` characters.
**/
void sha256_file(FILE *file, char hash[static SHA256_HEX_LENGTH + 1]);


/**
 * @brief Initialize an `HMAC-SHA-256` context.
 * @param ctx The `HMAC-SHA-256` context to initialize.
 * @param key The `HMAC` key.
 * @param key_length The number of bytes in key.
**/
void hmac_sha256_init(HMAC256Context *ctx, void *key, usize key_length);


/**
 * @brief Add data to an `HMAC-SHA-256` calculation.
 * @param ctx The `HMAC-SHA-256` context.
 * @param data The data to authenticate.
 * @param data_length The number of bytes in data.
**/
void hmac_sha256_update(HMAC256Context *ctx, void *data, usize data_length);


/**
 * @brief Finish an `HMAC-SHA-256` calculation and write its digest.
 * @param ctx The `HMAC-SHA-256` context.
 * @param digest The output buffer of `SHA256_DIGEST_SIZE` bytes.
**/
void hmac_sha256_final(HMAC256Context *ctx, byte digest[static SHA256_DIGEST_SIZE]);


/**
 * @brief Calculate an `HMAC-SHA-256` digest in one call.
 * @param key The `HMAC` key.
 * @param key_length The number of bytes in key.
 * @param data The data to authenticate.
 * @param data_length The number of bytes in data.
 * @param digest The output buffer of `SHA256_DIGEST_SIZE` bytes.
**/
void hmac_sha256(void *key, usize key_length, void *data, usize data_length, byte digest[static SHA256_DIGEST_SIZE]);


/**
 * @brief Compare two memory regions without early exit.
 * @param left The first memory region.
 * @param right The second memory region.
 * @param length The number of bytes to compare.
 * @return `True` when the regions are equal, otherwise `False`.
**/
bool secure_memequal(void *left, void *right, usize length);


/**
 * @brief Clear a memory region in a way intended to resist compiler elision.
 * @param buffer The memory region to clear.
 * @param length The number of bytes to clear.
**/
void secure_memzero(void *buffer, usize length);


/**
 * @brief Encode a `SHA-256` digest as a lowercase hexadecimal string.
 * @param digest The digest to encode.
 * @param hash The output buffer of `SHA256_HEX_LENGTH + 1` characters.
**/
void __sha256_hex_encode__(byte digest[static SHA256_DIGEST_SIZE], char hash[static SHA256_HEX_LENGTH + 1]);


/**
 * @brief Rotate a 32-bit value right by the specified number of bits.
 * @param value The value to rotate.
 * @param bits The number of bits to rotate.
 * @return The rotated value.
**/
uint32 __rotate_right__(uint32 value, unsigned int bits);


/**
 * @brief Process one 64-byte `SHA-256` message block.
 * @param context The `SHA-256` context to update.
 * @param block The 64-byte input block.
**/
void __sha256_transform__(SHA256Context *ctx, byte block[static 64]);


#endif
