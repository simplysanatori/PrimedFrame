# PrimedFrame 2.0.1

A fix for stutter in the heaviest scenes, like big fights and the Frigate's explosions.

### Fixed

- **Stutter in heavy scenes.** Frames ran late in big fights and explosions.
  - **FIFO:** the game no longer waits about 3 ms each frame for the GPU command queue. The queue's own thread finishes the frame while the game starts the next.
  - **CPU optimizations:** background loading is capped at 2 ms a frame, down from 8 ms or more.
  - **GPU optimizations:** a model cache that cuts GPU work is built in, but off until it stops stalling explosions.

### Known issues

- In a few of the heaviest scenes the game slows down to 75% speed for about half a second, then carries on at full speed.
- Some room loads slow the game down slightly for a moment.

## Download

- `primedframe-2.0.1-linux-arm64.tar.gz` (also what Frametanium installs)
- `primedframe-2.0.1-framedrop-linux-arm64.zip` and `primedframe.framedrop.json` (for FrameDrop)
- `SHA256SUMS`

## Install or upgrade

Update from Frametanium or FrameDrop, or unpack the tarball on the Frame and run `install.sh`. It keeps your saves and settings. The [README](https://github.com/simplysanatori/PrimedFrame/blob/main/README.md) has the full steps.
