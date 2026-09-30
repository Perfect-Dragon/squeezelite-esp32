#!/usr/bin/env python3
"""Host regression for the production AES wrapper (Python 3, g++, libssl-dev).

Extracts the actual method, not a rewritten copy. The mbedTLS shim implements
its CTR state API with OpenSSL AES; EVP is the independent expected-output
oracle. This checks bytes and hardware-call bounds, not ESP32 wall-clock timing.
Run --source with the pre-fix Crypto.cpp to reproduce the oversized call.
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser()
parser.add_argument('--source', type=Path, default=ROOT / 'components/spotify/cspot/bell/main/utilities/Crypto.cpp')
args = parser.parse_args()
source = args.source.read_text()
start = source.index('void CryptoMbedTLS::aesCTRXcrypt(')
brace = source.index('{', start)
depth = 1
end = brace + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
method = source[start:end]

PREAMBLE = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include <openssl/aes.h>
#include <openssl/evp.h>

using mbedtls_aes_context = AES_KEY;
static size_t maxCall, calls, failCall;
static bool failKey;
static void mbedtls_aes_init(mbedtls_aes_context*) {}
static int mbedtls_aes_setkey_enc(mbedtls_aes_context* ctx,
                                 const unsigned char* key, unsigned bits) {
    return failKey ? -1 : AES_set_encrypt_key(key, bits, ctx);
}
static int mbedtls_aes_crypt_ctr(mbedtls_aes_context* ctx, size_t length,
        size_t* off, unsigned char* iv, unsigned char* stream,
        const unsigned char* input, unsigned char* output) {
    maxCall = std::max(maxCall, length);
    if (++calls == failCall) return -1;
    size_t n = *off;
    while (length--) {
        if (!n) {
            AES_encrypt(iv, stream, ctx);
            for (int i = 15; i >= 0 && ++iv[i] == 0; --i) {}
        }
        *output++ = *input++ ^ stream[n];
        n = (n + 1) & 15;
    }
    *off = n;
    return 0;
}
class CryptoMbedTLS {
    mbedtls_aes_context aesCtx;
    bool aesCtxInitialized = false;
public:
    void aesCTRXcrypt(const std::vector<uint8_t>&, std::vector<uint8_t>&,
                     uint8_t*, size_t);
};
static std::vector<uint8_t> reference(const std::vector<uint8_t>& key,
        const std::vector<uint8_t>& iv, const std::vector<uint8_t>& input) {
    const EVP_CIPHER* cipher = key.size() == 16 ? EVP_aes_128_ctr() :
                              key.size() == 24 ? EVP_aes_192_ctr() : EVP_aes_256_ctr();
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    assert(ctx);
    assert(EVP_EncryptInit_ex(ctx, cipher, nullptr, key.data(), iv.data()) == 1);
    std::vector<uint8_t> result(input.size() + 16);
    int n = 0, tail = 0;
    assert(EVP_EncryptUpdate(ctx, result.data(), &n, input.data(), input.size()) == 1);
    assert(EVP_EncryptFinal_ex(ctx, result.data() + n, &tail) == 1);
    result.resize(n + tail);
    EVP_CIPHER_CTX_free(ctx);
    return result;
}
static void addBlocks(std::vector<uint8_t>& iv, size_t n) {
    for (int i = 15; i >= 0 && n; --i) {
        n += iv[i];
        iv[i] = n & 255;
        n >>= 8;
    }
}
'''

TESTS = r'''
int main() {
    CryptoMbedTLS crypto;
    size_t cases = 0;
    // Reuse the context, change keys, and force counter carry across bytes.
    for (size_t keySize : {16, 24, 32}) {
        std::vector<uint8_t> key(keySize);
        for (size_t i = 0; i < keySize; ++i) key[i] = i * 17 + 3;
        for (size_t length : {0, 1, 15, 16, 17, 1023, 1024, 1025,
                              2047, 2048, 2049, 65535, 65536, 262143, 262144, 262145}) {
            std::vector<uint8_t> iv(16, 255), data(length);
            iv[0] = 0x72;
            for (size_t i = 0; i < length; ++i) data[i] = (i * 37 + i / 251) & 255;
            const auto expected = reference(key, iv, data);
            auto expectedIV = iv;
            addBlocks(expectedIV, (length + 15) / 16);
            calls = maxCall = 0;
            crypto.aesCTRXcrypt(key, iv, data.data(), data.size());
            assert(data == expected);
            assert(iv == expectedIV);
            if (maxCall > 1024) {
                fprintf(stderr, "FAIL: %zu bytes in one hardware critical section (input=%zu)\n",
                        maxCall, length);
                return 1;
            }
            ++cases;
        }
    }
    // CDN seek/range IV = initial IV + aligned absolute file offset / 16.
    std::vector<uint8_t> key(16, 42), iv(16, 0), plaintext(600000);
    for (size_t i = 0; i < plaintext.size(); ++i) plaintext[i] = (i * 23 + i / 199) & 255;
    auto ciphertext = reference(key, iv, plaintext);
    for (size_t pos : {0, 160, 32752, 262128, 262144}) {
        auto rangeIV = iv;
        addBlocks(rangeIV, pos / 16);
        std::vector<uint8_t> range(ciphertext.begin() + pos, ciphertext.begin() + pos + 262144);
        crypto.aesCTRXcrypt(key, rangeIV, range.data(), range.size());
        assert(std::equal(range.begin(), range.end(), plaintext.begin() + pos));
        ++cases;
    }
    // Failure must propagate; no processing after a failed hardware chunk.
    for (size_t failingCall : {1, 2, 3}) {
        std::vector<uint8_t> data(4096, 0x55), nonce(16, 0);
        calls = 0;
        failCall = failingCall;
        bool threw = false;
        try { crypto.aesCTRXcrypt(key, nonce, data.data(), data.size()); }
        catch (const std::runtime_error&) { threw = true; }
        assert(threw && calls == failingCall);
        assert(std::all_of(data.begin() + (failingCall - 1) * 1024, data.end(),
                           [](uint8_t b) { return b == 0x55; }));
        ++cases;
    }
    failCall = 0;
    failKey = true;
    calls = 0;
    bool threw = false;
    try { crypto.aesCTRXcrypt(key, iv, plaintext.data(), plaintext.size()); }
    catch (const std::runtime_error&) { threw = true; }
    assert(threw && calls == 0);
    printf("PASS: %zu cases; byte-exact CTR, range offsets, error propagation, max hardware call <= 1024 bytes\n", ++cases);
}
'''

with tempfile.TemporaryDirectory(prefix='cspot-aes-test-') as tmp:
    cpp = Path(tmp) / 'test.cpp'
    binary = Path(tmp) / 'test'
    cpp.write_text(PREAMBLE + method + TESTS)
    subprocess.run(['g++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', str(cpp), '-lcrypto', '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
