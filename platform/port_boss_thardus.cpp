#include "port_savestate_boss.h"

#include "MetroidPrime/Enemies/CThardus.hpp"

// Stub: no Thardus fight state yet.
const uint8_t PortBossState::kThardusVersion = 1;

void PortBossState::PutThardus(CStateManager&, CThardus&, PortSaveState::BlockWriter&) {}

void PortBossState::RestoreThardus(CStateManager&, CThardus&, PortSaveState::BlockReader*) {}
