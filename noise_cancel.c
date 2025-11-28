#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include "spectralgate.h"

static volatile int running = 1;
static audio_context *global_ctx = NULL;

static void signal_handler(int signum) {
    running = 0;
    if (global_ctx) {
        cleanup_audio(global_ctx);
    }
    exit(0);
}

#define SAMPLE_RATE 44100
#define CHANNELS 1
#define NOISE_SECONDS 2  // Seconds of noise to sample for profile

typedef struct {
    pa_simple *capture;
    pa_simple *playback;
    float *buffer;
    float *output_buffer;
    SpectralGate *sg;
    size_t buffer_frames;
    bool noise_profile_computed;
} audio_context;

static int setup_audio(audio_context *ctx) {
    int error;
    
    // Initialize SpectralGate
    ctx->sg = spectralgate_create(SAMPLE_RATE);
    ctx->buffer_frames = ctx->sg->n_fft;  // Use FFT size as buffer size
    ctx->buffer = (float*)malloc(ctx->buffer_frames * sizeof(float));
    ctx->output_buffer = (float*)malloc(ctx->buffer_frames * sizeof(float));
    ctx->noise_profile_computed = false;

    if (!ctx->buffer || !ctx->output_buffer) {
        fprintf(stderr, "Cannot allocate buffers\n");
        return -1;
    }

    // Set up audio format
    pa_sample_spec ss = {
        .format = PA_SAMPLE_FLOAT32,
        .rate = SAMPLE_RATE,
        .channels = CHANNELS
    };

    // Create virtual source using module-pipe-source
    system("pactl load-module module-pipe-source source_name=noise_cancelled "
           "file=/tmp/noise_cancelled format=float32le rate=44100 channels=1 "
           "source_properties=device.description=NoiseCancel");

    // Open capture stream (default input device)
    ctx->capture = pa_simple_new(NULL, "NoiseCancel", PA_STREAM_RECORD,
                                NULL, "Input", &ss, NULL, NULL, &error);
    if (!ctx->capture) {
        fprintf(stderr, "Failed to create capture stream: %s\n",
                pa_strerror(error));
        return -1;
    }

    // Open pipe for writing
    int fd = open("/tmp/noise_cancelled", O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open pipe for writing\n");
        return -1;
    }
    close(fd);  // We'll let PulseAudio handle the actual writing

    // Open playback stream to the pipe source
    ctx->playback = pa_simple_new(NULL, "NoiseCancel", PA_STREAM_PLAYBACK,
                                 "noise_cancelled", "Output", &ss,
                                 NULL, NULL, &error);
    if (!ctx->playback) {
        fprintf(stderr, "Failed to create playback stream: %s\n",
                pa_strerror(error));
        return -1;
    }

    return 0;
}

static void cleanup_audio(audio_context *ctx) {
    if (ctx->capture)
        pa_simple_free(ctx->capture);
    if (ctx->playback)
        pa_simple_free(ctx->playback);
    if (ctx->buffer)
        free(ctx->buffer);
    if (ctx->output_buffer)
        free(ctx->output_buffer);
    if (ctx->sg)
        spectralgate_destroy(ctx->sg);
    
    // Find and remove the pipe-source module
    FILE *fp = popen("pactl list modules | grep -B 2 noise_cancelled | grep Module | cut -d '#' -f 2", "r");
    if (fp) {
        char module_id[32];
        if (fgets(module_id, sizeof(module_id), fp)) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "pactl unload-module %s", module_id);
            system(cmd);
        }
        pclose(fp);
    }
}

static int compute_noise_profile(audio_context *ctx) {
    printf("Computing noise profile... Please be quiet for %d seconds.\n", NOISE_SECONDS);
    
    size_t noise_samples = SAMPLE_RATE * NOISE_SECONDS;
    float *noise_buffer = (float*)malloc(noise_samples * sizeof(float));
    if (!noise_buffer) {
        fprintf(stderr, "Cannot allocate noise buffer\n");
        return -1;
    }

    int error;
    if (pa_simple_read(ctx->capture, noise_buffer, 
                       noise_samples * sizeof(float), &error) < 0) {
        fprintf(stderr, "Failed to read noise profile: %s\n",
                pa_strerror(error));
        free(noise_buffer);
        return -1;
    }

    spectralgate_compute_noise_thresh(ctx->sg, noise_buffer, noise_samples);
    free(noise_buffer);
    
    printf("Noise profile computed. Starting noise cancellation...\n");
    return 0;
}

int main() {
    audio_context ctx = {0};
    int error;
    
    // Set up signal handling
    global_ctx = &ctx;
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    if (setup_audio(&ctx) < 0) {
        cleanup_audio(&ctx);
        return 1;
    }

    // Compute initial noise profile
    if (compute_noise_profile(&ctx) < 0) {
        cleanup_audio(&ctx);
        return 1;
    }
    ctx.noise_profile_computed = true;

    printf("Virtual device 'noise_cancelled' created.\n");
    printf("To use it, select 'Null Output (noise_cancelled)' as your input source.\n");

    while (running) {
        // Read from capture device
        if (pa_simple_read(ctx.capture, ctx.buffer,
                          ctx.buffer_frames * sizeof(float), &error) < 0) {
            fprintf(stderr, "Read failed: %s\n", pa_strerror(error));
            break;
        }

        // Apply spectral gate
        spectralgate_process(ctx.sg, ctx.buffer, ctx.output_buffer, ctx.buffer_frames);

        // Write to virtual device
        if (pa_simple_write(ctx.playback, ctx.output_buffer,
                           ctx.buffer_frames * sizeof(float), &error) < 0) {
            fprintf(stderr, "Write failed: %s\n", pa_strerror(error));
            break;
        }
    }

    cleanup_audio(&ctx);
    return 0;
}