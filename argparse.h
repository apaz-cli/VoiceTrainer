#ifndef VOICETRAINER_ARGPARSE
#define VOICETRAINER_ARGPARSE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FOLDER_NAME "Voice"

typedef struct {
  char *output_file;    // Output file path (may be NULL for default)
  char *voice_dir;      // Directory for voice files
  bool no_playback : 1;      // Disable playback after recording
  bool help : 1;             // Show help message
} VoiceTrainerArgs;

static inline char *get_voice_dir(void) {
  const char *home_dir = getenv("HOME");
  if (!home_dir) {
    fprintf(stderr, "Error: HOME environment variable not set\n");
    exit(1);
  }

  char *voice_dir =
      (char *)malloc(strlen(home_dir) + 1 + strlen(FOLDER_NAME) + 1);
  sprintf(voice_dir, "%s/%s", home_dir, FOLDER_NAME);
  return voice_dir;
}

static inline char *get_save_path(char *output_file, char *voice_dir) {
  char *save_path = NULL;
  if (!output_file) {
    time_t now;
    time(&now);
    char filename[16384];
    strftime(filename, sizeof(filename), "/voice_sample_%Y%m%d-%H%M%S.wav",
             localtime(&now));
    save_path = (char *)malloc(strlen(voice_dir) + strlen(filename) + 1);
    sprintf(save_path, "%s%s", voice_dir, filename);
  } else {
    // Check if we need to append extension
    size_t len = strlen(output_file);
    if (len < 4 || strcmp(output_file + len - 4, ".wav") != 0) {
      char *new_output = (char *)malloc(len + 5);
      if (!new_output) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        exit(1);
      }
      strcpy(new_output, output_file);
      strcat(new_output, ".wav");
      output_file = new_output;
    } else {
        output_file = strdup(output_file);
    }
    save_path = output_file;
    output_file = NULL;
  }

  // If the file already exists, prompt the user to overwrite or not
  if (access(save_path, F_OK) == 0) {
    char response;
    printf("File %s already exists. Overwrite? (y/N): ", save_path);
    response = getchar();
    if (response != 'y' && response != 'Y') {
      printf("Not overwriting %s, exiting.\n", save_path);
      exit(0);
      return NULL;
    }
  }

  return save_path;
}

static inline VoiceTrainerArgs voicetrainer_argparse(int argc, char **argv) {
  VoiceTrainerArgs args = {0};

  // Second pass: parse other arguments
  for (int i = 1; i < argc; i++) {
    char *arg = argv[i];
    if (!strcmp(arg, "-o") || !strcmp(arg, "--output")) {
      if (i + 1 < argc) {
        args.output_file = argv[++i];
      } else {
        fprintf(stderr, "Error: -o requires an output filename\n");
        exit(1);
      }
    } else if (!strcmp(arg, "-n") || !strcmp(arg, "--no-playback")) {
      args.no_playback = 1;
    } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
      args.help = 1;
    } else if (arg[0] == '-') {
      fprintf(stderr, "Error: Unknown option '%s'\n", arg);
      exit(1);
    } else {
      // Positional argument (output file)
      if (args.output_file == NULL) {
        args.output_file = arg;
      } else {
        fprintf(stderr, "Error: Multiple output files specified\n");
        exit(1);
      }
    }
  }

  char helpmsg[] =
      "Voice Recorder with Playback\n\n"
      "Usage: voicetrainer [OPTIONS] [OUTPUT_FILE]\n\n"
      "Options:\n"
      "  -o, --output FILE    Specify output filename (default: timestamped in ~/Voice)\n"
      "  -n, --no-playback    Disable playback after recording\n"
      "  -h, --help           Show this help message and exit\n\n"
      "If OUTPUT_FILE can be specified positionally, or with the flag, or not at all.\n"
      "If OUTPUT_FILE doesn't end with .wav, it will be appended.\n";

  if (args.help) {
    puts(helpmsg);
    exit(0);
  }

  args.voice_dir = get_voice_dir();
  args.output_file = get_save_path(args.output_file, args.voice_dir);
  return args;
}

#endif /* VOICETRAINER_ARGPARSE */