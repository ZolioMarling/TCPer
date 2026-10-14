#include "sha256.h"


static const uint32 SHA256_CONSTANTS[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};


void sha256_init(SHA256Context *ctx) {
    ctx->state[0] = 0x6a09e667U;
    ctx->state[1] = 0xbb67ae85U;
    ctx->state[2] = 0x3c6ef372U;
    ctx->state[3] = 0xa54ff53aU;
    ctx->state[4] = 0x510e527fU;
    ctx->state[5] = 0x9b05688cU;
    ctx->state[6] = 0x1f83d9abU;
    ctx->state[7] = 0x5be0cd19U;
    ctx->bit_length = 0;
    ctx->block_length = 0;
}


void sha256_update(SHA256Context *ctx, void *data, usize length) {
    byte *input = (byte *)data;
    for (usize i = 0; i < length; i++) {
        ctx->block[ctx->block_length++] = input[i];
        if (ctx->block_length == sizeof(ctx->block)) {
            __sha256_transform__(ctx, ctx->block);
            ctx->bit_length = ctx->bit_length + 512;
            ctx->block_length = 0;
        }
    }
}


void sha256_final(SHA256Context *ctx, byte digest[static SHA256_DIGEST_SIZE]) {
    usize index = ctx->block_length;
    ctx->block[index++] = 0x80;
    if (index > 56) {
        while (index < sizeof(ctx->block)) ctx->block[index++] = 0;
        __sha256_transform__(ctx, ctx->block);
        index = 0;
    }
    while (index < 56) ctx->block[index++] = 0;

    ctx->bit_length = ctx->bit_length + ctx->block_length * 8;
    for (unsigned int i = 0; i < 8; i++) ctx->block[63 - i] = (byte)(ctx->bit_length >> (i * 8));
    __sha256_transform__(ctx, ctx->block);

    for (unsigned int i = 0; i < 8; i++) {
        digest[i * 4] = (byte)(ctx->state[i] >> 24);
        digest[i * 4 + 1] = (byte)(ctx->state[i] >> 16);
        digest[i * 4 + 2] = (byte)(ctx->state[i] >> 8);
        digest[i * 4 + 3] = (byte)ctx->state[i];
    }
    secure_memzero(ctx, sizeof(*ctx));
}


void sha256_string(char *input, char hash[static SHA256_HEX_LENGTH + 1]) {
    if (!input || !hash) return;
    SHA256Context context;
    byte digest[SHA256_DIGEST_SIZE];
    sha256_init(&context);
    sha256_update(&context, input, strlen(input));
    sha256_final(&context, digest);
    __sha256_hex_encode__(digest, hash);
    secure_memzero(digest, sizeof(digest));
}


void sha256_file(FILE *file, char hash[static SHA256_HEX_LENGTH + 1]) {
    if (!file || !hash) return;
    SHA256Context context;
    byte digest[SHA256_DIGEST_SIZE];
    char buffer[65536];
    sha256_init(&context);
    usize length;
    while ((length = fread(buffer, 1, sizeof(buffer), file)) > 0) sha256_update(&context, buffer, length);
    sha256_final(&context, digest);
    __sha256_hex_encode__(digest, hash);
    secure_memzero(digest, sizeof(digest));
}


void hmac_sha256_init(HMAC256Context *ctx, void *key, usize key_length) {
    byte normalized_key[64] = {0};
    if (key_length > sizeof(normalized_key)) {
        SHA256Context key_context;
        sha256_init(&key_context);
        sha256_update(&key_context, key, key_length);
        sha256_final(&key_context, normalized_key);
    } else if (key_length > 0) memcpy(normalized_key, key, key_length);

    byte inner_pad[64];
    for (usize i = 0; i < sizeof(normalized_key); i++) {
        inner_pad[i] = normalized_key[i] ^ 0x36;
        ctx->outer_pad[i] = normalized_key[i] ^ 0x5c;
    }

    sha256_init(&ctx->inner);
    sha256_update(&ctx->inner, inner_pad, sizeof(inner_pad));
    secure_memzero(normalized_key, sizeof(normalized_key));
    secure_memzero(inner_pad, sizeof(inner_pad));
}


void hmac_sha256_update(HMAC256Context *ctx, void *data, usize data_length) {
    sha256_update(&ctx->inner, data, data_length);
}


void hmac_sha256_final(HMAC256Context *ctx, byte digest[static SHA256_DIGEST_SIZE]) {
    byte inner_digest[SHA256_DIGEST_SIZE];
    sha256_final(&ctx->inner, inner_digest);

    SHA256Context outer;
    sha256_init(&outer);
    sha256_update(&outer, ctx->outer_pad, sizeof(ctx->outer_pad));
    sha256_update(&outer, inner_digest, sizeof(inner_digest));
    sha256_final(&outer, digest);

    secure_memzero(inner_digest, sizeof(inner_digest));
    secure_memzero(ctx, sizeof(*ctx));
}


void hmac_sha256(void *key, usize key_length, void *data, usize data_length, byte digest[static SHA256_DIGEST_SIZE]) {
    HMAC256Context context;
    hmac_sha256_init(&context, key, key_length);
    hmac_sha256_update(&context, data, data_length);
    hmac_sha256_final(&context, digest);
}


bool secure_memequal(void *left, void *right, usize length) {
    byte *a = (byte *)left;
    byte *b = (byte *)right;
    volatile byte difference = 0;
    for (usize i = 0; i < length; i++) difference = difference | (a[i] ^ b[i]);
    return difference == 0;
}


void secure_memzero(void *buffer, usize length) {
    volatile byte *bytes = (volatile byte *)buffer;
    while (length > 0) {
        *bytes++ = 0;
        length--;
    }
}


void __sha256_hex_encode__(byte digest[static SHA256_DIGEST_SIZE], char hash[static SHA256_HEX_LENGTH + 1]) {
    static const char digits[] = "0123456789abcdef";
    for (usize i = 0; i < SHA256_DIGEST_SIZE; i++) {
        hash[i * 2] = digits[digest[i] >> 4];
        hash[i * 2 + 1] = digits[digest[i] & 0x0f];
    }
    hash[SHA256_HEX_LENGTH] = '\0';
}


uint32 __rotate_right__(uint32 value, unsigned int bits) {
    return (value >> bits) | (value << (32U - bits));
}


void __sha256_transform__(SHA256Context *ctx, byte block[static 64]) {
    uint32 words[64];
    for (unsigned int i = 0; i < 16; i++) {
        unsigned int offset = i * 4;
        words[i] = ((uint32)block[offset] << 24) | ((uint32)block[offset + 1] << 16) | ((uint32)block[offset + 2] << 8) | (uint32)block[offset + 3];
    }
    for (unsigned int i = 16; i < 64; i++) {
        uint32 s0 = __rotate_right__(words[i - 15], 7) ^ __rotate_right__(words[i - 15], 18) ^ (words[i - 15] >> 3);
        uint32 s1 = __rotate_right__(words[i - 2], 17) ^ __rotate_right__(words[i - 2], 19) ^ (words[i - 2] >> 10);
        words[i] = words[i - 16] + s0 + words[i - 7] + s1;
    }

    uint32 a = ctx->state[0];
    uint32 b = ctx->state[1];
    uint32 c = ctx->state[2];
    uint32 d = ctx->state[3];
    uint32 e = ctx->state[4];
    uint32 f = ctx->state[5];
    uint32 g = ctx->state[6];
    uint32 h = ctx->state[7];

    for (unsigned int i = 0; i < 64; i++) {
        uint32 sum1 = __rotate_right__(e, 6) ^ __rotate_right__(e, 11) ^ __rotate_right__(e, 25);
        uint32 choose = (e & f) ^ (~e & g);
        uint32 temporary1 = h + sum1 + choose + SHA256_CONSTANTS[i] + words[i];
        uint32 sum0 = __rotate_right__(a, 2) ^ __rotate_right__(a, 13) ^ __rotate_right__(a, 22);
        uint32 majority = (a & b) ^ (a & c) ^ (b & c);
        uint32 temporary2 = sum0 + majority;

        h = g;
        g = f;
        f = e;
        e = d + temporary1;
        d = c;
        c = b;
        b = a;
        a = temporary1 + temporary2;
    }

    ctx->state[0] = ctx->state[0] + a;
    ctx->state[1] = ctx->state[1] + b;
    ctx->state[2] = ctx->state[2] + c;
    ctx->state[3] = ctx->state[3] + d;
    ctx->state[4] = ctx->state[4] + e;
    ctx->state[5] = ctx->state[5] + f;
    ctx->state[6] = ctx->state[6] + g;
    ctx->state[7] = ctx->state[7] + h;
}
