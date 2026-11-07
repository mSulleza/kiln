#!/bin/sh
# Runs inside ubuntu:20.04 (arm64, glibc 2.31; the Brick has 2.33) and writes the
# libraries ports expect but stock firmware 1.1.1 lacks or has too old into $1:
#   libpng16  stock only has libpng12
#   libz      stock has zlib 1.2.8; this libpng16 needs 1.2.9+
#   libSDL2_mixer  stock is older than 2.0.2 (no Mix_OpenAudioDevice). Built with its
#             bundled decoders only (OGG, MP3, FLAC, WAV, MIDI) so it needs nothing else.
set -eu
OUT=$1
MIXER_VER=2.8.1
MIXER_SHA256=$2

apt-get update -qq
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends ca-certificates curl cmake make gcc \
	libsdl2-dev >/dev/null
mkdir -p "$OUT" /tmp/deb && cd /tmp/deb
apt-get download -qq libpng16-16 zlib1g
for d in *.deb; do dpkg-deb -x "$d" root; done
cp -L root/usr/lib/aarch64-linux-gnu/libpng16.so.16 root/lib/aarch64-linux-gnu/libz.so.1 "$OUT/"

cd /tmp
curl -fsSLO https://github.com/libsdl-org/SDL_mixer/releases/download/release-$MIXER_VER/SDL2_mixer-$MIXER_VER.tar.gz
echo "$MIXER_SHA256  SDL2_mixer-$MIXER_VER.tar.gz" | sha256sum -c -
tar xzf SDL2_mixer-$MIXER_VER.tar.gz
cmake -S SDL2_mixer-$MIXER_VER -B mixer -DCMAKE_BUILD_TYPE=Release -DSDL2MIXER_VENDORED=OFF \
	-DSDL2MIXER_SAMPLES=OFF -DSDL2MIXER_MOD=OFF -DSDL2MIXER_OPUS=OFF -DSDL2MIXER_WAVPACK=OFF \
	-DSDL2MIXER_GME=OFF -DSDL2MIXER_MIDI_FLUIDSYNTH=OFF -DSDL2MIXER_FLAC_LIBFLAC=OFF \
	-DSDL2MIXER_MP3_MPG123=OFF -DSDL2MIXER_VORBIS=STB >/dev/null
cmake --build mixer -j8 >/dev/null
cp -L mixer/libSDL2_mixer-2.0.so.0 "$OUT/"
strip "$OUT"/*.so*
