#pragma once

// A Power Suit spider ball, made from the player's disc. Retail has none: the
// Power Suit's spider ball is the Gravity Suit's (SamusSpiderBallANCS material
// set 0, indigo). This makes Varia's (set 1) with Varia's own colours moved to
// the Power Suit's: the shift is fitted from the two suits' normal balls, and
// applied only where Varia's spider ball texture differs from Gravity's, so
// the parts all suits share (the red bands) stay as they are.
//
// The result goes under the PortModelVariant ids for suit 0, which the game
// already loads when they exist (CMorphBall::PortSuitVariant). PortMods serves
// it below every mod, mods on or off, from a cache in the user folder
// (generated/power-spider/), and leaves it out when a mod brings its own
// spider ball (the Remastered import).

#include "port_mods.h"
#include "port_remastered_image.h"

#include <cstdint>
#include <map>
#include <utility>
#include <vector>

namespace PortPowerSpiderBall {

// Bump when the output changes: the cache is remade when its stamp differs.
inline constexpr int kVersion = 2;

// The retail resources the look is made from.
inline constexpr uint32_t kSpiderAncs = 0xF37654A2;      // SamusSpiderBallANCS
inline constexpr uint32_t kSpiderCmdl = 0x223353FE;      // its model
inline constexpr uint32_t kLowPolyCmdl = 0x0B57F9E8;     // SamusSpiderBallLowPolyCMDL
inline constexpr uint32_t kGlassCmdl = 0xAE78BEBE;       // SamusSpiderBallGlassCMDL
inline constexpr uint32_t kSpiderVaria = 0x2EE6F56F;     // set 1 (Varia) shell
inline constexpr uint32_t kSpiderGravity = 0x50A70472;   // set 0 (Gravity) shell
inline constexpr uint32_t kLowPolyVaria = 0x07675658;
inline constexpr uint32_t kLowPolyGravity = 0xBA7DF5D6;
inline constexpr uint32_t kBallPower = 0xC01FFF01;       // the normal ball's Power shell
inline constexpr uint32_t kBallVaria = 0x49A1D81D;       // and its Varia shell

// Two texels count as different when a channel differs by more than this.
inline constexpr int kDifferThreshold = 16;

// A move in HSV: hue in degrees, saturation and value in 0..1.
struct Shift {
  double hue = 0.0;
  double saturation = 0.0;
  double value = 0.0;
  size_t samples = 0;  // texels the fit used
};

// The top level of a CMPR TXTR (format 10) as RGBA, decoded as GX does (the
// three-colour mode's fourth colour is transparent). False for another format.
bool DecodeCmprTxtr(const std::vector<uint8_t>& txtr, PortRemastered::Image& out);

// The median HSV move from `from` to `to` over the texels both have opaque and
// coloured that differ: the two looks of one texture.
Shift FitShift(const PortRemastered::Image& from, const PortRemastered::Image& to,
               int threshold = kDifferThreshold);

// Applies `shift` to the opaque texels of `image` that differ from `other`
// (the same texture in the look it should keep apart from). Returns how many
// changed; zero when the sizes differ.
size_t Recolour(PortRemastered::Image& image, const PortRemastered::Image& other, const Shift& shift,
                int threshold = kDifferThreshold);

// Makes material set `to` of a CMDL a copy of set `from`, with the texture ids
// in `swap` replaced in its texture list. False, and the model unchanged, when
// it is not a CMDL with both sets.
bool CopyMaterialSet(std::vector<uint8_t>& cmdl, uint32_t from, uint32_t to,
                     const std::map<uint32_t, uint32_t>& swap = {});

// The (type, id) keys that, when a mod has one, mean it brings its own spider
// ball: the retail resources above and the generated ids.
std::vector<std::pair<uint32_t, uint32_t>> ModelKeys();

// The generated resources, made or taken from the cache. Empty when the disc
// lacks a source (logged). Needs the disc open; for PortMods::Initialize.
std::vector<PortMods::LooseResource> Collect();

// What the last PortMods::Initialize did: whether the setting was on, and
// whether the look is being served.
void SetState(bool requested, bool active);
bool Requested();
bool Active();

}  // namespace PortPowerSpiderBall
