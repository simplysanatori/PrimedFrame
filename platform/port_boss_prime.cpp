#include "port_savestate_boss.h"

#include "MetroidPrime/Enemies/CMetroidPrimeRelay.hpp"
#include "MetroidPrime/Enemies/CMetroidPrimeStage2.hpp"

// Stub: no Metroid Prime fight state yet.
const uint8_t PortBossState::kMetroidPrimeVersion = 1;
const uint8_t PortBossState::kMetroidPrimeStage2Version = 1;

bool PortBossState::PutMetroidPrime(CStateManager&, CMetroidPrimeRelay&,
                                    PortSaveState::BlockWriter&) {
  return false;
}

void PortBossState::RestoreMetroidPrime(CStateManager&, CMetroidPrimeRelay&,
                                        PortSaveState::BlockReader*) {}

void PortBossState::PutMetroidPrimeStage2(CStateManager&, CMetroidPrimeStage2&,
                                          PortSaveState::BlockWriter&) {}

void PortBossState::RestoreMetroidPrimeStage2(CStateManager&, CMetroidPrimeStage2&,
                                              PortSaveState::BlockReader*) {}
