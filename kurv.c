#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/random.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>

#include <sys/stat.h>
#include <fcntl.h>

#include "base64.h"
#include "monocypher/monocypher.h"

//
// Macros
//
#define READ_SIZE (1024 * 4096)
#define B64_KEY_SIZE 44  // b64_encoded_size(32)
#define B64_SIG_SIZE 88  // b64_encoded_size(64)
#define die(...) {\
    fwrite("kurv: ", 1, 7, stderr);\
    fprintf(stderr, __VA_ARGS__);\
    fwrite("\n", 1, 1, stderr);\
    exit(1);\
}
#define errdie(prefix) {\
    err(prefix);\
    exit(1);\
}
#define err(prefix) {\
    fwrite("kurv: ", 1, 7, stderr);\
    perror(prefix);\
}

const uint8_t SIG_START[] = "\n----BEGIN KURV SIGNATURE----\n";
const uint8_t SIG_END[] =   "\n----END KURV SIGNATURE----\n";
const uint8_t USAGE[] =
    "usage:\n"
    "   kurv -h\n"
    "   kurv -g <name>\n"
    "   kurv -s <file> -P <privkey>\n"
    "   kurv -c <signed-file> [-p <pubkey>] [-io]\n"
    "   kurv -d <signed-file>\n"
    "\n"
    "options:\n"
    "   -h         show help page.\n"
    "   -g         generate keypair <name>.pub and <name>.priv.\n"
    "   -P <key>   private key file for signing.\n"
    "   -p <key>   public key file for checking.\n"
    "   -s <file>  sign <file> using the key given.\n"
    "   -c <signed-file> check signed file using the key given (if any)\n"
    "                    if no key file is specified, try .pub files in\n"
    "                    $KURV_KEYRING until we find a valid key.\n"
    "   -i         output the <key> used upon successful check.\n"
    "   -o         output the file contents upon successful check.\n"
    "   -d         detach signature from the signed file.\n"
    "\n"
    ;
#define SIG_START_LEN (sizeof(SIG_START)-1)
#define SIG_END_LEN   (sizeof(SIG_END)-1)

//
// Read exactly n bytes into buf.
// Return -1 on failure.
//
int read_exactly(uint8_t* buf, const size_t n, FILE* fp)
{
    if (fread(buf, 1, n, fp) != n) {
        err("kurv: fread");
        return -1;
    }
    return 0;
}

//
// Decode exactly n bytes
//
int decode_exactly(      uint8_t* buf, size_t bufsize,
                   const uint8_t* b64, size_t b64size)
{
    if (b64_validate(b64, b64size) != 0 || b64_decoded_size(b64, b64size) != bufsize)
        return -1;
    b64_decode(buf, b64, b64size);
    return 0;
}

//
// Read entirety of fp into memory, set bufsize as
// appropriate, and return ptr to the buffer.
// Returns NULL if any errors occured.
//
uint8_t* read_file(FILE* fp, size_t* bufsize)
{
    size_t total = 0;           // total buffer size
    size_t size = READ_SIZE;    // current buffer size
    uint8_t* buf = malloc(size);

    if (buf == NULL) {
        err("kurv: malloc");
        return NULL;
    }

    size_t r = 0;
    size_t n;
    while (!feof(fp) && (n = fread(buf + total, sizeof(uint8_t), READ_SIZE, fp)) > 0) {
        total += n;
        if (size <= total) {
            r++;
            // realloc
            size += r * READ_SIZE;
            uint8_t* new = reallocarray(buf, size, sizeof(uint8_t));
            if (new == NULL) {
                err("kurv: realloc");
                free(buf);
                return NULL;
            }
            buf = new;
        }
    }

    // error occured reading file
    if (ferror(fp)) {
        err("kurv: fread");
        free(buf);
        return NULL;
    }

    *bufsize = total;
    return buf;
}

//
// Try to find the b64 signature in the buffer, write
// raw signature to signature, and write the new buffer
// size (w/o signature to &bufsize).
//
int find_signature(uint8_t signature[64], const uint8_t* buf, size_t* bufsize)
{
    size_t start_size = SIG_START_LEN,
           sig_size   = B64_SIG_SIZE,
           end_size   = SIG_END_LEN,
           total      = start_size + sig_size + end_size;

    uint8_t b64_sig[B64_SIG_SIZE];

    // Cannot find signature...
    size_t size = *bufsize;
    if (size < total
            || memcmp(SIG_END,   buf + size - end_size, end_size) != 0
            || memcmp(SIG_START, buf + size - total,  start_size) != 0)
        return -1;
    // Copy to b64_sig
    memcpy(b64_sig, buf + size - sig_size - end_size, sig_size);
    // Invalid signature encoding
    if (decode_exactly(signature, 64,
                       b64_sig, B64_SIG_SIZE) != 0)
        return -1;
    *bufsize = size - total;
    return 0;
}

//
// Try to find a public / private key, write raw key into key.
// Returns -1 if fail, 0 if success.
//
int find_key(uint8_t key[32], const uint8_t* buf, size_t bufsize)
{
    if (bufsize < B64_KEY_SIZE
            || decode_exactly(key, 32,
                              buf, B64_KEY_SIZE) != 0)
        return -1;
    return 0;
}

//
// Read from keyfile
//
int find_key_in_file(uint8_t key[32], FILE* fp)
{
    uint8_t b64_key[B64_KEY_SIZE];
    if (read_exactly(b64_key, B64_KEY_SIZE, fp) != 0
            || find_key(key, b64_key, sizeof(b64_key)) != 0) {
        crypto_wipe(b64_key, sizeof(b64_key));
        return -1;
    }
    return 0;
}

//
// Concatenate a with b, adding the NUL byte at the end.
// Input dst must have size >= a_size + b_size + 1.
//
void concat(char* dst, const char* a, size_t a_size,
                       const char* b, size_t b_size)
{
    memcpy(dst,          a, a_size);
    memcpy(dst + a_size, b, b_size);
    dst[a_size + b_size] = '\0';
}

//
// Check if a string src ends with suffix
//
int endswith(const char* src, const char* suffix)
{
    size_t src_size    = strlen(src),
           suffix_size = strlen(suffix);
    if (src_size < suffix_size)
        return -1;
    return memcmp(src + src_size - suffix_size, suffix, suffix_size);
}


//
// Needed for generate(...): version of fopen supporting flags
//
FILE* safe_fopen_w(char* fn, int o_mode)
{
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    int fd = open(fn, flags, o_mode);
    if (fd == -1) {
        err("open");
        return NULL;
    }
    return fdopen(fd, "w");
}

//
// Generates keypair at <base>.priv and <base>.pub
//
int generate(char* base)
{
    uint8_t sk[32],
            pk[32],
            b64_sk[B64_KEY_SIZE],
            b64_pk[B64_KEY_SIZE];
    // Try to generate a random key
    if (getrandom(sk, 32, 0) < 0)
        errdie("getrandom");

    crypto_sign_public_key(pk, sk);
    b64_encode(b64_sk, sk, 32);
    b64_encode(b64_pk, pk, 32);
    crypto_wipe(sk, 32);
    crypto_wipe(pk, 32);

    // Reserve enough space for .priv and .pub
    FILE* fp;
    size_t len = strlen(base);
    char* path = calloc(len + 5 + 1, sizeof(char));
    if (path == NULL)
        errdie("calloc");

    // Write private key
    concat(path, base, len, ".priv", 5);
    fp = safe_fopen_w(path, S_IWUSR | S_IRUSR | S_IRGRP);
    if (fp == NULL
            || fwrite(b64_sk, 1, sizeof(b64_sk), fp) != sizeof(b64_sk)
            || fwrite("\n", 1, 1, fp) < 0)
        errdie("cannot write private key");
    crypto_wipe(b64_sk, sizeof(b64_sk));
    fclose(fp);

    // Write public key
    concat(path, base, len, ".pub", 4);
    fp = safe_fopen_w(path, S_IWUSR | S_IRUSR | S_IRGRP | S_IROTH);
    if (fp == NULL
            || fwrite(b64_pk, 1, sizeof(b64_pk), fp) != sizeof(b64_pk)
            || fwrite("\n", 1, 1, fp) < 0)
        errdie("cannot write public key");
    crypto_wipe(b64_pk, sizeof(b64_pk));
    fclose(fp);

    free(path);
    return 0;
}


//
// Sign a given file stream with the given signature stream sk_fp.
//
int sign(FILE* fp, FILE* sk_fp)
{
    // Try to read sk
    // Make sure to crypto_wipe!
    uint8_t sk      [32],
            pk      [32],  // used when computing signature
            sig     [64],
            b64_sig [B64_SIG_SIZE];

    if (find_key_in_file(sk, sk_fp) == -1) {
        crypto_wipe(sk, 32);
        die("invalid private key.");
    }

    size_t msg_size;
    uint8_t* msg = read_file(fp, &msg_size);
    if (msg == NULL) {
        crypto_wipe(sk, 32);
        errdie("cannot read file");
    }

    crypto_sign_public_key(pk, sk);
    crypto_sign(sig,
                sk, pk,
                msg, msg_size);
    b64_encode(b64_sig, sig, 64);
    crypto_wipe(sk, 32);
    crypto_wipe(pk, 32);

    fwrite(msg,       sizeof(uint8_t), msg_size,        stdout);
    fwrite(SIG_START, sizeof(uint8_t), SIG_START_LEN,   stdout);
    fwrite(b64_sig,   sizeof(uint8_t), sizeof(b64_sig), stdout);
    fwrite(SIG_END,   sizeof(uint8_t), SIG_END_LEN,     stdout);
    free(msg);
    return 0;
}


//
// Check against keyring
//
int check_keyring(FILE* fp, int should_show_id, int should_show_og)
{
    // Check if KURV_KEYRING is set.
    char*  keyring_dir     = getenv("KURV_KEYRING");
    size_t keyring_dir_len = keyring_dir == NULL ? 0 : strlen(keyring_dir);
    if (keyring_dir == NULL || keyring_dir_len == 0)
        die("$KURV_KEYRING is not set.");

    // Read message first.
    uint8_t sig [64];
    size_t msg_size;
    uint8_t* msg = read_file(fp, &msg_size);
    if (msg == NULL)
        die("error reading file.");

    if (find_signature(sig, msg, &msg_size) < 0)
        die("cannot find / malformed signature.");

    // Allocate enough for FNAME + 1 + 1 (NUL byte + '/' if necessary)
    char* path = malloc(keyring_dir_len + NAME_MAX + 2);
    if (path == NULL)
        errdie("malloc");

    memcpy(path, keyring_dir, keyring_dir_len);
    if (keyring_dir[keyring_dir_len - 1] != '/') {
        path[keyring_dir_len] = '/';
        keyring_dir_len++;
    }

    // Find a matching key
    DIR* dir = opendir(keyring_dir);
    struct dirent *dp;
    if (dir == NULL)
        errdie("opendir");

    while ((dp = readdir(dir)) != NULL) {
        // Check that file ends with .pub
        if (strcmp(dp->d_name, ".") == 0
                || strcmp(dp->d_name, "..") == 0
                || endswith(dp->d_name, ".pub") != 0)
            continue;

        memcpy(path + keyring_dir_len,
               dp->d_name, strlen(dp->d_name) + 1); // copy over NUL byte as well.

        // Try to read public key files (ignoring errors)
        uint8_t pk[32];
        FILE* pk_fp = fopen(path, "r");

        if (pk_fp == NULL) continue;
        if (find_key_in_file(pk, pk_fp) != 0 || crypto_check(sig, pk, msg, msg_size) != 0) {
            fclose(pk_fp);
            continue;
        }

        // Found it
        if (should_show_id) printf("%s\n", dp->d_name);
        if (should_show_og) fwrite(msg, sizeof(char), msg_size, stdout);
        exit(0);
    }

    die("cannot find a signer.");
    exit(1);
}


//
// Check that a file is signed by a given pk_fp
//
int check(FILE* fp, FILE* pk_fp, char* pk_fn, int should_show_id, int should_show_og)
{
    // It's fine to not crypto_wipe in this function, we are
    // only dealing with public keys.
    uint8_t pk  [32],
            sig [64];

    if (find_key_in_file(pk, pk_fp) == -1)
        die("invalid public key.");

    size_t msg_size;
    uint8_t* msg = read_file(fp, &msg_size);
    if (msg == NULL)
        die("error reading file.");

    if (find_signature(sig, msg, &msg_size) < 0)
        die("cannot find / malformed signature.");

    if (crypto_check(sig,
                     pk,
                     msg, msg_size) != 0)
        die("invalid signature.");

    if (should_show_id) printf("%s\n", pk_fn);
    if (should_show_og) fwrite(msg, sizeof(char), msg_size, stdout);
    free(msg);
    return 0;
}


//
// Detach a signature from the file.
//
int detach(FILE* fp)
{
    size_t msg_size;
    uint8_t* msg = read_file(fp, &msg_size);
    if (msg == NULL)
        die("error reading file.");

    uint8_t sig[64]; // unusued
    if (find_signature(sig, msg, &msg_size) < 0)
        die("cannot find / malformed signature.");

    fwrite(msg, sizeof(uint8_t), msg_size, stdout);
    free(msg);
    return 0;
}


//
// Warn if user specified a .priv instead of .pub
// or vice versa.
//
void fopen_warn(char* fn, int is_priv)
{
    if (endswith(fn, is_priv ? ".priv" : ".pub") != 0) {
        if (is_priv) {
            fprintf(stderr, "kurv: warning: private key file doesn't end in .priv\n");
        } else {
            fprintf(stderr, "kurv: warning: public key file doesn't end in .pub\n");
        }
    }
}


//
// Utility for opening a file or dying
//
FILE* fopen_or_die(const char* ctx, const char* fn)
{
    if (strcmp(fn, "-") == 0)
        return stdin;
    FILE* fp = fopen(fn, "r");
    if (fp == NULL) {
        err("fopen");
        die("cannot open '%s' for %s.", fn, ctx);
    }
    return fp;
}


int main(int argc, char** argv)
{
    char* pk_fn  = "";   // neeed for should_show_id
    FILE* fp     = NULL;
    FILE* pk_fp  = NULL;
    FILE* sk_fp  = NULL;
    char* base   = NULL;
    char  action = '0';
    int should_show_id = 0;
    int should_show_og = 0;
    int c;
    while ((c = getopt(argc, argv, "hg:s:c:d:p:P:io")) != -1)
        switch (c) {
            default:  exit(1);
            case 'h': fwrite(USAGE, sizeof(char), sizeof(USAGE), stdout); exit(0); break;
            case 'g':
                action = 'g';
                base = optarg;
                break;
            case 's': action = 's'; fp = fopen_or_die("signing",  optarg); break;
            case 'c': action = 'c'; fp = fopen_or_die("checking", optarg); break;
            case 'd': action = 'd'; fp = fopen_or_die("detach", optarg);   break;
            case 'P':
                fopen_warn(optarg, 1);
                sk_fp = fopen_or_die("private key file", optarg);
                break;
            case 'p':
                fopen_warn(optarg, 0);
                pk_fp = fopen_or_die("public key file",  optarg);
                pk_fn = optarg;
                break;
            case 'i': should_show_id = 1; break;
            case 'o': should_show_og = 1; break;
        }

    int rv = 1;
    switch (action) {
        default: die("invalid usage. see kurv -h."); break;
        case 'g':
            rv = generate(base);
            break;
        case 's':
            if (sk_fp == NULL) die("no private key file specified.");
            if (fp == NULL)    die("no file specified.");
            rv = sign(fp, sk_fp);
            break;
        case 'c':
            if (fp == NULL) die("no file specified.");
            rv = (pk_fp == NULL)
                ? check_keyring(fp, should_show_id, should_show_og)
                : check(fp, pk_fp, pk_fn, should_show_id, should_show_og);
            break;
        case 'd':
            if (fp == NULL) die("no file specified.");
            rv = detach(fp);
            break;
    }
    // If all goes well we close everything and exit.
    if (fp != NULL)    fclose(fp);
    if (pk_fp != NULL) fclose(pk_fp);
    if (sk_fp != NULL) fclose(sk_fp);
    exit(rv);
}
