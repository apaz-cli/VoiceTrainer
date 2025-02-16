#!/bin/sh

# Install packages if they aren't already installed

PACKAGES="libportaudio2 libportaudiocpp0 libaubio-dev libsndfile1-dev libfftw3-dev"
LIBS="-lportaudio -laubio -lsndfile -lfftw3f -lm"
if ! dpkg -s $PACKAGES > /dev/null 2>&1; then
    echo "Installing dependencies..."
    sudo apt install -y $PACKAGES > /dev/null
fi

# gcc -o voicetrainer voice.c -lm -lpthread -lrt -lasound -lportaudio -laubio -lsndfile -lfftw3f -lfftw3
gcc voice.c -o voice $LIBS -fsanitize=address
