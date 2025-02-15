import pyaudio
import wave
import numpy as np
import noisereduce as nr
from scipy.io import wavfile
import os
from datetime import datetime
import time
from pathlib import Path

class VoiceRecorder:
    def __init__(self):
        self.CHUNK = 1024
        self.FORMAT = pyaudio.paFloat32
        self.CHANNELS = 1
        self.RATE = 44100
        self.RECORD_SECONDS = 5  # Default recording duration
        self.VOICE_DIR = Path.home() / "Voice"
        
        # Create Voice directory if it doesn't exist
        self.VOICE_DIR.mkdir(exist_ok=True)
        
        self.p = pyaudio.PyAudio()

    def record(self):
        """Record audio from microphone"""
        print("* Recording...")
        
        stream = self.p.open(format=self.FORMAT,
                           channels=self.CHANNELS,
                           rate=self.RATE,
                           input=True,
                           frames_per_buffer=self.CHUNK)
        
        frames = []
        
        # Record initial noise profile (1 second)
        print("* Recording noise profile...")
        noise_frames = []
        for _ in range(0, int(self.RATE / self.CHUNK)):
            data = stream.read(self.CHUNK)
            noise_frames.append(np.frombuffer(data, dtype=np.float32))
        
        noise_profile = np.concatenate(noise_frames)
        print("* Noise profile captured")
        
        # Record actual audio
        print("* Now recording your voice...")
        for _ in range(0, int(self.RATE / self.CHUNK * self.RECORD_SECONDS)):
            data = stream.read(self.CHUNK)
            frames.append(np.frombuffer(data, dtype=np.float32))
        
        print("* Done recording")
        
        stream.stop_stream()
        stream.close()
        
        # Convert frames to numpy array
        audio_data = np.concatenate(frames)
        
        # Apply noise reduction
        reduced_noise = nr.reduce_noise(
            y=audio_data,
            sr=self.RATE,
            y_noise=noise_profile,
            prop_decrease=0.75
        )
        
        return reduced_noise

    def save_audio(self, audio_data):
        """Save the recorded audio to a file"""
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        filename = self.VOICE_DIR / f"recording_{timestamp}.wav"
        
        wavfile.write(filename, self.RATE, audio_data)
        print(f"* Saved to {filename}")
        return filename

    def play_audio(self, filename):
        """Play back the recorded audio"""
        print("* Playing back recording...")
        
        # Open the saved file
        wf = wave.open(str(filename), 'rb')
        
        stream = self.p.open(format=self.p.get_format_from_width(wf.getsampwidth()),
                           channels=wf.getnchannels(),
                           rate=wf.getframerate(),
                           output=True)
        
        # Read data in chunks and play
        data = wf.readframes(self.CHUNK)
        while data:
            stream.write(data)
            data = wf.readframes(self.CHUNK)
        
        stream.stop_stream()
        stream.close()
        print("* Playback finished")

    def cleanup(self):
        """Clean up PyAudio"""
        self.p.terminate()

def main():
    recorder = VoiceRecorder()
    
    try:
        while True:
            input("Press Enter to start recording (Ctrl+C to exit)...")
            audio_data = recorder.record()
            filename = recorder.save_audio(audio_data)
            time.sleep(0.5)  # Small delay before playback
            recorder.play_audio(filename)
    except KeyboardInterrupt:
        print("\n* Recording session ended")
    finally:
        recorder.cleanup()

if __name__ == "__main__":
    main()
