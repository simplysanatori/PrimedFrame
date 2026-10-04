#pragma once

// Images for the Remastered importer: resizing, and the three files a mod
// texture is written as: a GameCube TXTR (RGBA8 or CMPR, with mips) and the
// port's native texture, a DX10 .dds in BC7 or BC5 (port_mods.h).
//
// The encoders are the importer's own and aim at "fast and good", not at the
// best a format can do: BC7 uses mode 6 only (one RGBA line a block, 4-bit
// indices), which is right for the smooth colour and data maps these are.

#include <cstdint>
#include <vector>

namespace PortRemastered {

struct Image {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> rgba;  // width * height * 4
};

enum class DdsFormat {
  BC7,  // all four channels
  BC5,  // red and green only: a normal map
};

// Lanczos-3, each channel on its own (alpha is often a mask here, not
// opacity, so nothing is premultiplied). The arithmetic follows Pillow's.
Image Resize(const Image& image, int width, int height);

// A mipmapped RGBA8 TXTR (format 9); mips stop once a side reaches minSize.
// Sides must be multiples of 4.
std::vector<uint8_t> EncodeTxtrRgba8(const Image& image, int minSize = 8);
// A mipmapped CMPR TXTR (format 10). Without `alpha` every texel is opaque;
// with it, texels under alpha 128 are transparent, and the smaller levels keep
// the top level's share of opaque texels. Sides must be multiples of 8.
std::vector<uint8_t> EncodeTxtrCmpr(const Image& image, bool alpha);
// A .dds with the whole mip chain down to 1x1. `punch` (a cut-out alpha) keeps
// the share of opaque texels in the smaller levels, as EncodeTxtrCmpr does.
std::vector<uint8_t> EncodeDds(const Image& image, DdsFormat format, bool punch = false);

// One 4x4 block, 16 RGBA texels in, 16 bytes out; exposed for the tests.
void EncodeBc7Block(const uint8_t* rgba, uint8_t* out);
void EncodeBc5Block(const uint8_t* rgba, uint8_t* out);

}  // namespace PortRemastered
