# Spotify periodic playback artifact: bounded hardware AES

Base: `c80ecc5b0c7c9a84c66480ab4dfa1f1c31faca7c`, the supplied archive.

## Finding and correction

`CDNAudioFile::readBytes()` decrypts each successful CDN range through
`CryptoMbedTLS::aesCTRXcrypt()`. Previously that wrapper passed the complete
256 KiB range into a single `mbedtls_aes_crypt_ctr()` call.

The supplied build enables `CONFIG_MBEDTLS_HARDWARE_AES`. Its compile database
names `components/mbedtls/port/aes/block/esp_aes.c`; its linker map confirms
`esp_aes_crypt_ctr` from that object. In ESP-IDF v4.3.5, that function acquires
the AES hardware spinlock via `portENTER_CRITICAL`, processes the entire input,
then releases the critical section. Interrupts on the calling core are masked
throughout. Another core trying to acquire the same hardware also spins in a
critical section. The caller here is the decoder on core 1; TLS is another user
of the shared AES hardware. This is a concrete unbounded critical-section
problem, independent of network throughput or PCM FIFO occupancy.

The patch limits each AES call to 1024 bytes (64 AES blocks). For a 256 KiB range,
it makes 256 bounded calls instead of one large call. The IV, byte offset, and
keystream block persist across calls. It neither sleeps nor drops/repeats data,
and allocates no extra audio buffers. HTTP size, sink retries, I2S configuration,
and visualization settings are unchanged.

This is a strong candidate for the periodic audible artifact, not proof that
every reported artifact has this cause. Hardware duration and clean playback
have not been measured here. Header completion precedes body reading and AES;
the available log correlation alone cannot locate the audible fault precisely.

Driver source examined:
https://github.com/espressif/esp-idf/blob/v4.3.5/components/mbedtls/port/aes/block/esp_aes.c

## Verification

`tests/host/test_aes_ctr.py` extracts and compiles the actual production method.
An instrumented mbedTLS-compatible shim uses OpenSSL AES for the primitive;
OpenSSL EVP independently supplies expected CTR output. This is a host test,
not execution of the ESP32 driver or a full firmware build.

57 cases pass with AddressSanitizer and UndefinedBehaviorSanitizer:

- 128/192/256-bit keys; context reuse and key changes;
- zero-length, partial AES blocks, 1 KiB boundaries, 64 KiB and 256 KiB lengths;
- counter carry and the final IV;
- decryption of 256 KiB ranges at nonzero aligned file offsets;
- hardware/key error propagation without processing the remaining input;
- maximum hardware-call length of 1024 bytes.

The same test against the original method fails at input length 1025 because
it is passed as one hardware call. The test enforces the new bound; it does not
reproduce an audible glitch on the host.

Run from the repository root (requires g++ and OpenSSL development headers):

```sh
python3 tests/host/test_aes_ctr.py
```

In a restricted container where LeakSanitizer cannot inspect `/proc`, use
`ASAN_OPTIONS=detect_leaks=0`; address and undefined-behavior checks remain on.

## Build and the one remaining hardware check

The current execution environment has neither the ESP-IDF toolchain nor
Docker. The uploaded old binary is NOT a patched binary and must not be used
to assess this change. Build the changed source in the existing IDF 4.3.5
environment, then flash as before:

```sh
idf.py build
esptool.py --port /dev/ttyUSB0 write_flash 0x150000 build/squeezelite.bin
```

Listen to the same previously affected song for a few minutes, covering several
CDN refills. No new diagnostic logging or buffer experiments are needed for this
first check. Host tests establish byte correctness and bounded calls; only the
device can confirm disappearance of the audible artifact.

## Upstream status checked on 2026-09-29

The archive is not current with official `master-v4.3`. The official tip observed
was `1d0ac7fb27d121beca5717cc607c880efd73836a`; today's functional change is
`df6348e20c8516846e22ee32f071edabc7a51d44` (better handling of trackId changes).
The uploaded `Shim.cpp` still uses `lastTrackId` and calls
`notifyAudioReachedPlayback()` without an identifier, so that change and its
identifier-aware queue prerequisites are absent. Those are separate queue and
track-transition fixes; they are not silently merged into this focused patch.

https://github.com/sle118/squeezelite-esp32/commit/df6348e20c8516846e22ee32f071edabc7a51d44

The older HTTP fix `5034b8fe` and historical I2S/output fixes cited in the handoff
are present in the archive's ancestry, although later local changes alter some
of their behavior. Presence in Git history is not a claim of exact source parity.
