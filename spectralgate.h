#ifndef SPECTRALGATE_H
#define SPECTRALGATE_H

#include <complex.h>
#include <fftw3.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_N_FFT 1024
#define DEFAULT_HOP_LENGTH 256
#define DEFAULT_WIN_LENGTH 1024
#define DEFAULT_N_STD_THRESH 1.5f
#define DEFAULT_PROP_DECREASE 1.0f

typedef struct {
    int n_fft;
    int hop_length;
    int win_length;
    float n_std_thresh;
    float prop_decrease;
    int sample_rate;
    bool clip_noise;
    
    // FFTW plans and buffers
    fftwf_plan forward_plan;
    fftwf_plan inverse_plan;
    float *window;
    
    // Work buffers
    float *input_buffer;
    fftwf_complex *fft_buffer;
    float *magnitude_buffer;
    float *phase_buffer;
    
    // Noise profile
    float *noise_thresh;  // Frequency-domain threshold derived from noise
} SpectralGate;

// Create Hann window
static float* create_hann_window(int size) {
    float *window = (float*)malloc(size * sizeof(float));
    for (int i = 0; i < size; i++) {
        window[i] = 0.5f * (1.0f - cosf(2.0f * M_PI * i / (size - 1)));
    }
    return window;
}

SpectralGate* spectralgate_create(int sample_rate) {
    SpectralGate *sg = (SpectralGate*)calloc(1, sizeof(SpectralGate));
    
    // Initialize parameters with defaults
    sg->n_fft = DEFAULT_N_FFT;
    sg->hop_length = DEFAULT_HOP_LENGTH;
    sg->win_length = DEFAULT_WIN_LENGTH;
    sg->n_std_thresh = DEFAULT_N_STD_THRESH;
    sg->prop_decrease = DEFAULT_PROP_DECREASE;
    sg->sample_rate = sample_rate;
    sg->clip_noise = true;
    
    // Create window function
    sg->window = create_hann_window(sg->win_length);
    
    // Allocate work buffers
    sg->input_buffer = (float*)fftwf_malloc(sg->n_fft * sizeof(float));
    sg->fft_buffer = (fftwf_complex*)fftwf_malloc((sg->n_fft/2 + 1) * sizeof(fftwf_complex));
    sg->magnitude_buffer = (float*)malloc((sg->n_fft/2 + 1) * sizeof(float));
    sg->phase_buffer = (float*)malloc((sg->n_fft/2 + 1) * sizeof(float));
    sg->noise_thresh = (float*)calloc(sg->n_fft/2 + 1, sizeof(float));
    
    // Create FFTW plans
    sg->forward_plan = fftwf_plan_dft_r2c_1d(sg->n_fft, sg->input_buffer, sg->fft_buffer, FFTW_ESTIMATE);
    sg->inverse_plan = fftwf_plan_dft_c2r_1d(sg->n_fft, sg->fft_buffer, sg->input_buffer, FFTW_ESTIMATE);
    
    return sg;
}

void spectralgate_destroy(SpectralGate *sg) {
    if (!sg) return;
    
    fftwf_destroy_plan(sg->forward_plan);
    fftwf_destroy_plan(sg->inverse_plan);
    fftwf_free(sg->input_buffer);
    fftwf_free(sg->fft_buffer);
    free(sg->magnitude_buffer);
    free(sg->phase_buffer);
    free(sg->noise_thresh);
    free(sg->window);
    free(sg);
}

static void apply_window(float *buffer, const float *window, int size) {
    for (int i = 0; i < size; i++) {
        buffer[i] *= window[i];
    }
}

// Compute noise threshold from noise sample
void spectralgate_compute_noise_thresh(SpectralGate *sg, float *noise_data, int noise_length) {
    int num_frames = 1 + (noise_length - sg->n_fft) / sg->hop_length;
    float *mean = (float*)calloc(sg->n_fft/2 + 1, sizeof(float));
    float *sq_mean = (float*)calloc(sg->n_fft/2 + 1, sizeof(float));
    
    // Process noise frames
    for (int frame = 0; frame < num_frames; frame++) {
        // Copy and window frame
        memset(sg->input_buffer, 0, sg->n_fft * sizeof(float));
        memcpy(sg->input_buffer, noise_data + frame * sg->hop_length, 
               sg->win_length * sizeof(float));
        apply_window(sg->input_buffer, sg->window, sg->win_length);
        
        // Forward FFT
        fftwf_execute(sg->forward_plan);
        
        // Accumulate magnitude statistics
        for (int i = 0; i < sg->n_fft/2 + 1; i++) {
            float mag = cabsf(sg->fft_buffer[i]);
            mean[i] += mag;
            sq_mean[i] += mag * mag;
        }
    }
    
    // Compute threshold as mean + n_std_thresh * std
    for (int i = 0; i < sg->n_fft/2 + 1; i++) {
        mean[i] /= num_frames;
        sq_mean[i] /= num_frames;
        float var = sq_mean[i] - mean[i] * mean[i];
        float std = sqrtf(var > 0 ? var : 0);
        sg->noise_thresh[i] = mean[i] + sg->n_std_thresh * std;
    }
    
    free(mean);
    free(sq_mean);
}

void spectralgate_process(SpectralGate *sg, float *input, float *output, int input_size) {
    int num_frames = 1 + (input_size - sg->n_fft) / sg->hop_length;
    
    // Zero output buffer
    memset(output, 0, input_size * sizeof(float));
    
    // Process frame by frame
    for (int frame = 0; frame < num_frames; frame++) {
        int frame_start = frame * sg->hop_length;
        
        // Copy and window input frame
        memset(sg->input_buffer, 0, sg->n_fft * sizeof(float));
        memcpy(sg->input_buffer, input + frame_start, sg->win_length * sizeof(float));
        apply_window(sg->input_buffer, sg->window, sg->win_length);
        
        // Forward FFT
        fftwf_execute(sg->forward_plan);
        
        // Apply spectral gating
        for (int i = 0; i < sg->n_fft/2 + 1; i++) {
            float mag = cabsf(sg->fft_buffer[i]);
            float phase = cargf(sg->fft_buffer[i]);
            
            // Apply threshold
            float mask = (mag > sg->noise_thresh[i]) ? 1.0f : sg->prop_decrease;
            if (sg->clip_noise && mask < 1.0f) mask = 0.0f;
            
            // Apply mask and reconstruct complex spectrum
            sg->fft_buffer[i] = (mag * mask) * (cosf(phase) + I * sinf(phase));
        }
        
        // Inverse FFT
        fftwf_execute(sg->inverse_plan);
        
        // Apply window and accumulate output
        apply_window(sg->input_buffer, sg->window, sg->win_length);
        for (int i = 0; i < sg->n_fft; i++) {
            if (frame_start + i < input_size) {
                output[frame_start + i] += sg->input_buffer[i] / sg->n_fft;
            }
        }
    }
    
    // Normalize for overlap-add
    float normalization = (float)sg->n_fft / (float)sg->hop_length / 2.0f;
    for (int i = 0; i < input_size; i++) {
        output[i] /= normalization;
    }
}

#endif // SPECTRALGATE_H