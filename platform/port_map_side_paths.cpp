// Where the map side files go: the port's own user folder and the directory of
// the memory card the driver has mounted.
//
// Split from port_map_side.cpp on purpose: that file holds the format and the
// staging, and takes its directory from here, so its unit test needs neither
// the card stack nor an SDL user folder. The hash of the card's directory is
// what separates one card from another - the port's own card, a Dolphin or
// PrimeHack card shared through the emulator-card setting, and the card of an
// Archipelago room each key their own files. Nothing is ever written into the
// card's (or the emulator's) folder: the side data lives under mapdata in the
// port's user folder, and only the slot's bytes decide whether it is used.

#include <string>

#include "Kyoto/CMemoryCardSys.hpp"
#include "port_map_side.h"
#include "port_paths.h"

namespace PortMapSide {

void Location(std::string& userFolder, std::string& cardBase) {
  userFolder = PortPaths::UserFolder();
  const char* card = CMemoryCardSys::PortCardBase();
  cardBase = card != nullptr ? card : "";
}

} // namespace PortMapSide
