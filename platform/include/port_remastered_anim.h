// Reader for the skeletal animations in a Metroid Prime Remastered character (CHPR)
// file. A port of build/mpr/anim/chpr_anim.py and chpr_skel.py (the notes next to them
// say how the format was found). It has no game or GX dependencies.
//
// A CHPR holds a pool of names, one compressed animation blob per animation
// (CAnimCompStream in the exe) and a reference to the skinned model (SMDL) it moves.
// Only the stream layouts the Python decoder knows are supported: rotation tracks with
// constant translation and scale. Anything else makes ReadCharacter fail with a
// message rather than guess.

#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace PortRemasteredAnim {

// One bone at one frame, in Remastered model space.
struct Key {
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // unit quaternion, x y z w
  float translation[3] = {0.0f, 0.0f, 0.0f};
  float scale[3] = {1.0f, 1.0f, 1.0f};
};

struct Anim {
  std::string name;
  // Frames per second: the header float the exe's CAnimCompStream::GetNativeDuration
  // divides (frames - 1) by, so the duration in seconds is (frames - 1) / fps.
  float fps = 0.0f;
  uint32_t frames = 0;
  // bones[b][frame] for every frame 0..frames-1, sampled at integer frame times.
  // Bones the animation has no track for hold the identity key.
  std::vector<std::vector<Key>> bones;
};

struct Character {
  // The SMDL the character skins, as the 16 bytes sit in the file. That is the
  // property order (Python's bytes_le), the same form a room property GUID has
  // there, so pass it through the same swap as port_remastered_room.cpp's SwapUuid
  // (first three fields byte-reversed) to get the pak id the model lookup takes.
  // All zero when the file has no reference where the reader looks for it (it is
  // located by position after the animation records, which is only confirmed on the
  // elevator paddle character).
  std::array<uint8_t, 16> skinnedModel{};
  std::vector<Anim> anims;
};

// Parses a whole CHPR file. Returns false with a message in `error` for input that is
// malformed or uses a track layout that is not implemented; never reads out of range.
bool ReadCharacter(const std::vector<uint8_t>& chpr, Character& out, std::string& error);

// The animation called `name`, or nullptr.
const Anim* Find(const Character& c, std::string_view name);

}  // namespace PortRemasteredAnim
