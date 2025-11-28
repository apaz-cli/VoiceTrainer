#!/bin/sh

# Define package names for different distros
DEBIAN_PACKAGES="libportaudio2 libportaudiocpp0 libaubio-dev libsndfile1-dev libfftw3-dev"
ARCH_PACKAGES="portaudio aubio libsndfile fftw"

CFLAGS="-O3 -march=native"
DEBUG_FLAGS="-fsanitize=address -g -fsanitize=undefined -fno-omit-frame-pointer"
LIBS="-lportaudio -laubio -lsndfile -lfftw3f -lm"

# Detect package manager and install dependencies
if command -v pacman >/dev/null 2>&1; then
    # Arch Linux
    if ! pacman -Qs $ARCH_PACKAGES >/dev/null 2>&1; then
        echo "Installing dependencies..."
        sudo pacman -S --needed --noconfirm $ARCH_PACKAGES
    fi
elif command -v apt >/dev/null 2>&1; then
    # Debian/Ubuntu
    if ! dpkg -s $DEBIAN_PACKAGES >/dev/null 2>&1; then
        echo "Installing dependencies..."
        sudo apt install -y $DEBIAN_PACKAGES
    fi
else
    echo "Unsupported package manager. Please install dependencies manually:"
    echo "Required: $LIBS"
    exit 1
fi

if [ ! -f voice ]; then
    echo "gcc voice.c -o voice $LIBS $CFLAGS"
    gcc voice.c -o voice $LIBS $CFLAGS #$DEBUG_FLAGS
else
    echo "Binary exists, skipping build."
fi

if [ "$1" = "run" ]; then
    shift 1
    ./voice "$@"
fi
