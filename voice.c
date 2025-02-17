#include <aubio/aubio.h>
#include <fcntl.h>
#include <portaudio.h>
#include <pwd.h>
#include <signal.h>
#include <sndfile.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
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
#define NOISE_SAMPLE_DURATION 1.0 // Duration in seconds to sample noise

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
} RecordingState;

typedef struct {
  float *noise_data;
  size_t frames_count;
  size_t max_frames;
} NoiseState;

static volatile bool should_stop = false;

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
  int bar_length = (int)(40.0f * (avg_pitch) / 300.0f);
  if (bar_length < 0)
    bar_length = 0;
  if (bar_length > 40)
    bar_length = 40;

  printf("\r");
  for (int i = 0; i < bar_length; i++)
    printf("█");
  for (int i = bar_length; i < 40; i++)
    printf("▒");
  printf(" %.1f Hz\033[K", avg_pitch);
  fflush(stdout);
}

static int noise_callback(const void *input, void *output,
                          unsigned long frameCount,
                          const PaStreamCallbackTimeInfo *timeInfo,
                          PaStreamCallbackFlags statusFlags, void *userData) {
  NoiseState *state = (NoiseState *)userData;

  size_t remaining_space = state->max_frames - state->frames_count;
  size_t frames_to_copy =
      remaining_space < frameCount ? remaining_space : frameCount;

  if (frames_to_copy > 0) {
    memcpy(state->noise_data + state->frames_count, (const float *)input,
           frames_to_copy * sizeof(float));
    state->frames_count += frames_to_copy;
  }

  return (state->frames_count >= state->max_frames) ? paComplete : paContinue;
}

static int recording_callback(const void *input, void *output,
                              unsigned long frameCount,
                              const PaStreamCallbackTimeInfo *timeInfo,
                              PaStreamCallbackFlags statusFlags,
                              void *userData) {
  RecordingState *state = (RecordingState *)userData;
  const float *in = (const float *)input;

  // Expand buffer if needed
  if (state->frames_count + frameCount > state->max_frames) {
    size_t new_size = state->max_frames * 2;
    float *new_data = realloc(state->recorded_data, new_size * sizeof(float));
    if (!new_data)
      return paAbort;
    state->recorded_data = new_data;
    state->max_frames = new_size;
  }

  // Copy input data
  memcpy(state->recorded_data + state->frames_count, in,
         frameCount * sizeof(float));
  state->frames_count += frameCount;

  // Process pitch detection
  for (size_t i = 0; i < frameCount; i++) {
    state->input_buffer->data[state->samples_processed % AUBIO_HOP_SIZE] =
        in[i];
    state->samples_processed++;

    if (state->samples_processed % AUBIO_HOP_SIZE == 0) {
      aubio_pitch_do(state->pitch_detector, state->input_buffer,
                     state->pitch_output);
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

        if (state->samples_processed - state->last_display_update >=
            SAMPLE_RATE / 16) {
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

typedef struct {
  size_t total_frames;
  float *audio_data;
  size_t position;
} PlaybackData;

static int playback_callback(const void *input, void *output,
                             unsigned long frameCount,
                             const PaStreamCallbackTimeInfo *timeInfo,
                             PaStreamCallbackFlags statusFlags,
                             void *userData) {
  float *out = (float *)output;
  PlaybackData *pb_data = (PlaybackData *)userData;

  for (unsigned long i = 0; i < frameCount; i++) {
    if (pb_data->position < pb_data->total_frames) {
      out[i] = pb_data->audio_data[pb_data->position++];
    } else {
      out[i] = 0.0f; // Pad with silence if we run out of data
    }
  }

  if (pb_data->position >= pb_data->total_frames) {
    return paComplete;
  }

  return paContinue;
}

char *get_voice_dir() {
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

bool save_noise_profile(const char *voice_dir, float *noise_data,
                        size_t frames) {
  char filepath[4096];
  snprintf(filepath, sizeof(filepath), "%s/.noise_profile.dat", voice_dir);

  FILE *f = fopen(filepath, "wb");
  if (!f)
    return false;

  // Write number of frames first
  if (fwrite(&frames, sizeof(size_t), 1, f) != 1) {
    fclose(f);
    return false;
  }

  // Write noise data
  if (fwrite(noise_data, sizeof(float), frames, f) != frames) {
    fclose(f);
    return false;
  }

  fclose(f);
  return true;
}

bool load_noise_profile(const char *voice_dir, float **noise_data,
                        size_t *frames) {
  char filepath[4096];
  snprintf(filepath, sizeof(filepath), "%s/.noise_profile.dat", voice_dir);

  FILE *f = fopen(filepath, "rb");
  if (!f)
    return false;

  // Read number of frames
  if (fread(frames, sizeof(size_t), 1, f) != 1) {
    fclose(f);
    return false;
  }

  // Allocate and read noise data
  *noise_data = malloc(*frames * sizeof(float));
  if (!*noise_data) {
    fclose(f);
    return false;
  }

  if (fread(*noise_data, sizeof(float), *frames, f) != *frames) {
    free(*noise_data);
    fclose(f);
    return false;
  }

  fclose(f);
  return true;
}

void save_recording(const char *filename, float *data, size_t frames) {
  SF_INFO sfinfo = {.samplerate = SAMPLE_RATE,
                    .channels = CHANNELS,
                    .format = SF_FORMAT_WAV | SF_FORMAT_FLOAT};

  SNDFILE *file = sf_open(filename, SFM_WRITE, &sfinfo);
  if (!file) {
    fprintf(stderr, "Error opening output file: %s\n", sf_strerror(NULL));
    return;
  }

  sf_write_float(file, data, frames);
  sf_close(file);
}

void play_audio(float *data, size_t frames) {
  // Save and redirect stderr
  int stderr_fd = dup(STDERR_FILENO);
  freopen("/dev/null", "w", stderr);

  PlaybackData *pb_data = malloc(sizeof(PlaybackData));
  if (!pb_data) {
    fprintf(stderr, "Failed to allocate playback data\n");
    return;
  }

  pb_data->total_frames = frames;
  pb_data->audio_data = data;
  pb_data->position = 0;

  PaStream *playback_stream;
  PaError err;

  PaStreamParameters outputParameters = {
      .device = Pa_GetDefaultOutputDevice(),
      .channelCount = CHANNELS,
      .sampleFormat = paFloat32,
      .suggestedLatency = Pa_GetDeviceInfo(Pa_GetDefaultOutputDevice())
                              ->defaultLowOutputLatency,
      .hostApiSpecificStreamInfo = NULL};

  err = Pa_OpenStream(&playback_stream, NULL, &outputParameters, SAMPLE_RATE,
                      FRAMES_PER_BUFFER, paClipOff, playback_callback, pb_data);

  if (err != paNoError) {
    fprintf(stderr, "Error opening playback stream: %s\n",
            Pa_GetErrorText(err));
    free(pb_data);
    return;
  }

  printf("Playing back recording...\n");
  err = Pa_StartStream(playback_stream);
  if (err != paNoError) {
    fprintf(stderr, "Error starting playback: %s\n", Pa_GetErrorText(err));
    free(pb_data);
    return;
  }

  while (Pa_IsStreamActive(playback_stream)) {
    Pa_Sleep(100);
  }

  Pa_StopStream(playback_stream);
  Pa_CloseStream(playback_stream);
  free(pb_data);
}

float *capture_noise_profile(size_t *noise_frames) {
  PaStream *noise_stream;
  NoiseState noise_state = {
      .noise_data = malloc(SAMPLE_RATE * NOISE_SAMPLE_DURATION * sizeof(float)),
      .frames_count = 0,
      .max_frames = SAMPLE_RATE * NOISE_SAMPLE_DURATION};

  if (!noise_state.noise_data) {
    fprintf(stderr, "Failed to allocate noise buffer\n");
    return NULL;
  }

  PaStreamParameters inputParameters = {
      .device = Pa_GetDefaultInputDevice(),
      .channelCount = CHANNELS,
      .sampleFormat = paFloat32,
      .suggestedLatency =
          Pa_GetDeviceInfo(Pa_GetDefaultInputDevice())->defaultLowInputLatency,
      .hostApiSpecificStreamInfo = NULL};

  PaError err =
      Pa_OpenStream(&noise_stream, &inputParameters, NULL, SAMPLE_RATE,
                    FRAMES_PER_BUFFER, paClipOff, noise_callback, &noise_state);

  if (err != paNoError) {
    fprintf(stderr, "Error opening noise capture stream: %s\n",
            Pa_GetErrorText(err));
    free(noise_state.noise_data);
    return NULL;
  }

  printf("Please be quiet for %.1f seconds to capture noise profile...\n",
         NOISE_SAMPLE_DURATION);
  err = Pa_StartStream(noise_stream);
  if (err != paNoError) {
    fprintf(stderr, "Error starting noise capture: %s\n", Pa_GetErrorText(err));
    free(noise_state.noise_data);
    Pa_CloseStream(noise_stream);
    return NULL;
  }

  // Wait for the full duration plus a small buffer
  Pa_Sleep((int)(NOISE_SAMPLE_DURATION * 1000) + 100);

  Pa_StopStream(noise_stream);
  Pa_CloseStream(noise_stream);

  // Check if we got enough frames
  if (noise_state.frames_count < (SAMPLE_RATE * NOISE_SAMPLE_DURATION *
                                  0.9)) { // Allow for small variations
    fprintf(stderr, "Incomplete noise capture (got %zu frames, expected %zu)\n",
            noise_state.frames_count,
            (size_t)(SAMPLE_RATE * NOISE_SAMPLE_DURATION));
    free(noise_state.noise_data);
    return NULL;
  }

  *noise_frames = noise_state.frames_count;
  return noise_state.noise_data;
}

int main() {
  char *voice_dir = get_voice_dir();
#ifdef _WIN32
  _mkdir(voice_dir);
#else
  mkdir(voice_dir, 0755);
#endif

  int stderr_fd = dup(STDERR_FILENO); // Save original stderr
  freopen("/dev/null", "w", stderr);  // Redirect stderr to /dev/null

  PaError err = Pa_Initialize();
  if (err != paNoError)
    goto error;

  fflush(stderr); // restore stderr
  dup2(stderr_fd, STDERR_FILENO);
  close(stderr_fd);

  // First, try to load existing noise profile
  float *noise_data = NULL;
  size_t noise_frames = 0;
  bool load_success = load_noise_profile(voice_dir, &noise_data, &noise_frames);

  // If no existing profile, capture a new one

  if (!load_success) {
    printf("No existing noise profile found. Need to capture one.\n");
    noise_data = capture_noise_profile(&noise_frames);
    if (!noise_data) {
      fprintf(stderr, "Failed to capture noise profile\n");
      goto cleanup;
    }

    // Save the new noise profile
    if (!save_noise_profile(voice_dir, noise_data, noise_frames)) {
      fprintf(stderr, "Warning: Failed to save noise profile for future use\n");
    }
  }

  // Initialize recording state
  size_t max_time_seconds = 60 * 30; // 30 minutes is ~10 MB of audio
  RecordingState state = {
      .recorded_data = malloc(SAMPLE_RATE * max_time_seconds * sizeof(float)),
      .frames_count = 0,
      .max_frames = SAMPLE_RATE * max_time_seconds,
      .pitch_detector = new_aubio_pitch("yin", AUBIO_BUFFER_SIZE,
                                        AUBIO_HOP_SIZE, SAMPLE_RATE),
      .input_buffer = new_fvec(AUBIO_HOP_SIZE),
      .pitch_output = new_fvec(1),
      .pitch_history_count = 0,
      .samples_processed = 0,
      .last_display_update = 0};

  if (!state.recorded_data || !state.pitch_detector || !state.input_buffer ||
      !state.pitch_output) {
    fprintf(stderr, "Failed to allocate resources\n");
    goto cleanup;
  }

  // Start recording audio
  PaStream *recording_stream;
  PaStreamParameters inputParameters = {
      .device = Pa_GetDefaultInputDevice(),
      .channelCount = CHANNELS,
      .sampleFormat = paFloat32,
      .suggestedLatency =
          Pa_GetDeviceInfo(Pa_GetDefaultInputDevice())->defaultLowInputLatency,
      .hostApiSpecificStreamInfo = NULL};

  err = Pa_OpenStream(&recording_stream, &inputParameters, NULL, SAMPLE_RATE,
                      FRAMES_PER_BUFFER, paClipOff, recording_callback, &state);
  if (err != paNoError)
    goto error;

  // Wait for user input to stop or cancel recording.
  signal(SIGINT, handle_sigint);

  printf("\033[?25l"); // Hide cursor
  printf("\nRecording started. Press Enter to stop, or ^C to cancel.\n\n");
  draw_pitch_bar(0.0f);

  err = Pa_StartStream(recording_stream);
  if (err != paNoError)
    goto error;

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

  should_stop = false;
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

  Pa_StopStream(recording_stream);
  Pa_CloseStream(recording_stream);
  printf("\033[?25h"); // Show cursor

  // Trim last 30ms and apply noise reduction
  size_t trim_samples = (SAMPLE_RATE * 30) / 1000; // 30ms worth of samples
  size_t final_frames = state.frames_count > trim_samples ? 
                       state.frames_count - trim_samples : 
                       state.frames_count;

  float *cleaned_audio = malloc(final_frames * sizeof(float));
  if (!cleaned_audio) {
    fprintf(stderr, "Failed to allocate memory for cleaned audio\n");
    goto cleanup;
  }

  // Compute noise threshold and process the trimmed audio
  SpectralGate *sg = spectralgate_create(SAMPLE_RATE);
  if (!sg) {
    fprintf(stderr, "Failed to create spectral gate\n");
    goto cleanup;
  }
  sg->prop_decrease = 0.0;
  sg->n_std_thresh = 2.5;

  spectralgate_compute_noise_thresh(sg, noise_data, noise_frames);
  spectralgate_process(sg, state.recorded_data, cleaned_audio,
                       final_frames);

  // Save recording with timestamp
  time_t now;
  time(&now);
  char filename[4096];
  strftime(filename, sizeof(filename), "/voice_sample_%Y%m%d-%H%M%S.wav",
           localtime(&now));
  char *full_path = malloc(strlen(voice_dir) + strlen(filename) + 1);
  sprintf(full_path, "%s%s", voice_dir, filename);

  // Save the file
  save_recording(full_path, cleaned_audio, final_frames);
  printf("\nSaved cleaned audio to: %s\n", full_path);

  // Play back the cleaned audio
  play_audio(cleaned_audio, final_frames);

cleanup:
  // Clean up resources
  spectralgate_destroy(sg);
  free(voice_dir);
  free(full_path);
  free(cleaned_audio);
  free(noise_data);
  if (state.recorded_data)
    free(state.recorded_data);
  if (state.pitch_detector)
    del_aubio_pitch(state.pitch_detector);
  if (state.input_buffer)
    del_fvec(state.input_buffer);
  if (state.pitch_output)
    del_fvec(state.pitch_output);
  Pa_Terminate();
  return 0;

error:
  fprintf(stderr, "PortAudio error: %s\n", Pa_GetErrorText(err));
  goto cleanup;
}
