# simple-sc

A simple PipeWire screen recording utility for Linux.

### Usage

`simple-sc` will launch the ScreenCast portal, begin recording, save to `$HOME/Videos/` by default.

###### Flags
`-h, --help` self explanatory.\
`-s, --stop` will stop a running instance of simple-sc, useful if you need a keybind to stop.\
`-o, --output` allows one to select the output path.

### Why did you make this? Aren't there are many good screen recording utilities like wf-recorder, OBS, Spectacle, etc...

wf-recorder (with slurp) is ideal for me, but KDE Plasma doesn't support the Wayland extension needed to use it. OBS is very full-featured, but has too much friction for what I need. Spectacle is great, but doesn't record desktop audio, which is useful for me since I create [an audio metering app](https://minimeters.app).

Besides it's fun creating things. The Desktop Portal already prompts to select between capturing the display, a region, or an application so that part was done for me. The rest was learning to use PipeWire to receive the video/audio, ffmpeg's libraries to encode a video file, and creating this utility app which puts them together.

### Installing

**Arch (AUR)**\
```yay -S simple-sc```

### Building From Source

Ensure the following dependencies are installed:\
```libpipewire-0.3 dbus-1 libportal libavcodec libavformat libavutil libswscale libswresample```

**Arch**\
```pacman -S base-devel cmake libpipewire dbus libportal ffmpeg```

**Ubuntu 22.04 (and later)**\
```apt install build-essential cmake libpipewire-0.3-dev libdbus-1-dev libportal-dev libavcodec-dev libavformat-dev libswscale-dev libswresample-dev```

```
git clone https://github.com/directmusic/simple-sc
cd simple-sc
cmake -B build
cmake --build build --config Release
```

To install:
```cd build && sudo make install```

### Is This Vibe-Coded?

No, but I did use a chatbot to learn about frame-timing and to help understand the encoding process. I will follow up and update this if/when any AI tools are used during development.

### Similar Tools
[wf-recorder](https://github.com/ammen99/wf-recorder)\
[Spectacle](https://github.com/kde/spectacle)

### Acknowledgments
[OBS](https://github.com/obsproject/obs-studio) - For inspiring me to create this.\
[wemeet-wayland-screenshare](https://github.com/xuwd1/wemeet-wayland-screenshare) - For helping me understand the PipeWire initialization flow.
