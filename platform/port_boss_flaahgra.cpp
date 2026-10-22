#include "port_savestate_boss.h"

#include "MetroidPrime/Enemies/CFlaahgra.hpp"

// Stub: no Flaahgra fight state yet.
const uint8_t PortBossState::kFlaahgraVersion = 1;

void PortBossState::PutFlaahgra(CStateManager&, CFlaahgra&, PortSaveState::BlockWriter&) {}

void PortBossState::RestoreFlaahgra(CStateManager&, CFlaahgra&, PortSaveState::BlockReader*) {}
