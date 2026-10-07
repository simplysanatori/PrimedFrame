#include "port_strings.h"
#include "port_rando_gen.h"

#include "port_ap_logic.h"
#include "port_ap_metroidprime.h"
#include "port_ap_world.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <random>
#include <sstream>

namespace PortRandoGen {
namespace {

namespace MP = PortAp::MetroidPrime;

// AP item ids past MP::kItemBase, as the port's own table (port_ap_metroidprime.cpp)
// has them. They differ from the released apworld's numbers, so never take an id
// from the Python reference.
enum Item : int {
  kPower = 0,
  kIce = 1,
  kWave = 2,
  kPlasma = 3,
  kMissileExp = 4,
  kScan = 5,
  kBomb = 6,
  kPbExp = 7,
  kFlamethrower = 8,
  kThermal = 9,
  kCharge = 10,
  kSuper = 11,
  kGrapple = 12,
  kXray = 13,
  kIceSpreader = 14,
  kSpace = 15,
  kMorph = 16,
  kBoost = 18,
  kSpider = 19,
  kGravity = 21,
  kVaria = 22,
  kPhazon = 23,
  kEtank = 24,
  kWavebuster = 28,
  kArtifactFirst = 29,
  kArtifactLast = 40,
  kLauncher = 43,
  kMainPb = 44,
  kProgPower = 49,
  kProgIce = 51,
  kProgWave = 52,
  kProgPlasma = 53,
  kItemSlots = 64,
};

constexpr size_t kLocationCount = 100;
constexpr int kMaxAttempts = 60;

// ---------------------------------------------------------------------------
// Determinism: our own PRNG and hash, since the standard distributions and
// shuffle differ between standard libraries.

uint64_t SplitMix(uint64_t& state) {
  uint64_t z = (state += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

class Rng {
public:
  explicit Rng(uint64_t seed) {
    for (uint64_t& word : mState)
      word = SplitMix(seed);
  }

  // xoshiro256**
  uint64_t Next() {
    const uint64_t result = Rotl(mState[1] * 5, 7) * 9;
    const uint64_t t = mState[1] << 17;
    mState[2] ^= mState[0];
    mState[3] ^= mState[1];
    mState[1] ^= mState[2];
    mState[0] ^= mState[3];
    mState[2] ^= t;
    mState[3] = Rotl(mState[3], 45);
    return result;
  }

  // Uniform in [0, n), without modulo bias.
  size_t Below(size_t n) {
    const uint64_t bound = n;
    const uint64_t threshold = (0 - bound) % bound;
    for (;;) {
      const uint64_t value = Next();
      if (value >= threshold)
        return static_cast< size_t >(value % bound);
    }
  }

  template < typename T >
  void Shuffle(std::vector< T >& values) {
    for (size_t i = values.size(); i > 1; --i)
      std::swap(values[i - 1], values[Below(i)]);
  }

  template < typename T >
  const T& Pick(const std::vector< T >& values) {
    return values[Below(values.size())];
  }

private:
  static uint64_t Rotl(uint64_t value, int shift) { return (value << shift) | (value >> (64 - shift)); }
  std::array< uint64_t, 4 > mState{};
};

class Hasher {
public:
  void Add(int64_t value) {
    for (int i = 0; i < 8; ++i)
      Byte(static_cast< uint8_t >(static_cast< uint64_t >(value) >> (8 * i)));
  }
  void Add(const std::string& text) {
    Add(static_cast< int64_t >(text.size()));
    for (const char c : text)
      Byte(static_cast< uint8_t >(c));
  }
  uint64_t Value() const { return mHash; }

private:
  void Byte(uint8_t byte) {
    mHash ^= byte;
    mHash *= 0x100000001B3ull;
  }
  uint64_t mHash = 0xCBF29CE484222325ull;
};

uint64_t HashInput(const Settings& s, const std::string& seedText) {
  Hasher h;
  h.Add(seedText);
  h.Add(s.requiredArtifacts);
  h.Add(s.finalBosses);
  h.Add(s.artifactHints);
  h.Add(s.missileLauncher);
  h.Add(s.mainPowerBomb);
  h.Add(s.shuffleScanVisor);
  h.Add(s.preScanElevators);
  h.Add(s.elevatorRandomization);
  h.Add(s.doorColorRandomization);
  h.Add(s.progressiveBeams);
  h.Add(s.nonVariaHeatDamage);
  h.Add(s.staggeredSuitDamage);
  h.Add(s.combatLogic);
  h.Add(s.trickDifficulty);
  h.Add(static_cast< int64_t >(s.trickAllow.size()));
  for (const std::string& name : s.trickAllow)
    h.Add(name);
  h.Add(static_cast< int64_t >(s.trickDeny.size()));
  for (const std::string& name : s.trickDeny)
    h.Add(name);
  h.Add(s.flaahgraPowerBombs);
  h.Add(s.backwardsLowerMines);
  h.Add(s.removeXray);
  h.Add(s.removeThermal);
  h.Add(s.removeHiveMecha);
  h.Add(s.springBall);
  return h.Value();
}

// ---------------------------------------------------------------------------
// Layout: elevators and door colours (Transports.py, DoorRando.py).

const char* const kAreas[] = {"Tallon Overworld", "Chozo Ruins", "Magmoor Caverns", "Phendrana Drifts",
                              "Phazon Mines"};
constexpr int kAreaCount = 5;

struct ElevatorInfo {
  int area;
  const char* name;
  const char* destination; // where it leads on the disc
};

// default_elevator_mappings, in the apworld's dictionary order (the random
// walk picks the first of equally large areas).
const ElevatorInfo kElevators[] = {
    {0, "Transport to Chozo Ruins West", "Transport to Tallon Overworld North"},
    {0, "Transport to Magmoor Caverns East", "Transport to Tallon Overworld West"},
    {0, "Transport to Chozo Ruins East", "Transport to Tallon Overworld East"},
    {0, "Transport to Chozo Ruins South", "Chozo Ruins: Transport to Tallon Overworld South"},
    {0, "Transport to Phazon Mines East", "Phazon Mines: Transport to Tallon Overworld South"},
    {1, "Transport to Tallon Overworld North", "Transport to Chozo Ruins West"},
    {1, "Transport to Magmoor Caverns North", "Transport to Chozo Ruins North"},
    {1, "Transport to Tallon Overworld East", "Transport to Chozo Ruins East"},
    {1, "Chozo Ruins: Transport to Tallon Overworld South", "Transport to Chozo Ruins South"},
    {2, "Transport to Chozo Ruins North", "Transport to Magmoor Caverns North"},
    {2, "Transport to Phendrana Drifts North", "Transport to Magmoor Caverns West"},
    {2, "Transport to Tallon Overworld West", "Transport to Magmoor Caverns East"},
    {2, "Transport to Phendrana Drifts South", "Phendrana Drifts: Transport to Magmoor Caverns South"},
    {2, "Transport to Phazon Mines West", "Phazon Mines: Transport to Magmoor Caverns South"},
    {3, "Transport to Magmoor Caverns West", "Transport to Phendrana Drifts North"},
    {3, "Phendrana Drifts: Transport to Magmoor Caverns South", "Transport to Phendrana Drifts South"},
    {4, "Phazon Mines: Transport to Tallon Overworld South", "Transport to Phazon Mines East"},
    {4, "Phazon Mines: Transport to Magmoor Caverns South", "Transport to Phazon Mines West"},
};
constexpr int kElevatorCount = 18;

using ElevatorMap = std::map< std::string, std::map< std::string, std::string > >;

ElevatorMap DefaultElevators() {
  ElevatorMap out;
  for (const ElevatorInfo& e : kElevators)
    out[kAreas[e.area]][e.name] = e.destination;
  return out;
}

// get_random_elevator_mapping: pair elevators two ways across areas, always
// starting from the area with the most unpaired ones. `saveStation1` is the
// start room's allowed_elevators rule. False when the walk strands the last
// area (the apworld would raise); the caller rolls again.
bool RandomElevators(Rng& rng, bool saveStation1, ElevatorMap& out) {
  out.clear();
  std::vector< std::vector< int > > available(kAreaCount);
  for (int i = 0; i < kElevatorCount; ++i)
    available[kElevators[i].area].push_back(i);
  std::vector< int > alive = {0, 1, 2, 3, 4};

  const auto drop = [&available, &alive](int area, int elevator) {
    std::vector< int >& list = available[area];
    list.erase(std::find(list.begin(), list.end(), elevator));
    if (list.empty())
      alive.erase(std::find(alive.begin(), alive.end(), area));
  };
  const auto pair = [&](int source, int target) {
    out[kAreas[kElevators[source].area]][kElevators[source].name] = kElevators[target].name;
    out[kAreas[kElevators[target].area]][kElevators[target].name] = kElevators[source].name;
    drop(kElevators[source].area, source);
    drop(kElevators[target].area, target);
  };

  if (saveStation1) {
    // Save Station 1's elevator (Chozo Ruins: Transport to Tallon Overworld
    // North) may only lead to three of the other areas' elevators.
    static const char* const kAllowed[] = {"Transport to Chozo Ruins East", "Transport to Magmoor Caverns West",
                                           "Transport to Chozo Ruins West"};
    const int source = 5;
    std::vector< int > options;
    for (int i = 0; i < kElevatorCount; ++i) {
      if (kElevators[i].area == kElevators[source].area)
        continue;
      for (const char* name : kAllowed) {
        if (std::string(name) == kElevators[i].name)
          options.push_back(i);
      }
    }
    if (options.empty())
      return false;
    pair(source, rng.Pick(options));
  }

  while (!alive.empty()) {
    int sourceArea = alive[0];
    for (const int area : alive) {
      if (available[area].size() > available[sourceArea].size())
        sourceArea = area;
    }
    const int source = rng.Pick(available[sourceArea]);
    std::vector< int > targets;
    for (const int area : alive) {
      if (area != sourceArea)
        targets.push_back(area);
    }
    if (targets.empty())
      return false;
    const int targetArea = rng.Pick(targets);
    pair(source, rng.Pick(available[targetArea]));
  }
  return true;
}

// generate_random_door_color_mapping: the three coloured locks shuffled until
// none keeps its own colour.
std::map< std::string, std::string > RandomLocks(Rng& rng) {
  const std::vector< std::string > locks = {"Wave Beam", "Ice Beam", "Plasma Beam"};
  std::vector< std::string > shuffled = locks;
  for (;;) {
    rng.Shuffle(shuffled);
    bool valid = true;
    for (size_t i = 0; i < locks.size(); ++i)
      valid = valid && locks[i] != shuffled[i];
    if (valid)
      break;
  }
  std::map< std::string, std::string > out;
  for (size_t i = 0; i < locks.size(); ++i)
    out[locks[i]] = shuffled[i];
  return out;
}

// The start room the apworld picks for the Normal start (init_starting_room_data).
struct StartInfo {
  const char* room;
  bool saveStation1;
  bool prefill; // the vanilla-start prefill that keeps a first-sphere walk possible
};

StartInfo PickStart(const Settings& s) {
  const bool tricks = s.trickDifficulty != -1 && !s.elevatorRandomization;
  const bool noScanElevators = !s.preScanElevators && s.shuffleScanVisor;
  // Without pre-scanned elevators and a shuffled Scan Visor, Landing Site has
  // no way out, tricks or not, so Save Station 1 is used then as well (the
  // apworld skips that case); only the vanilla-start prefill stays off.
  if (s.elevatorRandomization || noScanElevators)
    return {"Save Station 1", true, !tricks};
  return {"Landing Site", false, !tricks};
}

PortApWorld::Layout MakeLayout(const Settings& s, const StartInfo& start, const ElevatorMap& elevators,
                               const std::map< std::string, std::map< std::string, std::string > >& colors) {
  PortApWorld::Layout layout;
  layout.startRoom = start.room;
  layout.finalBosses = s.finalBosses;
  layout.requiredArtifacts = s.requiredArtifacts;
  layout.elevators = elevators;
  layout.doorColorRandomization = s.doorColorRandomization != 0;
  layout.hasDoorColors = !colors.empty();
  layout.doorColors = colors;
  layout.removeHiveMecha = s.removeHiveMecha;
  layout.backwardsLowerMines = s.backwardsLowerMines;
  layout.flaahgraPowerBombs = s.flaahgraPowerBombs;
  return layout;
}

PortApLogic::Options LogicFor(const Settings& s, const PortApWorld::Layout& layout) {
  PortApLogic::Options options;
  options.trickDifficulty = s.trickDifficulty;
  options.combatLogic = s.combatLogic;
  options.removeXray = s.removeXray;
  options.removeThermal = s.removeThermal;
  options.flaahgraPowerBombs = s.flaahgraPowerBombs;
  options.progressiveBeams = s.progressiveBeams;
  options.mainMissile = s.missileLauncher;
  options.mainPowerBomb = s.mainPowerBomb;
  options.variaOnlyHeat = s.nonVariaHeatDamage;
  options.preScanElevators = s.preScanElevators;
  options.trickAllow = s.trickAllow;
  options.trickDeny = s.trickDeny;
  PortApWorld::FillLogic(layout, options);
  return options;
}

// ---------------------------------------------------------------------------
// Items (ItemPool.py).

struct PoolItem {
  int id;
  bool progression;
};

int BeamItem(const Settings& s, int beam) {
  if (!s.progressiveBeams)
    return beam;
  switch (beam) {
  case kPower: return kProgPower;
  case kIce: return kProgIce;
  case kWave: return kProgWave;
  default: return kProgPlasma;
  }
}

int LauncherItem(const Settings& s) { return s.missileLauncher ? kLauncher : kMissileExp; }
int MainPbItem(const Settings& s) { return s.mainPowerBomb ? kMainPb : kPbExp; }

// The start room's vanilla-loadout prefill: location name -> item. One rule set
// is picked at random, as StartRoomData.py does.
std::vector< std::pair< const char*, int > > PickPrefill(const Settings& s, const StartInfo& start, Rng& rng) {
  if (!start.prefill)
    return {};
  const char* const hive = "Chozo Ruins: Hive Totem";
  const char* const beetle = "Chozo Ruins: Ruined Shrine - Plated Beetle";
  const char* const gallery = "Chozo Ruins: Ruined Gallery - Missile Wall";
  using Rule = std::vector< std::pair< const char*, int > >;
  std::vector< Rule > rules = {
      {{hive, kLauncher}, {beetle, kMorph}, {gallery, kBomb}},
      {{hive, kLauncher}, {beetle, kBomb}, {gallery, kMorph}},
  };
  // Landing Site has a second loadout with a smaller prefill; Save Station 1 doesn't.
  if (!start.saveStation1 && rng.Below(2) == 1)
    rules = {{{hive, kLauncher}, {gallery, kMorph}}};
  Rule rule = rules[rng.Below(rules.size())];
  for (auto& entry : rule) {
    if (entry.second == kLauncher)
      entry.second = LauncherItem(s);
  }
  return rule;
}

std::vector< int > StartInventory(const Settings& s) {
  std::vector< int > items = {BeamItem(s, kPower)};
  if (!s.shuffleScanVisor)
    items.push_back(kScan);
  return items;
}

std::vector< PoolItem > BuildPool(const Settings& s, const std::vector< int >& prefilled,
                                  const std::vector< int >& start) {
  std::vector< PoolItem > items;
  for (int artifact = kArtifactFirst; artifact <= kArtifactLast; ++artifact)
    items.push_back({artifact, true});
  for (const int id : {kMorph, kBomb, kThermal, kXray, kScan, kGrapple, kSpace, kSpider, kBoost, kVaria,
                       kGravity, kPhazon})
    items.push_back({id, true});
  for (int i = 0; i < 8; ++i)
    items.push_back({kMissileExp, true});
  items.push_back({LauncherItem(s), true});
  for (int i = 0; i < 4; ++i)
    items.push_back({kPbExp, false});
  items.push_back({MainPbItem(s), true});
  for (int i = 0; i < 14; ++i)
    items.push_back({kEtank, i < 8});
  if (s.progressiveBeams) {
    for (const int id : {kProgPower, kProgIce, kProgWave, kProgPlasma}) {
      for (int i = 0; i < 3; ++i)
        items.push_back({id, true});
    }
  } else {
    for (const int id : {kPower, kWave, kIce, kPlasma, kCharge, kSuper, kWavebuster, kIceSpreader, kFlamethrower})
      items.push_back({id, true});
  }
  const auto remove = [&items](int id) {
    for (size_t i = 0; i < items.size(); ++i) {
      if (items[i].id == id) {
        items.erase(items.begin() + static_cast< std::ptrdiff_t >(i));
        return;
      }
    }
  };
  for (const int id : prefilled)
    remove(id);
  for (const int id : start)
    remove(id);
  while (items.size() + prefilled.size() < kLocationCount)
    items.push_back({kMissileExp, false});
  return items;
}

// ---------------------------------------------------------------------------
// Reachability.

using Counts = std::array< int, kItemSlots >;

PortApLogic::Items ToItems(const Counts& counts) {
  PortApLogic::Items items;
  for (size_t i = 0; i < counts.size(); ++i) {
    if (counts[i] > 0)
      items[MP::kItemBase + static_cast< int64_t >(i)] = counts[i];
  }
  return items;
}

// Collects what the player can reach from `base`, picking up the items in
// `placed` (-1 for an empty location) as their locations come in logic.
// `reached` marks every location in logic; `spheres` (optional) lists the
// locations of each sweep.
void Closure(const PortApLogic::Options& options, Counts have, const std::vector< int >& placed,
             std::vector< char >& reached, Counts* finalItems = nullptr,
             std::vector< std::vector< int > >* spheres = nullptr) {
  reached.assign(kLocationCount, 0);
  for (;;) {
    const std::vector< PortApLogic::Level > levels = PortApLogic::Evaluate(options, ToItems(have));
    std::vector< int > sphere;
    for (size_t i = 0; i < kLocationCount; ++i) {
      if (reached[i] || levels[i] != PortApLogic::Level::Normal)
        continue;
      reached[i] = 1;
      sphere.push_back(static_cast< int >(i));
    }
    if (sphere.empty())
      break;
    for (const int location : sphere) {
      if (placed[static_cast< size_t >(location)] >= 0)
        ++have[static_cast< size_t >(placed[static_cast< size_t >(location)])];
    }
    if (spheres != nullptr)
      spheres->push_back(sphere);
  }
  if (finalItems != nullptr)
    *finalItems = have;
}

// Regions.py's Mission Complete rule, from the items alone (the Artifact
// Temple being in logic is checked by the caller).
bool CanComplete(const Settings& s, const Counts& have) {
  const auto has = [&have](int id) { return have[static_cast< size_t >(id)] > 0; };
  const auto count = [&have](int id) { return have[static_cast< size_t >(id)]; };
  int artifacts = 0;
  for (int id = kArtifactFirst; id <= kArtifactLast; ++id)
    artifacts += has(id) ? 1 : 0;
  if (artifacts < s.requiredArtifacts)
    return false;
  const bool missile = s.missileLauncher ? has(kLauncher) : has(kMissileExp);
  if (!missile)
    return false;
  if (s.finalBosses == 3)
    return true;
  const bool power = has(kPower) || has(kProgPower);
  const bool ice = has(kIce) || has(kProgIce);
  const bool wave = has(kWave) || has(kProgWave);
  const bool plasma = has(kPlasma) || has(kProgPlasma);
  const bool charge = has(kCharge) || count(kProgPower) >= 2 || count(kProgIce) >= 2 || count(kProgWave) >= 2 ||
                      count(kProgPlasma) >= 2;
  const auto combat = [&](int normalTanks, int minimalTanks) {
    if (s.combatLogic < 0)
      return true;
    return count(kEtank) >= (s.combatLogic == 0 ? normalTanks : minimalTanks) && charge;
  };
  const bool ridley = combat(8, 8);
  if (s.finalBosses == 1) {
    const bool superMissile = power && missile && ((has(kCharge) && has(kSuper)) || count(kProgPower) >= 3);
    return (plasma || superMissile) && ridley;
  }
  const bool prime = combat(8, 5);
  const bool xray = s.removeXray == 2 || has(kXray);
  const bool thermal = s.removeThermal == 2 || has(kThermal);
  return prime && ridley && has(kPhazon) && plasma && wave && ice && power && xray && thermal;
}

// ---------------------------------------------------------------------------
// Output.

void AppendNames(std::ostringstream& text, const char* key, const std::vector< std::string >& names) {
  text << ",\"" << key << "\":[";
  for (size_t i = 0; i < names.size(); ++i)
    text << (i != 0 ? "," : "") << port::JsonQuote(names[i]);
  text << ']';
}

void AppendMapping(std::ostringstream& text, const ElevatorMap& mapping, bool withArea) {
  bool firstArea = true;
  for (const auto& area : mapping) {
    text << (firstArea ? "" : ",") << port::JsonQuote(area.first) << ":{";
    firstArea = false;
    if (withArea)
      text << "\"area\":" << port::JsonQuote(area.first) << ",\"type_mapping\":{";
    bool first = true;
    for (const auto& entry : area.second) {
      text << (first ? "" : ",") << port::JsonQuote(entry.first) << ':' << port::JsonQuote(entry.second);
      first = false;
    }
    text << (withArea ? "}}" : "}");
  }
}

const char* ItemLabel(int64_t id) {
  const char* name = MP::ItemName(id);
  return name != nullptr ? name : "?";
}

struct Attempt {
  PortApWorld::Layout layout;
  std::vector< int > placed; // location index -> item offset
  std::vector< int > start;
  std::vector< std::vector< int > > spheres;
  Counts finalItems{};
};

std::string SlotData(const Settings& s, const Attempt& a, const std::vector< int64_t >& locationIds) {
  std::ostringstream text;
  const auto flag = [](bool value) { return value ? 1 : 0; };
  text << "{\"missile_launcher\":" << flag(s.missileLauncher) << ",\"main_power_bomb\":" << flag(s.mainPowerBomb)
       << ",\"progressive_beam_upgrades\":" << flag(s.progressiveBeams)
       << ",\"shuffle_scan_visor\":" << flag(s.shuffleScanVisor)
       << ",\"pre_scan_elevators\":" << flag(s.preScanElevators)
       << ",\"elevator_randomization\":" << flag(s.elevatorRandomization)
       << ",\"door_color_randomization\":" << s.doorColorRandomization
       << ",\"trick_difficulty\":" << s.trickDifficulty << ",\"combat_logic_difficulty\":" << s.combatLogic
       << ",\"remove_xray_requirements\":" << s.removeXray << ",\"remove_thermal_requirements\":" << s.removeThermal
       << ",\"flaahgra_power_bombs\":" << flag(s.flaahgraPowerBombs)
       << ",\"non_varia_heat_damage\":" << flag(s.nonVariaHeatDamage)
       << ",\"staggered_suit_damage\":" << s.staggeredSuitDamage
       << ",\"backwards_lower_mines\":" << flag(s.backwardsLowerMines)
       << ",\"remove_hive_mecha\":" << flag(s.removeHiveMecha) << ",\"spring_ball\":" << flag(s.springBall)
       << ",\"required_artifacts\":" << s.requiredArtifacts << ",\"final_bosses\":" << s.finalBosses
       << ",\"artifact_hints\":" << flag(s.artifactHints);
  AppendNames(text, "trick_allow_list", s.trickAllow);
  AppendNames(text, "trick_deny_list", s.trickDeny);
  text << ",\"starting_room_name\":" << port::JsonQuote(a.layout.startRoom) << ",\"elevator_mapping\":{";
  AppendMapping(text, a.layout.elevators, false);
  text << '}';
  if (a.layout.hasDoorColors) {
    text << ",\"door_color_mapping\":{";
    AppendMapping(text, a.layout.doorColors, true);
    text << '}';
  }
  if (s.artifactHints) {
    text << ",\"artifact_locations\":{";
    bool first = true;
    for (int artifact = kArtifactFirst; artifact <= kArtifactLast; ++artifact) {
      for (size_t i = 0; i < kLocationCount; ++i) {
        if (a.placed[i] != artifact)
          continue;
        text << (first ? "" : ",") << port::JsonQuote(ItemLabel(MP::kItemBase + artifact)) << ":["
             << locationIds[i] << ",1]";
        first = false;
      }
    }
    text << '}';
  }
  text << '}';
  return text.str();
}

std::string Spoiler(const std::string& name, const Settings& s, const Attempt& a, const MP::Location* locations) {
  std::ostringstream text;
  text << "Metroid Prime seed " << name << "\n\nStart: " << a.layout.startRoom << "\nStarting items:";
  for (const int id : a.start)
    text << ' ' << ItemLabel(MP::kItemBase + id) << ';';
  text << '\n';
  if (s.elevatorRandomization) {
    text << "\nElevators:\n";
    for (const auto& area : a.layout.elevators) {
      for (const auto& entry : area.second)
        text << "  " << area.first << ": " << entry.first << " -> " << entry.second << '\n';
    }
  }
  if (a.layout.hasDoorColors) {
    text << "\nDoor colours:\n";
    for (const auto& area : a.layout.doorColors) {
      text << "  " << area.first << ':';
      for (const auto& entry : area.second)
        text << ' ' << entry.first << " -> " << entry.second << ';';
      text << '\n';
    }
  }
  text << "\nPlaythrough:\n";
  for (size_t sphere = 0; sphere < a.spheres.size(); ++sphere) {
    bool header = false;
    for (const int location : a.spheres[sphere]) {
      const int item = a.placed[static_cast< size_t >(location)];
      // Expansions and tanks never gate anything the spoiler needs to show.
      if (item == kMissileExp || item == kPbExp || item == kEtank)
        continue;
      if (!header)
        text << "  Sphere " << sphere << ":\n";
      header = true;
      text << "    " << locations[location].name << ": " << ItemLabel(MP::kItemBase + item) << '\n';
    }
  }
  text << "\nLocations:\n";
  for (size_t i = 0; i < kLocationCount; ++i)
    text << "  " << locations[i].name << ": " << ItemLabel(MP::kItemBase + a.placed[i]) << '\n';
  return text.str();
}

// ---------------------------------------------------------------------------
// One attempt: layout, assumed fill, independent check.

bool TryOnce(const Settings& s, Rng& rng, Attempt& out, std::string& why) {
  size_t locationCount = 0;
  const MP::Location* locations = MP::Locations(locationCount);
  if (locationCount != kLocationCount) {
    why = "unexpected location table";
    return false;
  }

  const StartInfo start = PickStart(s);
  ElevatorMap elevators;
  if (s.elevatorRandomization) {
    if (!RandomElevators(rng, start.saveStation1, elevators)) {
      why = "elevator layout stranded an area";
      return false;
    }
  } else {
    elevators = DefaultElevators();
  }
  ElevatorMap colors;
  if (s.doorColorRandomization == 1) {
    const auto mapping = RandomLocks(rng);
    for (const char* area : kAreas)
      colors[area] = mapping;
  } else if (s.doorColorRandomization == 2) {
    for (const char* area : kAreas)
      colors[area] = RandomLocks(rng);
  }
  out.layout = MakeLayout(s, start, elevators, colors);
  const PortApLogic::Options options = LogicFor(s, out.layout);

  // The start inventory and the prefilled locations.
  const auto prefill = PickPrefill(s, start, rng);
  out.start = StartInventory(s);
  out.placed.assign(kLocationCount, -1);
  std::vector< int > prefilledItems;
  for (const auto& entry : prefill) {
    for (size_t i = 0; i < kLocationCount; ++i) {
      if (std::string(locations[i].name) == entry.first) {
        out.placed[i] = entry.second;
        prefilledItems.push_back(entry.second);
      }
    }
  }
  std::vector< PoolItem > pool = BuildPool(s, prefilledItems, out.start);

  Counts base{};
  for (const int id : out.start)
    ++base[static_cast< size_t >(id)];
  std::vector< char > reached;

  // Every location must be reachable with every item in hand; a layout that
  // fails this can't be saved by any placement.
  {
    Counts everything = base;
    for (const PoolItem& item : pool)
      ++everything[static_cast< size_t >(item.id)];
    for (const int id : prefilledItems)
      ++everything[static_cast< size_t >(id)];
    const std::vector< int > nothing(kLocationCount, -1);
    Closure(options, everything, nothing, reached);
    if (std::count(reached.begin(), reached.end(), 1) != static_cast< std::ptrdiff_t >(kLocationCount)) {
      why = "layout leaves a location out of logic";
      return false;
    }
  }

  std::vector< int > progression;
  std::vector< int > rest;
  for (const PoolItem& item : pool)
    (item.progression ? progression : rest).push_back(item.id);
  rng.Shuffle(progression);

  // Scan Visor early when the elevators need it: any location in logic before
  // other items come in (prefilled ones count, they're placed already).
  if (s.shuffleScanVisor && !s.preScanElevators) {
    const auto it = std::find(progression.begin(), progression.end(), static_cast< int >(kScan));
    if (it != progression.end()) {
      progression.erase(it);
      Closure(options, base, out.placed, reached);
      // The prefill may hold every sphere-0 location (Save Station 1 reaches
      // only the Hive Totem). The Scan Visor then takes it and the prefilled
      // item goes back among the progression items.
      std::vector< int > candidates;
      for (size_t i = 0; i < kLocationCount; ++i) {
        if (reached[i] && out.placed[i] < 0)
          candidates.push_back(static_cast< int >(i));
      }
      if (candidates.empty()) {
        for (size_t i = 0; i < kLocationCount; ++i) {
          if (reached[i])
            candidates.push_back(static_cast< int >(i));
        }
      }
      if (candidates.empty()) {
        // Nothing opens before some other item (tricks start without the
        // prefill): the Scan Visor is placed like the rest.
        progression.push_back(kScan);
      } else {
        const size_t chosen = static_cast< size_t >(rng.Pick(candidates));
        if (out.placed[chosen] >= 0)
          progression.push_back(out.placed[chosen]);
        out.placed[chosen] = kScan;
      }
    }
  }

  // Assumed fill: each progression item goes where the player could stand
  // holding every progression item still unplaced.
  Counts assumed = base;
  for (const int id : progression)
    ++assumed[static_cast< size_t >(id)];
  for (const int item : progression) {
    --assumed[static_cast< size_t >(item)];
    Closure(options, assumed, out.placed, reached);
    std::vector< int > candidates;
    for (size_t i = 0; i < kLocationCount; ++i) {
      if (reached[i] && out.placed[i] < 0)
        candidates.push_back(static_cast< int >(i));
    }
    if (candidates.empty()) {
      why = "no reachable location for a progression item";
      return false;
    }
    out.placed[static_cast< size_t >(rng.Pick(candidates))] = item;
  }

  // The rest anywhere.
  std::vector< int > open;
  for (size_t i = 0; i < kLocationCount; ++i) {
    if (out.placed[i] < 0)
      open.push_back(static_cast< int >(i));
  }
  rng.Shuffle(open);
  rng.Shuffle(rest);
  if (open.size() != rest.size()) {
    why = "pool does not fill the locations";
    return false;
  }
  for (size_t i = 0; i < open.size(); ++i)
    out.placed[static_cast< size_t >(open[i])] = rest[i];

  // The check proper: a walk from the start inventory over the final placements.
  out.spheres.clear();
  Closure(options, base, out.placed, reached, &out.finalItems, &out.spheres);
  if (std::count(reached.begin(), reached.end(), 1) != static_cast< std::ptrdiff_t >(kLocationCount)) {
    why = "a location is out of reach in the final walk";
    return false;
  }
  if (!CanComplete(s, out.finalItems)) {
    why = "the final inventory can't finish the game";
    return false;
  }
  return true;
}

std::string RandomName() {
  std::random_device device;
  char text[16];
  std::snprintf(text, sizeof(text), "%08x", static_cast< unsigned >(device()));
  return text;
}

} // namespace

bool Generate(const Settings& settings, const std::string& seedText, Seed& out, std::string& error) {
  Settings s = settings;
  s.requiredArtifacts = std::clamp(s.requiredArtifacts, 1, 12);
  s.finalBosses = std::clamp(s.finalBosses, 0, 3);
  s.doorColorRandomization = std::clamp(s.doorColorRandomization, 0, 2);
  s.combatLogic = std::clamp(s.combatLogic, -1, 1);
  s.trickDifficulty = std::clamp(s.trickDifficulty, -1, 2);
  s.removeXray = std::clamp(s.removeXray, 0, 2);
  s.removeThermal = std::clamp(s.removeThermal, 0, 2);

  const std::string name = seedText.empty() ? RandomName() : seedText;
  const uint64_t hash = HashInput(s, name);
  size_t locationCount = 0;
  const MP::Location* locations = MP::Locations(locationCount);
  std::vector< int64_t > locationIds;
  for (size_t i = 0; i < locationCount; ++i)
    locationIds.push_back(locations[i].id);

  std::string why;
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    Rng rng(hash + static_cast< uint64_t >(attempt) * 0x9E3779B97F4A7C15ull);
    Attempt result;
    if (!TryOnce(s, rng, result, why))
      continue;
    out = Seed();
    out.name = name;
    out.settings = s;
    for (const int id : result.start)
      out.startItems.push_back(MP::kItemBase + id);
    for (size_t i = 0; i < locationCount; ++i)
      out.placements[locationIds[i]] = MP::kItemBase + result.placed[i];
    out.slotData = SlotData(s, result, locationIds);
    out.spoiler = Spoiler(name, s, result, locations);
    error.clear();
    return true;
  }
  error = "no beatable seed found in " + std::to_string(kMaxAttempts) + " attempts (" + why + ")";
  return false;
}

} // namespace PortRandoGen
