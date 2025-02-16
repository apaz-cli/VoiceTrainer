#include <portaudio.h>
#include <aubio/aubio.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stdbool.h>
#include <termios.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pwd.h>
#ifdef _WIN32
#include <windows.h>
#endif

// Include the spectral gate header
#include "spectralgate.h"

#define SAMPLE_RATE 44100
#define FRAMES_PER_BUFFER 512
#define CHANNELS 1
#define AUBIO_HOP_SIZE 512
#define AUBIO_BUFFER_SIZE 2048
#define MAX_PITCH_HISTORY 256
#define NOISE_SAMPLE_DURATION 1.0  // Duration in seconds to sample noise

typedef struct {
    float *recorded_data;
    size_t frames_count;
    size_t max_frames;
    // Pitch detection state
    aubio_pitch_t *pitch_detector;
    fvec_t *input_buffer;
    fvec_t *pitch_output;
    float pitch_history[MAX_PITCH_HISTORY];
    int pitch_history_count;
    size_t samples_processed;
    size_t last_display_update;
    // Noise reduction state
    float *noise_sample;
    size_t noise_frames;
    bool noise_captured;
} RecordingState;

static volatile bool should_stop = false;
static volatile bool is_calibrating = true;

// Previous utility functions remain the same
void handle_sigint(int signum) {
    printf("\033[?25h\n");
    #ifdef _WIN32
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        GetConsoleMode(hStdin, &mode);
        SetConsoleMode(hStdin, mode);
    #else
        struct termios term;
        tcgetattr(STDIN_FILENO, &term);
        term.c_lflag |= ICANON | ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &term);
    #endif
    printf("\nRecording cancelled\n");
    exit(1);
}

void draw_pitch_bar(float avg_pitch) {
    int bar_length = (int)(40.0f * (avg_pitch) / 400.0f);
    if (bar_length < 0) bar_length = 0;
    if (bar_length > 40) bar_length = 40;
    
    printf("\r");
    for (int i = 0; i < bar_length; i++) printf("█");
    for (int i = bar_length; i < 40; i++) printf("▒");
    printf(" %.1f Hz\033[K", avg_pitch);
    fflush(stdout);
}

static int recording_callback(const void *input,
                            void *output,
                            unsigned long frameCount,
                            const PaStreamCallbackTimeInfo *timeInfo,
                            PaStreamCallbackFlags statusFlags,
                            void *userData) {
    RecordingState *state = (RecordingState *)userData;
    const float *in = (const float *)input;
    
    if (is_calibrating) {
        // During calibration, store samples in noise buffer
        if (state->noise_frames + frameCount <= SAMPLE_RATE * NOISE_SAMPLE_DURATION) {
            memcpy(state->noise_sample + state->noise_frames, in, frameCount * sizeof(float));
            state->noise_frames += frameCount;
            return paContinue;
        } else {
            is_calibrating = false;
            state->noise_captured = true;
            printf("\nNoise profile captured. Recording started. Press Enter to stop.\n\n");
            return paContinue;
        }
    }
    
    // Expand buffer if needed
    if (state->frames_count + frameCount > state->max_frames) {
        size_t new_size = state->max_frames * 2;
        float *new_data = realloc(state->recorded_data, new_size * sizeof(float));
        if (!new_data) return paAbort;
        state->recorded_data = new_data;
        state->max_frames = new_size;
    }
    
    // Copy input data
    memcpy(state->recorded_data + state->frames_count, in, frameCount * sizeof(float));
    state->frames_count += frameCount;
    
    // Process pitch detection
    for (size_t i = 0; i < frameCount; i++) {
        state->input_buffer->data[state->samples_processed % AUBIO_HOP_SIZE] = in[i];
        state->samples_processed++;
        
        if (state->samples_processed % AUBIO_HOP_SIZE == 0) {
            aubio_pitch_do(state->pitch_detector, state->input_buffer, state->pitch_output);
            float pitch = state->pitch_output->data[0];
            float confidence = aubio_pitch_get_confidence(state->pitch_detector);
            
            if (confidence > 0.8f && pitch >= 50.0f && pitch <= 2000.0f) {
                if (state->pitch_history_count < MAX_PITCH_HISTORY) {
                    state->pitch_history[state->pitch_history_count++] = pitch;
                } else {
                    memmove(state->pitch_history, state->pitch_history + 1, 
                           (MAX_PITCH_HISTORY - 1) * sizeof(float));
                    state->pitch_history[MAX_PITCH_HISTORY - 1] = pitch;
                }
                
                if (state->samples_processed - state->last_display_update >= SAMPLE_RATE / 16) {
                    state->last_display_update = state->samples_processed;
                    float avg_pitch = 0;
                    for (int j = 0; j < state->pitch_history_count; j++) {
                        avg_pitch += state->pitch_history[j];
                    }
                    avg_pitch /= state->pitch_history_count;
                    draw_pitch_bar(avg_pitch);
                }
            }
        }
    }
    
    return should_stop ? paComplete : paContinue;
}

// Playback callback function
static int playback_callback(const void *input,
                           void *output,
                           unsigned long frameCount,
                           const PaStreamCallbackTimeInfo *timeInfo,
                           PaStreamCallbackFlags statusFlags,
                           void *userData) {
    float *out = (float*)output;
    float *data = (float*)userData;
    static size_t position = 0;
    
    for (unsigned long i = 0; i < frameCount; i++) {
        out[i] = data[position++];
    }
    
    return position < frameCount ? paContinue : paComplete;
}

char* get_voice_dir() {
    const char *home_dir;
    #ifdef _WIN32
        home_dir = getenv("USERPROFILE");
    #else
        home_dir = getenv("HOME");
        if (!home_dir) {
            struct passwd *pw = getpwuid(getuid());
            home_dir = pw->pw_dir;
        }
    #endif
    
    char *voice_dir = malloc(strlen(home_dir) + 7);
    sprintf(voice_dir, "%s/Voice", home_dir);
    return voice_dir;
}

void save_recording(const char *filename, float *data, size_t frames) {
    SF_INFO sfinfo = {
        .samplerate = SAMPLE_RATE,
        .channels = CHANNELS,
        .format = SF_FORMAT_WAV | SF_FORMAT_FLOAT
    };
    
    SNDFILE *file = sf_open(filename, SFM_WRITE, &sfinfo);
    if (!file) {
        fprintf(stderr, "Error opening output file: %s\n", sf_strerror(NULL));
        return;
    }
    
    sf_write_float(file, data, frames);
    sf_close(file);
}

void play_audio(float *data, size_t frames) {
    PaStream *stream;
    PaError err;
    
    PaStreamParameters outputParameters = {
        .device = Pa_GetDefaultOutputDevice(),
        .channelCount = CHANNELS,
        .sampleFormat = paFloat32,
        .suggestedLatency = Pa_GetDeviceInfo(Pa_GetDefaultOutputDevice())->defaultLowOutputLatency,
        .hostApiSpecificStreamInfo = NULL
    };
    
    err = Pa_OpenStream(&stream,
                       NULL,
                       &outputParameters,
                       SAMPLE_RATE,
                       FRAMES_PER_BUFFER,
                       paClipOff,
                       playback_callback,
                       data);
    
    if (err != paNoError) {
        fprintf(stderr, "Error opening playback stream: %s\n", Pa_GetErrorText(err));
        return;
    }
    
    printf("Playing back recording...\n");
    err = Pa_StartStream(stream);
    if (err != paNoError) {
        fprintf(stderr, "Error starting playback: %s\n", Pa_GetErrorText(err));
        return;
    }
    
    while (Pa_IsStreamActive(stream)) {
        Pa_Sleep(100);
    }
    
    Pa_CloseStream(stream);
}

int main() {
    char *voice_dir = get_voice_dir();
    #ifdef _WIN32
        _mkdir(voice_dir);
    #else
        mkdir(voice_dir, 0755);
    #endif
    
    PaError err = Pa_Initialize();
    if (err != paNoError) goto error;
    
    // Initialize recording state
    RecordingState state = {
        .recorded_data = malloc(SAMPLE_RATE * 60 * sizeof(float)),
        .frames_count = 0,
        .max_frames = SAMPLE_RATE * 60,
        .pitch_detector = new_aubio_pitch("yin", AUBIO_BUFFER_SIZE, AUBIO_HOP_SIZE, SAMPLE_RATE),
        .input_buffer = new_fvec(AUBIO_HOP_SIZE),
        .pitch_output = new_fvec(1),
        .pitch_history_count = 0,
        .samples_processed = 0,
        .last_display_update = 0,
        .noise_sample = malloc(SAMPLE_RATE * NOISE_SAMPLE_DURATION * sizeof(float)),
        .noise_frames = 0,
        .noise_captured = false
    };
    
    if (!state.recorded_data || !state.pitch_detector || 
        !state.input_buffer || !state.pitch_output || !state.noise_sample) {
        fprintf(stderr, "Failed to allocate resources\n");
        goto cleanup;
    }
    
    // Create spectral gate
    SpectralGate *sg = spectralgate_create(SAMPLE_RATE);
    if (!sg) {
        fprintf(stderr, "Failed to create spectral gate\n");
        goto cleanup;
    }
    
    // Set up stream parameters
    PaStream *stream;
    PaStreamParameters inputParameters = {
        .device = Pa_GetDefaultInputDevice(),
        .channelCount = CHANNELS,
        .sampleFormat = paFloat32,
        .suggestedLatency = Pa_GetDeviceInfo(Pa_GetDefaultInputDevice())->defaultLowInputLatency,
        .hostApiSpecificStreamInfo = NULL
    };
    
    err = Pa_OpenStream(&stream,
                       &inputParameters,
                       NULL,
                       SAMPLE_RATE,
                       FRAMES_PER_BUFFER,
                       paClipOff,
                       recording_callback,
                       &state);
    if (err != paNoError) goto error;
    
    signal(SIGINT, handle_sigint);
    
    printf("\033[?25l");
    printf("Please be quiet for %.1f seconds to capture noise profile...\n", NOISE_SAMPLE_DURATION);
    err = Pa_StartStream(stream);
    if (err != paNoError) goto error;
    
    // Set stdin to non-blocking mode
    #ifdef _WIN32
        HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
        DWORD mode;
        GetConsoleMode(hStdin, &mode);
        SetConsoleMode(hStdin, mode & ~ENABLE_ECHO_INPUT & ~ENABLE_LINE_INPUT);
    #else
        struct termios old_term, new_term;
        tcgetattr(STDIN_FILENO, &old_term);
        new_term = old_term;
        new_term.c_lflag &= ~(ICANON | ECHO);
        tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
        int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
        fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
    #endif

    fflush(stdout);
    
    char c;
    while (!should_stop) {
        if (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == '\n' || c == '\r') {
                should_stop = true;
            }
        }
        Pa_Sleep(10);
    }

    #ifdef _WIN32
        SetConsoleMode(hStdin, mode);
    #else
        tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
        fcntl(STDIN_FILENO, F_SETFL, flags);
    #endif
    
    Pa_StopStream(stream);
    Pa_CloseStream(stream);
    printf("\033[?25h");
    
    // Apply noise reduction
    printf("\nApplying noise reduction...\n");
    float *cleaned_audio = malloc(state.frames_count * sizeof(float));
    if (!cleaned_audio) {
        fprintf(stderr, "Failed to allocate memory for cleaned audio\n");
        goto cleanup;
    }
    
    // Compute noise threshold from captured noise sample
    spectralgate_compute_noise_thresh(sg, state.noise_sample, state.noise_frames);
    
    // Process the recorded audio
    spectralgate_process(sg, state.recorded_data, cleaned_audio, state.frames_count);
    
    // Save recording with timestamp
    time_t now;
    time(&now);
    char filename[512];
    strftime(filename, sizeof(filename), "/voice_sample_%Y%m%d-%H%M%S.wav", localtime(&now));
    char *full_path = malloc(strlen(voice_dir) + strlen(filename) + 1);
    sprintf(full_path, "%s%s", voice_dir, filename);

    // Save the file
    save_recording(full_path, cleaned_audio, state.frames_count);
    printf("Saved cleaned audio to: %s\n", full_path);
    
    // Play back the cleaned audio
    play_audio(cleaned_audio, state.frames_count);
    
    // Cleanup
    free(full_path);
    free(voice_dir);
    free(cleaned_audio);
    spectralgate_destroy(sg);
    
cleanup:
    // Clean up resources
    free(state.recorded_data);
    free(state.noise_sample);
    del_aubio_pitch(state.pitch_detector);
    del_fvec(state.input_buffer);
    del_fvec(state.pitch_output);
    Pa_Terminate();
    return 0;
    
error:
    fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
    goto cleanup;
}