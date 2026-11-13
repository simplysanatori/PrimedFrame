# PrimedFrame 2.0.1

A fix for stutter in the heaviest scenes, like big fights and the Frigate's explosions.

### Fixed

- In heavy scenes, frames could run late and the picture would stutter. At the end of every frame, the game stopped for about 3 ms while the GPU command queue caught up, before it could start the next frame. It now hands that last stretch to the queue's own thread and starts the next frame straight away, so heavy frames have about 3 ms more room.
- Loading the next room could stall a single frame. Background loading could take 8 ms or more of one frame, which made that frame late. Loading now gets at most 2 ms a frame and finishes over the next few frames instead.

### Changed

- The release files are now built by GitHub Actions from the tagged source.

### Known issues

- In a few of the heaviest scenes the game slows down to 75% speed for about half a second, then carries on at full speed.
- Some room loads slow the game down slightly for a moment.

## Download

- `primedframe-2.0.1-linux-arm64.tar.gz` (also what Frametanium installs)
- `primedframe-2.0.1-framedrop-linux-arm64.zip` and `primedframe.framedrop.json` (for FrameDrop)
- `SHA256SUMS`

## Install or upgrade

Update from Frametanium or FrameDrop, or unpack the tarball on the Frame and run `install.sh`. It keeps your saves and settings. The [README](https://github.com/simplysanatori/PrimedFrame/blob/main/README.md) has the full steps.
