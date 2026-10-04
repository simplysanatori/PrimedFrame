// Prints every animation PortRemasteredAnim reads from one Remastered CHPR, in the
// number format of build/mpr/anim/chpr_anim.py so the two can be diffed. It needs the
// developer's own game data, so it is not a ctest.
//
//   port_remastered_anim_tool <file.CHPR>

#include <cstdio>
#include <fstream>
#include <iterator>

#include "port_remastered_anim.h"

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <file.CHPR>\n", argv[0]);
    return 2;
  }
  std::ifstream in(argv[1], std::ios::binary);
  if (!in) {
    std::fprintf(stderr, "could not open '%s'\n", argv[1]);
    return 1;
  }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  PortRemasteredAnim::Character chr;
  std::string error;
  if (!PortRemasteredAnim::ReadCharacter(data, chr, error)) {
    std::fprintf(stderr, "%s: %s\n", argv[1], error.c_str());
    return 1;
  }
  std::printf("skinned model:");
  for (uint8_t b : chr.skinnedModel) {
    std::printf(" %02x", b);
  }
  std::printf("\n");
  for (const auto& a : chr.anims) {
    std::printf("%s fps=%g frames=%u bones=%zu\n", a.name.c_str(), double(a.fps), a.frames, a.bones.size());
    for (size_t b = 0; b < a.bones.size(); ++b) {
      for (uint32_t f = 0; f < a.frames; ++f) {
        const auto& k = a.bones[b][f];
        std::printf("  bone %zu f%02u rot=(%.6f %.6f %.6f %.6f) trans=(%g %g %g) scale=(%g %g %g)\n", b, f,
                    double(k.rotation[0]), double(k.rotation[1]), double(k.rotation[2]), double(k.rotation[3]),
                    double(k.translation[0]), double(k.translation[1]), double(k.translation[2]),
                    double(k.scale[0]), double(k.scale[1]), double(k.scale[2]));
      }
    }
  }
  return 0;
}
