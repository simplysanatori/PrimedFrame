#include "port_map_pickups.h"

#include "port_ap_metroidprime.h"
#include "port_apclient.h"
#include "port_debug.h"
#include "MetroidPrime/Player/CGameState.hpp"

namespace PortMapPickups {

PortDebug::EMapPickups Mode() { return PortDebug::MapPickups(); }

bool Seen(size_t dot) { return gpGameState != nullptr && IsSeen(gpGameState->PortSeenPickups(), dot); }

bool MarkSeen(size_t dot) {
  if (gpGameState == nullptr || dot >= kSeenWords * 32 || IsSeen(gpGameState->PortSeenPickups(), dot)) {
    return false;
  }
  SetSeen(gpGameState->PortSeenPickups(), dot);
  return true;
}

size_t SeenCount() {
  size_t count = 0;
  size_t dots = 0;
  Dots(dots);
  for (size_t i = 0; i < dots; ++i) {
    count += Seen(i) ? 1 : 0;
  }
  return count;
}

void ClearSeen() {
  if (gpGameState != nullptr) {
    for (int i = 0; i < kSeenWords; ++i) {
      gpGameState->PortSeenPickups()[i] = 0;
    }
  }
}

void MarkAllSeen() {
  size_t dots = 0;
  Dots(dots);
  for (size_t i = 0; i < dots; ++i) {
    MarkSeen(i);
  }
}

bool Colors(const unsigned char*& colors) {
  size_t kCount = 0;
  const Dot* kDots = Dots(kCount);
  static unsigned char sColors[kMaxDots];
  // Which AP location each dot is: the one whose pickup sets the same relay.
  static int sLocation[kMaxDots];
  static bool sMapped = false;
  if (!sMapped) {
    size_t count = 0;
    const PortAp::MetroidPrime::Location* locations = PortAp::MetroidPrime::Locations(count);
    for (size_t i = 0; i < kCount; ++i) {
      sLocation[i] = -1;
      for (size_t j = 0; j < count && sLocation[i] < 0; ++j) {
        if (locations[j].world == kDots[i].world && locations[j].relay == kDots[i].relay)
          sLocation[i] = static_cast< int >(j);
      }
    }
    sMapped = true;
  }
  static PortAp::LogicState sState;
  if (!PortDebug::MapLogicColors() || !PortAp::Logic(sState))
    return false;
  for (size_t i = 0; i < kCount; ++i) {
    const int location = sLocation[i];
    if (location < 0) {
      sColors[i] = kC_White;
    } else if (sState.checked[location]) {
      sColors[i] = kC_Grey;
    } else {
      switch (sState.levels[location]) {
      case PortApLogic::Level::Normal: sColors[i] = kC_Green; break;
      case PortApLogic::Level::SequenceBreak: sColors[i] = kC_Yellow; break;
      case PortApLogic::Level::Inspect: sColors[i] = kC_Blue; break;
      case PortApLogic::Level::None: sColors[i] = kC_Red; break;
      }
    }
  }
  colors = sColors;
  return true;
}

} // namespace PortMapPickups
