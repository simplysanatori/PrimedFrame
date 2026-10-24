#pragma once

#include <cstdint>

// What the game and the mod loader need to know of the "dread-samus" mod (port_dread_samus.h, the
// import that makes it), without the import's headers.
namespace PortDreadSamus {

// The mod's folder in the mods folder, and its stamp: kVersion, then the build's commit. Its own
// number, kept out of PortRemastered::kImportVersion, so a change here never asks for a Remastered
// re-import. Bump it when the output changes.
inline constexpr const char* kModName = "dread-samus";
inline constexpr const char* kStampName = ".dread-samus-version";
// 1: Power/Varia/Gravity, cutscene (CINF 19C53902) and in-game (A496DA02) bodies.
// 2: suit materials carry REFL (env specular on the metal plates).
// 3: shoulder pads skinned to the chest, not voted from Prime's collar.
inline constexpr int kVersion = 3;

// The in-game body CMDL of a CPlayerState::EPlayerSuit (0 Power, 1 Gravity, 2 Varia; also
// BallTransitionsANCS's character index), 0 for the suits the mod leaves alone (Phazon, Fusion).
inline uint32_t InGameBody(int suit) {
  switch (suit) {
  case 0:
    return 0x4A1A8FD3;
  case 1:
    return 0x57489AD3;
  case 2:
    return 0xE449874A;
  default:
    return 0;
  }
}

// The cutscene body CMDL of a cutscene ANCS character (0 Power, 1 Gravity, 2 Varia, as every
// cutscene ANCS and the elevator's order them), 0 for the others.
inline uint32_t CutsceneBody(int charIdx) {
  switch (charIdx) {
  case 0:
    return 0xF39476F2;
  case 1:
    return 0x57F5C930;
  case 2:
    return 0x776C1A96;
  default:
    return 0;
  }
}

// Whether the Dread body stands in for a retail body CMDL now: the dread-samus mod is loaded and
// serves that id (PortMods). Its skinned cannon replaces Prime's separate one, which is then not drawn.
bool Replaces(uint32_t bodyCmdl);

// The skin the mod ships for a cutscene body CMDL it serves (CINF 19C53902), 0 for any other. A
// Remastered install replaces the Samus ANCS and with it the skin ids, so a character's own skin id
// may be Remastered's, whose weights belong to Remastered's mesh, not the Dread one.
inline uint32_t CutsceneSkin(uint32_t bodyCmdl) {
  switch (bodyCmdl) {
  case 0xF39476F2:
    return 0x98BBD924;
  case 0x776C1A96:
    return 0x1C43B540;
  case 0x57F5C930:
    return 0x3CDA66E6;
  default:
    return 0;
  }
}

// The Dread-proportioned skeleton the mod ships for a body CMDL it serves: the retail CINF's joints
// with their heads moved to the Dread body's (one per suit and skeleton), 0 for any other. New ids,
// as Phazon still uses the retail CINFs.
inline uint32_t DreadCinf(uint32_t bodyCmdl) {
  switch (bodyCmdl) {
  case 0x4A1A8FD3:
    return 0xD7EAD001;
  case 0x57489AD3:
    return 0xD7EAD002;
  case 0xE449874A:
    return 0xD7EAD003;
  case 0xF39476F2:
    return 0xD7EAD004;
  case 0x57F5C930:
    return 0xD7EAD005;
  case 0x776C1A96:
    return 0xD7EAD006;
  default:
    return 0;
  }
}

}  // namespace PortDreadSamus
