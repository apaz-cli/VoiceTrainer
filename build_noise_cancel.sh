
sudo apt-get install -y libpulse-dev libfftw3-dev
gcc -o noise_cancel noise_cancel.c -lpulse-simple -lpulse -lfftw3f -lm
