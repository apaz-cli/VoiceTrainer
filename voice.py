import os
import sounddevice as sd
import soundfile as sf
import numpy as np
from datetime import datetime

# Configuration
SAMPLE_RATE = 44100  # Audio sample rate
CHANNELS = 1  # Number of audio channels
OUTPUT_DIR = os.path.expanduser("~/Voice")  # Output directory
NOISE_PROFILE_PATH = os.path.join(OUTPUT_DIR, ".noise_profile.wav")  # Hidden noise profile

def record_noise_profile():
    """Record and save a 2-second noise profile."""
    print("\nNoise Profile Setup")
    print("===================")
    print("1. Ensure environment is quiet (no speaking)")
    print("2. Will record 2 seconds of ambient noise")
    input("Press Enter to start noise recording...")

    print("Recording noise...")
    noise = sd.rec(int(2 * SAMPLE_RATE), samplerate=SAMPLE_RATE, channels=CHANNELS)
    sd.wait()
    sf.write(NOISE_PROFILE_PATH, noise, SAMPLE_RATE)
    print(f"Noise profile saved to: {NOISE_PROFILE_PATH}")
    return noise.flatten()

def load_noise_profile():
    """Load existing noise profile."""
    if os.path.exists(NOISE_PROFILE_PATH):
        print("Loading existing noise profile...")
        noise, _ = sf.read(NOISE_PROFILE_PATH)
        return noise.flatten()
    return None

def record_until_stop():
    """Record audio until Enter is pressed or Ctrl+C."""
    print("\nVoice Recording")
    print("===============")
    print("1. Speak clearly into your microphone")
    print("2. Press Enter to stop recording")
    print("   or press Ctrl+C to cancel")

    recorded_frames = []  # Renamed to avoid conflict

    def callback(indata, frame_count, time, status):
        """Callback function for audio input stream."""
        recorded_frames.append(indata.copy())

    try:
        with sd.InputStream(samplerate=SAMPLE_RATE, channels=CHANNELS, callback=callback):
            input("\nRecording started...\nPress Enter to stop ")
    except KeyboardInterrupt:
        print("\nRecording cancelled")
        return None

    if not recorded_frames:
        print("No audio recorded")
        return None

    return np.concatenate(recorded_frames, axis=0).flatten()

def play_audio(audio, samplerate):
    """Play audio with a larger buffer size to avoid underruns."""
    try:
        # Use a larger buffer size to prevent underruns
        blocksize = 2048  # Increase buffer size
        sd.play(audio, samplerate, blocksize=blocksize)
        sd.wait()
    except Exception as e:
        print(f"Error during playback: {e}")

def main():
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    # Voice recording
    raw_audio = record_until_stop()
    if raw_audio is None:
        return

    # Noise profile handling
    noise_clip = load_noise_profile()
    if noise_clip is None:
        noise_clip = record_noise_profile()

    # Noise reduction
    import noisereduce as nr
    cleaned_audio = nr.reduce_noise(
        y=raw_audio,
        y_noise=noise_clip,
        sr=SAMPLE_RATE,
        stationary=False
    )

    # Save and playback
    timestamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    filename = os.path.join(OUTPUT_DIR, f"voice_sample_{timestamp}.wav")
    sf.write(filename, cleaned_audio.reshape(-1, 1), SAMPLE_RATE)
    print(f"\nSaved cleaned audio to: {filename}")

    print("Playing back...")
    play_audio(cleaned_audio, SAMPLE_RATE)
    print("Playback complete")

if __name__ == "__main__":
    main()
