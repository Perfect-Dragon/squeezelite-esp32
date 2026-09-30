# Channel mapping and local AirPlay screen

Base: Perfect-Dragon/squeezelite-esp32 minimal-streamer, 9c8a59f354a06ab83d2368f0c5914dc0fe91b0cd.
The AES and AirPlay handover fixes are already applied in that commit.

## Changes

- Restores the DAC left/right swap first added in abab85a9 and removed in
  c80ecc5b. The latest pushed output_i2s.c had no swap. This applies to normal
  I2S DAC output, after the visualizer receives the data, without changing the
  shared FIFO or S/PDIF channel order. It is a device-specific mapping based on
  the reported reversed output, not a universal change for every I2S DAC.
- AirPlay now uses the same local spectrum/title renderer as Spotify. Titles
  are horizontally centered in the upper 80-pixel area; longer titles scroll.
  The lower 160 pixels retain the visualizer on the 320x240 display.
- Adds an AirPlay progress bar beneath the title (y=64..69). Timing updates
  correct its position, PLAY starts interpolation, and FLUSH/STOP freeze or
  reset it. No bar is shown when duration is unknown. Sender-supplied title
  and progress metadata are required; live/system audio may supply neither.
- RTP progress calculation keeps milliseconds and handles 32-bit timestamp
  wrap for media shorter than half the RTP counter range (~13.5 hours).
  The old code already subtracted start from end, but rounded to whole seconds.
- Stops Spotify artwork downloads. AirPlay image bodies are consumed through
  a fixed 512-byte stack buffer, preserving RTSP framing without a large image
  allocation. Other RTSP bodies keep the previous allocation safety checks.
- Explicitly starts the local visualizer on source setup. It no longer enables
  the legacy AirPlay renderer, which used to take display ownership away.

## Apply

From the repository directory:

```sh
git apply --check ~/Downloads/channels-airplay-display.patch
git apply ~/Downloads/channels-airplay-display.patch
```

Build and flash in the usual Docker/ESP-IDF environment:

```sh
idf.py build
esptool.py --port /dev/ttyUSB0 write_flash 0x150000 build/squeezelite.bin
```

## Validation

Host tests compile extracted production functions with hardware/display stubs:

```sh
ASAN_OPTIONS=detect_leaks=0 python3 tests/host/test_local_display.py
ASAN_OPTIONS=detect_leaks=0 python3 tests/host/test_handover.py
```

Tests cover 16/32-bit channel swapping, S/PDIF bypass, silence, preservation of
other frames/FIFO, RTP precision/wrap/invalid data, title centering/scrolling,
progress pause/resume/seek/end/tick wrap, and allocation-free image draining.
AddressSanitizer and UndefinedBehaviorSanitizer are enabled. The follow-up patch
was checked against the latest pushed source. No full ESP-IDF build or physical
DAC/display test was possible here. After flashing, check a left/right test and
an AirPlay song whose sender supplies metadata, including pause and seek.

## Buffer tradeoffs

At 44.1 kHz stereo 16-bit, PCM consumes 176400 bytes/s. The 2048000-byte output
allocation can hold about 11.6 seconds. This protects against download stalls
but reserves heap that other components may need and lets decoding run well
ahead of playback. Track-boundary notification must account for that lead.
Pause/seek need not wait for the entire buffer: that depends on stop/flush logic.

A 256 KiB compressed cache holds about 13.1 seconds at nominal 160 kbit/s (VBR
varies). It reduces request frequency but needs a contiguous allocation and
consumes more memory during concurrent source/network activity. Free memory
in aggregate does not guarantee a sufficiently large free block. With bounded
AES calls, the cache size no longer determines one giant AES critical section.

This patch leaves buffer sizes unchanged. Removing unused artwork is the first
resource improvement; reducing audio buffers should be a separate measured
change after playback and handover are stable.
