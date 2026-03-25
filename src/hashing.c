#include <openssl/sha.h>

// Hash function from bytos to bytes
void hash(const unsigned char *input, unsigned char *output)
{
    unsigned char hash[SHA512_DIGEST_LENGTH];
    SHA512((unsigned char *)input, 4096, hash);
    memcpy(output, hash, SHA512_DIGEST_LENGTH);
}
