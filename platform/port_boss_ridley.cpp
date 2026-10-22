#include "port_savestate_boss.h"

#include "MetroidPrime/Enemies/CRidley.hpp"

// Stub: no Ridley fight state yet.
const uint8_t PortBossState::kRidleyVersion = 1;

void PortBossState::PutRidley(CStateManager&, CRidley&, PortSaveState::BlockWriter&) {}

void PortBossState::RestoreRidley(CStateManager&, CRidley&, PortSaveState::BlockReader*) {}
