<div align="center">

```
░█████╗░░█████╗░██╗██████╗░    ██╗░░██╗░█████╗░██╗░░░░░███████╗██╗██████╗░░█████╗░
██╔══██╗██╔══██╗██║██╔══██╗    ██║░██╔╝██╔══██╗██║░░░░░██╔════╝██║██╔══██╗██╔══██╗
███████║██║░░╚═╝██║██║░░██║    █████═╝░███████║██║░░░░░█████╗░░██║██║░░██║██║░░██║
██╔══██║██║░░██╗██║██║░░██║    ██╔═██╗░██╔══██║██║░░░░░██╔══╝░░██║██║░░██║██║░░██║
██║░░██║╚█████╔╝██║██████╔╝    ██║░╚██╗██║░░██║███████╗███████╗██║██████╔╝╚█████╔╝
╚═╝░░╚═╝░╚════╝░╚═╝╚═════╝░    ╚═╝░░╚═╝╚═╝░░╚═╝╚══════╝╚══════╝╚═╝╚═════╝░░╚════╝░
```

![Platform](https://img.shields.io/badge/platform-Windows-00ffaa?style=for-the-badge&logo=windows&logoColor=black)
![Language](https://img.shields.io/badge/C++17-00ffaa?style=for-the-badge&logo=cplusplus&logoColor=black)
![SDL2](https://img.shields.io/badge/SDL2-00ffaa?style=for-the-badge&logoColor=black)

</div>

---

a music visualizer i made because winamp skins weren't cutting it anymore.

it does real-time FFT on whatever's playing, splits the output into frequency bands, and throws it all at a stack of visual effects — kaleidoscopes, shockwaves, wormholes, neural nets, aurora curtains, the whole thing. everything reacts to the music. it also has a library, playlists, and can pull songs straight from a spotify playlist via yt-dlp.

---

## building it

you'll need MSYS2 with MinGW64. grab the dependencies:

```bash
pacman -S mingw-w64-x86_64-SDL2 mingw-w64-x86_64-SDL2_mixer mingw-w64-x86_64-SDL2_ttf mingw-w64-x86_64-fftw
```

then build:

```bash
g++ -std=c++17 visualizer.cpp -o visualizer.exe \
  -I/mingw64/include/SDL2 -I/mingw64/include \
  -L/mingw64/lib \
  -lmingw32 -lSDL2main -lSDL2 -lSDL2_mixer -lSDL2_ttf \
  -lfftw3 -lwinhttp -lws2_32 -lshell32 \
  -lm -lcomdlg32 -lstdc++fs -mwindows
```

---

## running it

put these next to `visualizer.exe`:

```
SDL2.dll, SDL2_mixer.dll, SDL2_ttf.dll, libfftw3-3.dll
libmpg123-0.dll, libopus-0.dll, libvorbis-0.dll
libvorbisfile-3.dll, libogg-0.dll, libFLAC.dll
```

you can grab all of these from `/mingw64/bin/` after installing the packages above.

for spotify import you also need `yt-dlp.exe` and `ffmpeg.exe` in the same folder — links in the spotify section below.

---

## controls

| | |
|---|---|
| `☰` top center | open menu |
| `space` | play / pause |
| `← →` | prev / next |
| `O` | open file |
| `drag & drop` | add to library |
| `esc` | close / quit |

---

## spotify import

> you need spotify premium for this. not my rule, spotify's. sorry.

1. make a free app at [developer.spotify.com](https://developer.spotify.com)
2. add `http://127.0.0.1:8888/callback` as a redirect URI
3. grab your client ID and secret from the app settings
4. hit **SPT** in the menu, paste them in, press enter
5. log in on the browser page that opens
6. pick a playlist — downloads start automatically

you'll need [yt-dlp.exe](https://github.com/yt-dlp/yt-dlp/releases) and [ffmpeg.exe](https://www.gyan.dev/ffmpeg/builds/) for the downloads to work.

---

## how it actually works

the audio goes through SDL2_mixer's post-mix callback into a ring buffer. every frame that buffer gets windowed with a Hann function and run through a 4096-point FFT via FFTW. the output bins get grouped into bands (sub-bass through high), smoothed with a leaky integrator, and fed into all the visual effects as parameters.

the trippy infinite-depth thing is a feedback loop — each frame the previous frame gets blitted back at a slight zoom, rotation, and hue shift before the new content goes on top. additive blending means it accumulates instead of replacing.

---

## folder layout

```
AcidKaleido/
├── visualizer.exe
├── *.dll
├── yt-dlp.exe
├── ffmpeg.exe
├── library.dat          ← auto-generated
├── spotify_config.txt   ← auto-generated
└── downloads/           ← spotify imports land here
    └── playlist name/
        └── *.mp3
```

---

## known issues

- if MP3s won't play, you're probably missing `libmpg123-0.dll` — copy it from `/mingw64/bin/`
- non-ASCII characters (emoji etc.) in spotify playlist names get stripped from the download folder name, the playlist itself is fine
- spotify premium is required and yes it's stupid

---

*made by Zaeer*
