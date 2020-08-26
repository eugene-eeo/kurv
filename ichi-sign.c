#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>

#include "monocypher/monocypher.h"
#include "base64/base64.h"
#include "utils.h"


#define ERR(...)      _err("ichi-sign", __VA_ARGS__)
#define WIPE_BUF(buf) crypto_wipe(buf, sizeof(buf));

#define BUF_SIZE (64 * 1024)
#define B64_KEY_SIZE (44)
#define B64_SIG_SIZE (88)
#define SIG_ARMOR_TOP "\n----BEGIN ICHI SIGNATURE----\n"
#define SIG_ARMOR_END "\n---- END ICHI SIGNATURE ----\n"

#define XERR(...)    do { ERR(__VA_ARGS__); goto error; } while(0)
#define XREAD(...)   do { if (_read(__VA_ARGS__)) XERR("fread()"); } while(0)
#define XWRITE(...)  do { if (_write(__VA_ARGS__)) XERR("fwrite()"); } while(0)

const char* HELP =
    "usage:\n"
    "  ichi-sign -k SK [-d] [-o OUTPUT] [INPUT]\n"
    "  ichi-sign -V [-p PK] [-s SIG] [-x] [-o OUTPUT] [INPUT]\n"
    "\n"
    "options:\n"
    "  -o OUTPUT specify output file.\n"
    "  -k SK     uses the secret key at path SK to sign INPUT.\n"
    "  -d        produce a detached signature at OUTPUT.\n"
    "  -V        verify detached signature SIG or joined signature in INPUT.\n"
    "  -p PK     specify public key at path PK.\n"
    "  -s SIG    specify file for detached signature.\n"
    "  -x        print out contents if verification is successful.\n"
    "\n"
    "INPUT and OUTPUT default to stdin and stdout respectively.\n"
    "\n"
    "SK and PK can be generated by ichi-keygen.\n\n";

enum action {
    ACTION_SIGN,
    ACTION_VERIFY,
    ACTION_TRIM,
};

struct sign_ctx {
    FILE* input;
    FILE* output;
    uint8_t sk[32];
    uint8_t detached; // produce a detached signature
};

struct verify_ctx {
    FILE* input;
    FILE* output;
    FILE* sig; // if detached, check this for signature instead.
    char* pk_fn;
    uint8_t pk[32];
    uint8_t detached;
    uint8_t keyring;
    uint8_t stream_output;
};

int trim(FILE* input, FILE* output);
int sign(struct sign_ctx ctx);
int verify(struct verify_ctx ctx);
int verify_keyring(const struct verify_ctx* ctx,
                   const uint8_t *msg, size_t msg_size,
                   const uint8_t *sig);

uint8_t* fp_to_buf(FILE* fp, size_t* buf_size)
{
    size_t total = 0;
    size_t size = BUF_SIZE;
    uint8_t* buf = malloc(size);
    if (buf == NULL)
        XERR("malloc()");

    while (1) {
        total += fread(buf + total, 1, BUF_SIZE, fp);
        if (ferror(fp))
            XERR("fread()");
        if (feof(fp)) {
            *buf_size = total;
            return buf;
        }
        if (total + BUF_SIZE > size) {
            size += BUF_SIZE;
            uint8_t* new_buf = realloc(buf, size);
            if (new_buf == NULL) {
                free(buf);
                XERR("malloc()");
            }
            buf = new_buf;
        }
    }

error:
    return NULL;
}

int sign(struct sign_ctx ctx)
{
    int rv = 1;
    uint8_t pk      [32],
            sig     [64],
            b64_sig [B64_SIG_SIZE];

    size_t msg_size;
    uint8_t* msg = fp_to_buf(ctx.input, &msg_size);
    if (msg == NULL)
        goto error;

    crypto_sign_public_key(pk, ctx.sk);
    crypto_sign(sig, ctx.sk, pk, msg, msg_size);
    b64_encode(b64_sig, sig, 64);

    if (ctx.detached) {
        XWRITE(ctx.output, b64_sig, B64_SIG_SIZE);
    } else {
        XWRITE(ctx.output, msg, msg_size);
        XWRITE(ctx.output, (uint8_t *) SIG_ARMOR_TOP, strlen(SIG_ARMOR_TOP));
        XWRITE(ctx.output, b64_sig, 44);
        XWRITE(ctx.output, (uint8_t *) "\n", 1);
        XWRITE(ctx.output, b64_sig + 44, 44);
        XWRITE(ctx.output, (uint8_t *) SIG_ARMOR_END, strlen(SIG_ARMOR_END));
    }
    rv = 0;

error:
    if (msg != NULL) free(msg);
    return rv;
}

int sig_from_file(FILE* fp, uint8_t *sig)
{
    int rv = 1;
    uint8_t b64_sig[B64_SIG_SIZE];

    // read signature from ctx.sig
    XREAD(fp, b64_sig, B64_SIG_SIZE);
    if (b64_validate(b64_sig, B64_SIG_SIZE) != 0
            || b64_decoded_size(b64_sig, B64_SIG_SIZE) != 64)
        XERR("malformed signature");

    b64_decode(sig, b64_sig, B64_SIG_SIZE);
    rv = 0;

error:
    return rv;
}

int sig_from_buf(uint8_t* sig, size_t* new_buf_size, const uint8_t* buf, size_t buf_size)
{
    #define TOP_SZ   (strlen(SIG_ARMOR_TOP))
    #define SIG_SZ   (B64_SIG_SIZE + 1)
    #define END_SZ   (strlen(SIG_ARMOR_END))
    #define TOTAL_SZ (TOP_SZ + SIG_SZ + END_SZ)
    #define TOP_PTR  (buf + buf_size - TOTAL_SZ)
    #define SIG_PTR  (TOP_PTR + TOP_SZ)
    #define END_PTR  (SIG_PTR + SIG_SZ)

    uint8_t b64_sig[B64_SIG_SIZE];

    if (buf_size < TOTAL_SZ
            || memcmp(TOP_PTR, SIG_ARMOR_TOP, TOP_SZ) != 0
            || memcmp(END_PTR, SIG_ARMOR_END, END_SZ) != 0
            || SIG_PTR[44] != '\n')
        return -1;

    memcpy(b64_sig,      SIG_PTR,      44);
    memcpy(b64_sig + 44, SIG_PTR + 45, 44);
    if (b64_validate(b64_sig, B64_SIG_SIZE) != 0
            || b64_decoded_size(b64_sig, B64_SIG_SIZE) != 64)
        return -1;
    b64_decode(sig, b64_sig, B64_SIG_SIZE);
    *new_buf_size = buf_size - TOTAL_SZ;
    return 0;

    #undef TOP_SZ
    #undef SIG_SZ
    #undef END_SZ
    #undef TOTAL_SZ
    #undef TOP_PTR
    #undef SIG_PTR
    #undef END_PTR
}

int verify(struct verify_ctx ctx)
{
    int rv = 1;
    uint8_t sig[64];

    size_t msg_size;
    uint8_t *msg = fp_to_buf(ctx.input, &msg_size);
    if (msg == NULL)
        goto error;

    if (ctx.detached) {
        if (sig_from_file(ctx.sig, sig) != 0)
            goto error;
    } else {
        if (sig_from_buf(sig, &msg_size, msg, msg_size) != 0)
            XERR("malformed signature");
    }

    if (ctx.keyring) {
        rv = verify_keyring(&ctx, msg, msg_size, sig);
    } else {
        if (crypto_check(sig, ctx.pk, msg, msg_size) != 0)
            XERR("invalid signature");
        if (ctx.stream_output)
            XWRITE(ctx.output, msg, msg_size);
        ERR("good signature by '%s'", ctx.pk_fn);
        rv = 0;
    }

error:
    if (msg != NULL) free(msg);
    return rv;
}

int verify_keyring(const struct verify_ctx* ctx,
                   const uint8_t* msg, size_t msg_size,
                   const uint8_t* sig)
{
    int rv = 1;
    int dir_fd;
    DIR* dir = NULL;
    char* keyring_dir = getenv("ICHI_SIGN_KEYRING");
    if (keyring_dir == NULL)
        XERR("$ICHI_SIGN_KEYRING is unset");

    dir = opendir(keyring_dir);
    dir_fd = dirfd(dir);
    if (dir_fd < 0)
        XERR("dirfd()");

    uint8_t b64_pk [B64_KEY_SIZE],
            pk     [32];

    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strlen(entry->d_name) >= 9
                && strcmp(entry->d_name + strlen(entry->d_name) - 9, ".sign.pub") == 0) {
            int fd = openat(dir_fd, entry->d_name, O_RDONLY);
            if (fd < 0
                    || read(fd, b64_pk, B64_KEY_SIZE) < B64_KEY_SIZE
                    || b64_validate(b64_pk, B64_KEY_SIZE) != 0
                    || b64_decoded_size(b64_pk, B64_KEY_SIZE) != 32) {
                if (fd >= 0)
                    close(fd);
                continue;
            }
            close(fd);
            b64_decode(pk, b64_pk, B64_KEY_SIZE);
            if (crypto_check(sig, pk, msg, msg_size) == 0) {
                ERR("good signature by '%s%s%s'",
                    keyring_dir,
                    keyring_dir[strlen(keyring_dir)] == '/' ? "" : "/",
                    entry->d_name);
                if (ctx->stream_output)
                    XWRITE(ctx->output, msg, msg_size);
                rv = 0;
                break;
            }
        }
    }

error:
    if (dir != NULL)
        closedir(dir);
    return rv;
}

int trim(FILE* input, FILE* output)
{
    int rv = 1;
    size_t msg_size;
    uint8_t* msg = fp_to_buf(input, &msg_size);
    if (msg == NULL)
        goto error;

    uint8_t tmp[64];

    if (sig_from_buf(tmp, &msg_size, msg, msg_size) != 0)
        XERR("malformed signature");

    XWRITE(output, msg, msg_size);
    rv = 0;

error:
    if (msg != NULL) free(msg);
    return rv;
}

int read_key_from_file(char* fn, uint8_t *key)
{
    int rv = 1;
    uint8_t b64_key[B64_KEY_SIZE];
    FILE* fp = fopen(fn, "r");
    if (fp == NULL)
        XERR("fopen()");

    if (_read(fp, b64_key, sizeof(b64_key)) != 0)
        XERR("fread()");

    if (b64_validate(b64_key, sizeof(b64_key)) != 0
            || b64_decoded_size(b64_key, sizeof(b64_key)) != 32)
        XERR("malformed key");

    b64_decode(key, b64_key, sizeof(b64_key));
    rv = 0;

error:
    WIPE_BUF(b64_key);
    if (fp != NULL)
        fclose(fp);
    return rv;
}

int main(int argc, char** argv)
{
    int rv = 1;
    FILE* input = stdin;
    FILE* output = stdout;
    enum action action = ACTION_SIGN;

    struct sign_ctx sctx;
    sctx.detached = 0;

    struct verify_ctx vctx;
    vctx.sig = NULL;
    vctx.detached = 0;
    vctx.keyring = 1;
    vctx.stream_output = 0;

    int kflag = 0;
    int c;
    while ((c = getopt(argc, argv, "ho:k:dVp:s:xT")) != -1)
        switch (c) {
            default: goto error;
            case 'h':
                printf("%s", HELP);
                rv = 0;
                goto error;
            case 'o':
                output = fopen(optarg, "w");
                if (output == NULL)
                    XERR("fopen()");
                break;
            // Sign
            case 'k':
                kflag = 1;
                if (read_key_from_file(optarg, sctx.sk) != 0)
                    goto error;
                break;
            case 'd':
                sctx.detached = 1;
                break;
            // Verify
            case 'V':
                action = ACTION_VERIFY;
                break;
            case 'p':
                if (read_key_from_file(optarg, vctx.pk) != 0)
                    goto error;
                vctx.keyring = 0;
                vctx.pk_fn = optarg;
                break;
            case 's':
                vctx.detached = 1;
                vctx.sig = fopen(optarg, "r");
                if (vctx.sig == NULL)
                    XERR("fopen()");
                break;
            case 'x':
                vctx.stream_output = 1;
                break;
            // trim
            case 'T':
                action = ACTION_TRIM;
                break;
        }

    if (argc > optind + 1) XERR("invalid usage");
    if (argc == optind + 1) {
        input = fopen(argv[optind], "r");
        if (input == NULL)
            XERR("fopen()");
    }

    switch (action) {
        default: goto error;
        case ACTION_SIGN:
            if (!kflag) {
                ERR("no secret key specified");
                goto error;
            }
            sctx.output = output;
            sctx.input = input;
            rv = sign(sctx);
            break;
        case ACTION_VERIFY:
            vctx.output = output;
            vctx.input = input;
            rv = verify(vctx);
            break;
        case ACTION_TRIM:
            rv = trim(input, output);
            break;
    }

error:
    WIPE_BUF(sctx.sk);
    if (vctx.sig != NULL) fclose(vctx.sig);
    if (output != NULL) fclose(output);
    if (input != NULL) fclose(input);
    return rv;
}
