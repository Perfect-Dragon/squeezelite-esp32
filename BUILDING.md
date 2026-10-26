# Building and flashing this fork

Everything builds inside Docker with ESP-IDF 4.4.8 (the same image upstream CI uses),
so nothing needs to be installed except Docker. The ESP-IDF 6.x install in
`~/.espressif` is **not** used: this code only builds with ESP-IDF 4.x.

One-time setup (already done on this machine):

```bash
docker pull sle118/squeezelite-esp32-idfv448:4.4.8
```

## Boards

| Board | Command | Settings file | Build folder |
|---|---|---|---|
| ESP32-WROVER-E (4MB flash layout) | `./docker-build.sh wrover` | `sdkconfig.wrover` | `build-wrover/` |
| ESP32-S3-DevKitC-1-N32R16V | `./docker-build.sh s3` | `sdkconfig.s3` | `build-s3/` |

`sdkconfig.s3` is created from `build-scripts/S3-N32R16V-sdkconfig.defaults` the first time.
Delete `sdkconfig.s3` to regenerate it after changing that defaults file.

Do not run a plain `idf.py build` in the project folder: it creates `./sdkconfig`, which
confuses ESP-IDF 4.4 when building the other board (the script refuses to run if it exists).

## Everyday commands

```bash
./docker-build.sh s3                                   # build
./docker-build.sh s3 -p /dev/ttyUSB0 flash             # build + flash
./docker-build.sh s3 -p /dev/ttyUSB0 monitor           # serial log, Ctrl+] to quit
./docker-build.sh s3 -p /dev/ttyUSB0 flash monitor     # both
./docker-build.sh s3 menuconfig                        # change build settings
./docker-build.sh s3 -p /dev/ttyUSB0 erase_flash       # wipe everything, incl. Wi-Fi & settings
```

To build with your own Spotify client id/secret instead of the one in
`components/spotify/client_info.h`, set it in your shell first (not in a file) and
reconfigure once, as it is read when CMake configures:

```bash
read -rs -p "Spotify client id: " ID; echo; read -rs -p "Spotify client secret: " SECRET; echo
export SPOTIFY_SECRET="-DCLIENT_ID=\"$ID\" -DCLIENT_SECRET=\"$SECRET\""
./docker-build.sh s3 reconfigure build
```

Use the board's **UART** micro-USB port (CP2102N, shows up as `/dev/ttyUSB0`).

## S3 wiring (N32R16V)

GPIO 26-37 are used by the octal flash/PSRAM and must not be wired.
Also avoid 0, 3, 45, 46 (boot straps), 19/20 (USB), 43/44 (serial console), 47/48.

| Signal | GPIO |
|---|---|
| Display MOSI / SCLK / CS | 11 / 12 / 10 (SPI2 IOMUX pins, allows high SPI clock) |
| Display DC / RST / backlight | 9 / 14 / 21 |
| PCM5102 BCK / LRCK(WS) / DIN | 4 / 5 / 6 |

The display runs at 40 MHz (`speed=40000000` in `display_config`). If the picture is
garbled with long jumper wires, lower it (e.g. 26 MHz) in the web UI or with menuconfig.

## Notes

- The WROVER partition table changed (recovery 0x150000, app 0x290000) because the
  ESP-IDF 4.4.8 recovery image no longer fitted. Flash it over USB once; OTA cannot
  change the partition table. Settings (NVS) stay where they were.
- `CONFIG_I2S_SWAP_LR` (menuconfig > Squeezelite-ESP32 > Audio settings) swaps L/R
  for the DAC. It is on by default; turn it off if channels come out reversed on the S3.
