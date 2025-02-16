#!/bin/sh

PACKAGES="libportaudio2 libportaudiocpp0 libaubio-dev libsndfile1-dev libfftw3-dev"
CFLAGS="-O3 -march=native -fsanitize=address -g -fsanitize=undefined -fno-omit-frame-pointer"
LIBS="-lportaudio -laubio -lsndfile -lfftw3f -lm"


# Install packages if they aren't already installed
if ! dpkg -s $PACKAGES > /dev/null 2>&1; then
    echo "Installing dependencies..."
    sudo apt install -y $PACKAGES > /dev/null
fi

if [ ! -f voice ] || [ ! "$1" = "run" ]; then
    echo "Compiling..."
    gcc voice.c -o voice $LIBS $CFLAGS
fi

if [ "$1" = "run" ]; then
    ./voice
fi