#!/bin/bash
# Build/flash this project inside the ESP-IDF 4.4.8 docker image.
#
#   ./docker-build.sh wrover                       build for the ESP32-WROVER-E
#   ./docker-build.sh s3                           build for the ESP32-S3-WROOM-2 N32R16V
#   ./docker-build.sh s3 -p /dev/ttyUSB0 flash     build + flash
#   ./docker-build.sh s3 -p /dev/ttyUSB0 monitor   serial monitor (Ctrl+] to quit)
#   ./docker-build.sh s3 menuconfig                change settings (saved in sdkconfig.s3)
#
# Anything after the board name is passed to idf.py (default: build).
# Each board has its own build folder and sdkconfig, so both can coexist:
#   wrover: build-wrover/  sdkconfig.wrover
#   s3:     build-s3/      sdkconfig.s3 (created from build-scripts/S3-N32R16V-sdkconfig.defaults)

set -e

IMAGE=sle118/squeezelite-esp32-idfv448:4.4.8
BOARD=$1
shift || true
[ $# -eq 0 ] && set -- build

case "$BOARD" in
    wrover)
        TARGET=esp32
        IDF_ARGS="-B build-wrover -DSDKCONFIG=sdkconfig.wrover"
        ;;
    s3)
        TARGET=esp32s3
        IDF_ARGS="-B build-s3 -DSDKCONFIG=sdkconfig.s3 -DSDKCONFIG_DEFAULTS=build-scripts/S3-N32R16V-sdkconfig.defaults"
        ;;
    *)
        sed -n '2,14p' "$0"
        exit 1
        ;;
esac

cd "$(dirname "$0")"

# idf.py 4.4 always cross-checks the chip against ./sdkconfig, whatever SDKCONFIG says,
# so a stray ./sdkconfig (e.g. from a plain "idf.py build") breaks the other board
if [ -f sdkconfig ]; then
    echo "A ./sdkconfig file exists. Board settings now live in sdkconfig.wrover / sdkconfig.s3."
    echo "Move or delete ./sdkconfig, then run this script again."
    exit 1
fi

# idf.py takes the chip from IDF_TARGET (a -DIDF_TARGET argument would be overridden)
DOCKER_ARGS=(--rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e IDF_TARGET=$TARGET
             -e CCACHE_DIR=/ccache -v "$HOME/.cache/squeezelite-ccache:/ccache"
             -v "$PWD:/project" -w /project)
mkdir -p "$HOME/.cache/squeezelite-ccache"

# Spotify client id/secret, read by components/spotify/CMakeLists.txt at configure time:
#   export SPOTIFY_SECRET='-DCLIENT_ID="..." -DCLIENT_SECRET="..."'
# "-e NAME" without a value copies it from this shell, so it never appears on a command line.
# Without it, components/spotify/client_info.h (upstream's) is used.
[ -n "$SPOTIFY_SECRET" ] && DOCKER_ARGS+=(-e SPOTIFY_SECRET)

# give the container access to any serial port that is plugged in
for dev in /dev/ttyUSB* /dev/ttyACM*; do
    [ -e "$dev" ] && DOCKER_ARGS+=(--device "$dev" --group-add "$(stat -c %g "$dev")")
done
[ -t 0 ] && DOCKER_ARGS+=(-it)

exec docker run "${DOCKER_ARGS[@]}" "$IMAGE" bash -c \
    ". \$IDF_PATH/export.sh >/dev/null 2>&1 && idf.py $IDF_ARGS -DDEPTH=16 $*"
