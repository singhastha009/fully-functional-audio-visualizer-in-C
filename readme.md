# 🎧 Real-Time Audio Visualizer – FFmpeg + CoreAudio + GTK (macOS)

This is a real-time audio visualizer written in C using:
- **FFmpeg** for decoding audio files
- **CoreAudio** for low-latency playback
- **GTK + Cairo** for rendering waveform and bar visualizations
- **Pthreads** and **circular buffer** for thread-safe audio streaming

Visualizations respond to audio in real time and support theme switching and layout changes via keyboard input.

---

## 🧠 Features

- ✅ Decodes and plays MP3 or other audio formats using FFmpeg
- ✅ Real-time waveform and bar-style visualizations
- ✅ Audio playback with CoreAudio (macOS)
- ✅ Smooth animations with exponential averaging
- ✅ Multithreaded decoding and playback using circular buffer
- ✅ Keyboard controls for theme/mode switching

---

## 🎛 Controls

| Key | Action                          |
|-----|---------------------------------|
| `C` | Cycle through visualization themes (color) |
| `1` | Switch to waveform mode         |
| `2` | Switch to bar mode (default)    |

---

## 🛠 Technologies Used

- **Language:** C  
- **Audio Decoding:** FFmpeg (`libavformat`, `libavcodec`, `libswresample`)  
- **Audio Output:** CoreAudio (macOS only)  
- **Graphics:** GTK 4, Cairo  
- **Threading:** Pthreads  
- **Buffers:** Circular ring buffer (double buffering pattern)

---

## 🚀 How to Build

### 1. Install Dependencies

#### On macOS (Homebrew):
```bash
brew install ffmpeg gtk4

Compile - clang A9.c -o A9 \
  $(pkg-config --cflags --libs gtk4) \
  -lavformat -lavcodec -lswresample -lavutil \
  -framework CoreAudio -framework AudioToolbox \
  -lm -lpthread
Run - ./A9
