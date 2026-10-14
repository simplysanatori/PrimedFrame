#pragma once

#include <cstddef>
#include <cstdint>

#include "port_debug.h"

// Pickup marks on the map, like randomprime's: a plain white dot where each of
// the 100 item pickups sits, until it is collected. Deliberately one colour
// for every item, so the map doesn't spoil a randomized game; the colours an
// Archipelago game gives them say where to go, not what is there. The
// map_pickups setting picks the marking (PortDebug::EMapPickups): every dot,
// only the dots of pickups the player has seen (CScriptPickup::PortMarkSeen),
// or no dots but a tint on the rooms that still hold one. CMapWorld draws them
// (grep PortMapPickups).
namespace PortMapPickups {

struct Dot {
  uint32_t world; // MLVL
  uint32_t area;  // MREA
  uint32_t relay; // memory relay the pickup activates when collected
  float pos[3];   // world space
};

// The setting's mode; Off draws nothing.
PortDebug::EMapPickups Mode();

// Every pickup, from tools/gen_map_pickups.py.
const Dot* Dots(size_t& count);

// The dot of the pickup at a position in an area (its own coordinates), the
// nearest within kMatchRadius, or -1.
constexpr float kMatchRadius = 2.f;
int DotIndex(uint32_t world, uint32_t area, const float pos[3]);

// The seen set: one bit per dot index, in a save's four words
// (CGameState::PortSeenPickups).
constexpr int kSeenWords = 4;
constexpr size_t kMaxDots = kSeenWords * 32;
bool IsSeen(const uint32_t* words, size_t dot);
void SetSeen(uint32_t* words, size_t dot);
// The current game's set: false (and empty) outside a game.
bool Seen(size_t dot);
// True when the bit was new.
bool MarkSeen(size_t dot);
size_t SeenCount();
void ClearSeen();
void MarkAllSeen();

// In an Archipelago game the dots take a tracker's colours instead (the
// logic_colors setting, on by default), and collected ones stay as grey dots.
enum EColor {
  kC_White,  // no logic to go by
  kC_Green,  // in logic
  kC_Yellow, // reachable out of logic (a sequence break)
  kC_Blue,   // can be seen, not collected
  kC_Red,    // out of reach
  kC_Grey,   // checked
};
// One EColor per dot, in Dots order. False when the dots are plain white:
// call once a frame, not per dot.
bool Colors(const unsigned char*& colors);

} // namespace PortMapPickups
