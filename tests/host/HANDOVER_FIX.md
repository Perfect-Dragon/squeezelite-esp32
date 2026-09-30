# AirPlay allocation failure and external-source isolation

Apply this follow-up on the supplied c80ecc5b source, including the previous
AES fix. It does not modify or include that AES fix.

## Crash

The backtrace points to `http_parse`, `(*body)[*len] = '\0'`. The parser checked
allocation failure only after this write. EXCVADDR = 0x2c000 is consistent with
a null body pointer plus a Content-Length of 180224 bytes. Large artwork is a
possible request body, but the capture does not establish its content type.

The parser now rejects allocation failure before touching the body. Invalid or
overflowing lengths and truncated bodies return failure rather than being
passed to the RTSP command handler. The existing caller frees the body/headers
and closes/cleans up that AirPlay session. This prevents the identified reboot;
allocation failure still ends the AirPlay session rather than preserving it.

## Source ownership

The capture shows Spotify START/PLAY rejected because AirPlay owns the output.
The old shared PCM callback nevertheless accepted data from any external source
as long as output.external was nonzero. Spotify could therefore write PCM into
AirPlay's FIFO. This is a separate confirmed code defect, not the faulting store
in the backtrace.

Each PCM callback now supplies its source identity. Ownership is checked under
the output mutex and rechecked after every backpressure wait. A previous owner
cannot write to or realign the new owner's FIFO. Rejected Spotify bytes return
zero, so the existing TrackPlayer retains them instead of dropping them. This
patch enforces isolation; it does not implement automatic source takeover or
retry rejected START/PLAY commands. A connection may need to be selected again
once the previous source releases the output.

## TLS timeout and track skipping

The attached capture shows successful TCP/TLS connections, then repeated
3-second waits for HTTP response headers. Two HTTP attempts inside each of two
CDN attempts produce four timeouts over roughly 14.5 seconds. Exhaustion causes
CDNAudioFile::readBytes to return zero; TrackPlayer logs EOF and advances. The
next track later recovers from a similar timeout sequence and continues.

The log cannot establish why headers failed to arrive. This patch does not
change transport timeouts/retries or claim to fix that server/network failure.
A future recovery change should distinguish read failure from real EOF and
define cancellable retry/recovery for the same track without duplicate PCM.

## Verification and installation

`ASAN_OPTIONS=detect_leaks=0 python3 tests/host/test_handover.py` passes 23 host
cases under AddressSanitizer and UndefinedBehaviorSanitizer, compiling the
actual production parser, sink handlers and buffer helpers with controlled I/O.
The cases cover the 180224-byte allocation failure, a successful body of that
size, truncation/invalid lengths, all source/owner combinations, a handover while
waiting, and a handover on the final retry. Output tests use 16-bit stereo, as
in the supplied device build. These tests do not emulate FreeRTOS scheduling.

Apply from the repository root, then rebuild in the usual IDF environment:

```sh
git apply --check ~/Downloads/airplay-handover-fix.patch
git apply ~/Downloads/airplay-handover-fix.patch
idf.py build
esptool.py --port /dev/ttyUSB0 write_flash 0x150000 build/squeezelite.bin
```

No firmware build or physical ESP32 test was possible in this environment.
Continue the longer AES playback test first if desired. After applying this
separate patch, check AirPlay-to-Spotify switching during normal use.
