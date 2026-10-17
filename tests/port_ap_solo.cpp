#include "port_ap_metroidprime.h"
#include "port_ap_protocol.h"
#include "port_ap_solo.h"
#include "port_rando_gen.h"
#include "port_test.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

unsigned long TestProcessId() {
#ifdef _WIN32
  return static_cast<unsigned long>(_getpid());
#else
  return static_cast<unsigned long>(getpid());
#endif
}

namespace Mp = PortAp::MetroidPrime;
using PortAp::Protocol::ItemGrant;
using PortAp::Protocol::Session;

PortJson::Value Packet(const std::string& json) {
  PortJson::Value value;
  size_t offset = 0;
  const char* reason = nullptr;
  if (!PortJson::Parse(json, value, offset, &reason))
    CHECK_MSG(false, "server packet is JSON");
  return value;
}

// What the client does with the server's messages: feeds them to the session
// and answers with whatever the session sends back, until the server is quiet.
std::vector<ItemGrant> Pump(PortApSolo::Server& server, Session& session) {
  std::vector<ItemGrant> grants;
  std::string message;
  while (server.ReceiveText(message, 20)) {
    const PortJson::Value root = Packet(message);
    CHECK_MSG(root.IsArray(), "the server sends arrays of commands");
    for (const PortJson::Value& command : root.AsArray()) {
      std::vector<std::string> outgoing;
      session.HandlePacket(command, outgoing, grants);
      if (command.StringOr("cmd") == "RoomInfo")
        outgoing.push_back(session.BuildConnect());
      for (const std::string& packet : outgoing)
        CHECK_MSG(server.SendText("[" + packet + "]"), "the server takes a packet");
    }
  }
  return grants;
}

Session MakeSession(const PortAp::Protocol::State& state = PortAp::Protocol::State()) {
  const PortAp::Protocol::Config config =
      PortAp::Protocol::ParseConfig(R"({"server":"solo:t","slot":"Samus"})");
  CHECK_MSG(config.valid && config.builtin, "a solo config uses the built-in tables");
  return Session(config, state);
}

} // namespace

int main() {
  const std::filesystem::path testDir =
      std::filesystem::temp_directory_path() / ("mp-solo-test-" + std::to_string(TestProcessId()));
  std::filesystem::remove_all(testDir);
  std::filesystem::create_directories(testDir);
  // Before anything asks for the user folder, which is resolved only once.
  const std::string userPath = testDir.string();
#ifdef _WIN32
  _putenv_s("MP_USER_PATH", userPath.c_str());
#else
  setenv("MP_USER_PATH", userPath.c_str(), 1);
#endif

  // Settings text.
  {
    PortRandoGen::Settings settings;
    PortRandoGen::Settings parsed;
    CHECK_MSG(PortRandoGen::ParseSettings(PortRandoGen::SettingsText(settings), parsed) && parsed == settings,
              "the default settings round-trip");
    settings.requiredArtifacts = 7;
    settings.finalBosses = 2;
    settings.artifactHints = false;
    settings.missileLauncher = true;
    settings.mainPowerBomb = true;
    settings.shuffleScanVisor = true;
    settings.preScanElevators = false;
    settings.elevatorRandomization = true;
    settings.doorColorRandomization = 2;
    settings.progressiveBeams = true;
    settings.nonVariaHeatDamage = false;
    settings.staggeredSuitDamage = 2;
    settings.combatLogic = -1;
    settings.trickDifficulty = 2;
    settings.trickAllow = {"Wallcrawl", "Say \"hi\""};
    settings.trickDeny = {"Bomb Jump"};
    settings.flaahgraPowerBombs = true;
    settings.backwardsLowerMines = true;
    settings.removeXray = 1;
    settings.removeThermal = 2;
    settings.removeHiveMecha = true;
    settings.springBall = false;
    settings.startingRoom = 2;
    settings.randomizeStartingBeam = true;
    settings.blastShieldRandomization = 2;
    settings.blastShieldFrequency = 6;
    settings.blastShieldAvailableTypes = 1;
    settings.lockedDoorCount = 2;
    settings.includePowerBeamDoors = true;
    settings.includeMorphBallBombDoors = true;
    CHECK_MSG(PortRandoGen::ParseSettings(PortRandoGen::SettingsText(settings), parsed) && parsed == settings,
              "changed settings round-trip");
    PortRandoGen::Settings other = settings;
    other.trickDeny.push_back("x");
    CHECK_MSG(other != settings, "a different list is a different setting");

    CHECK_MSG(PortRandoGen::ParseSettings("{}", parsed) && parsed == PortRandoGen::Settings(),
              "missing options are the defaults");
    CHECK_MSG(PortRandoGen::ParseSettings(
              R"({"required_artifacts":99,"final_bosses":-4,"trick_difficulty":50,"spring_ball":false})", parsed) &&
              parsed.requiredArtifacts == 12 && parsed.finalBosses == 0 && parsed.trickDifficulty == 2 &&
              !parsed.springBall,
          "out of range options are clamped");
    CHECK_MSG(!PortRandoGen::ParseSettings("[1,2]", parsed) && !PortRandoGen::ParseSettings("{", parsed),
              "malformed settings are refused");
  }

  // A small seed with known placements.
  size_t locationCount = 0;
  const Mp::Location* locations = Mp::Locations(locationCount);
  CHECK_MSG(locationCount >= 4, "the location table has locations");
  PortRandoGen::Seed seed;
  seed.name = "t1 /weird";
  seed.settings.requiredArtifacts = 5;
  seed.slotData = R"({"required_artifacts":5,"missile_launcher":0,"note":"a\"b","n":1.5})";
  seed.startItems = {Mp::kItemBase + Mp::kSpringBall};
  const int64_t items[4] = {Mp::kItemBase + Mp::kEnergyTank, Mp::kItemBase + Mp::kMissileLauncher,
                            Mp::kItemBase + Mp::kMissileExpansion, Mp::kItemBase + Mp::kArtifactTruth};
  for (int i = 0; i < 4; ++i)
    seed.placements[locations[i].id] = items[i];
  seed.spoiler = "line one\nline \"two\"\n";

  // Save and Load.
  {
    std::string error;
    CHECK_MSG(PortRandoGen::Save(seed, error), "a seed saves");
    const std::string path = PortRandoGen::SeedPath(seed.name);
    CHECK_MSG(path.find("weird") != std::string::npos && path.find(' ') == std::string::npos &&
                  path.find("t1 ") == std::string::npos &&
                  std::filesystem::path(path).parent_path() == std::filesystem::path(PortRandoGen::SeedDirectory()),
              "the seed's file name is made safe");
    CHECK_MSG(std::filesystem::exists(path) && !std::filesystem::exists(path + ".tmp"),
              "the seed file exists and no temporary is left");
    PortRandoGen::Seed loaded;
    CHECK_MSG(PortRandoGen::Load(path, loaded, error), "a seed loads");
    CHECK_MSG(loaded.name == seed.name && loaded.settings == seed.settings &&
                  loaded.startItems == seed.startItems && loaded.placements == seed.placements &&
                  loaded.spoiler == seed.spoiler,
              "a loaded seed matches the saved one");
    const PortJson::Value slot = Packet(loaded.slotData);
    CHECK_MSG(slot.Find("required_artifacts") != nullptr && slot.Find("required_artifacts")->AsInt() == 5 &&
                  slot.StringOr("note") == "a\"b" && slot.Find("n") != nullptr && slot.Find("n")->AsNumber() == 1.5,
              "slot data survives the round trip");
    CHECK_MSG(!PortRandoGen::Load((testDir / "nothing.json").string(), loaded, error) && !error.empty(),
              "a missing seed file is an error");
    {
      std::ofstream(testDir / "bad.json") << "{\"version\":2}";
    }
    CHECK_MSG(!PortRandoGen::Load((testDir / "bad.json").string(), loaded, error), "an unknown version is refused");
    PortRandoGen::Seed broken = seed;
    broken.slotData = "[1]";
    CHECK_MSG(!PortRandoGen::Save(broken, error), "slot data that is not an object is not saved");
    PortRandoGen::Seed again = seed;
    CHECK_MSG(PortRandoGen::Save(again, error) && again.name == seed.name, "the same seed saved again keeps its name");
    PortRandoGen::Seed other = seed;
    other.settings.requiredArtifacts = 6;
    CHECK_MSG(PortRandoGen::Save(other, error) && other.name == seed.name + "-2" &&
                  PortRandoGen::Load(PortRandoGen::SeedPath(seed.name), loaded, error) &&
                  loaded.settings == seed.settings,
              "a different seed with the same name gets a new one and leaves the old file");
    std::filesystem::remove(PortRandoGen::SeedPath(other.name));
    CHECK_MSG(PortRandoGen::SeedPath("x.state") != PortApSolo::StatePath("x"),
              "a seed named x.state doesn't take seed x's state file");
    PortRandoGen::Seed longSeed = seed;
    longSeed.name = std::string(80, 'L');
    PortRandoGen::Seed longOther = longSeed;
    longOther.settings.requiredArtifacts = 7;
    CHECK_MSG(PortRandoGen::Save(longSeed, error) && PortRandoGen::Save(longOther, error) &&
                  PortRandoGen::SeedPath(longOther.name) != PortRandoGen::SeedPath(longSeed.name),
              "a long name that is taken still gets a free one");
    std::filesystem::remove(PortRandoGen::SeedPath(longSeed.name));
    std::filesystem::remove(PortRandoGen::SeedPath(longOther.name));
  }

  // The handshake through the real session.
  const std::string statePath = PortApSolo::StatePath(seed.name);
  CHECK_MSG(statePath.find(".state.json") != std::string::npos, "the state file is beside the seed");
  std::filesystem::remove(statePath);
  PortAp::Protocol::State saved;
  {
    PortApSolo::Server server;
    std::string error;
    CHECK_MSG(server.OpenSeed(seed, statePath, error), "the solo server opens");
    Session session = MakeSession();
    const std::vector<ItemGrant> grants = Pump(server, session);
    CHECK_MSG(session.HandshakeComplete(), "Connected is parsed");
    CHECK_MSG(session.GetSlotData().received && session.GetSlotData().requiredArtifacts == 5,
              "the seed's slot data reaches the session");
    CHECK_MSG(grants.size() == 1 && grants[0].itemId == seed.startItems[0] && grants[0].index == 0,
              "the start item is granted at index 0");
    int64_t item = 0;
    bool sameGame = false;
    int64_t flags = -1;
    CHECK_MSG(session.ScoutedAt(locations[1].id, item, sameGame, &flags) && item == items[1] && sameGame &&
                  flags == 1,
              "scouting a main item says progression");
    CHECK_MSG(session.ScoutedAt(locations[0].id, item, sameGame, &flags) && item == items[0] && flags == 0,
              "scouting an energy tank says filler");

    // A check grants its item at the next index.
    CHECK_MSG(server.SendText("[" + Session::BuildLocationChecks({locations[1].id}) + "]"), "a check is sent");
    std::vector<ItemGrant> next = Pump(server, session);
    CHECK_MSG(next.size() == 1 && next[0].itemId == items[1] && next[0].index == 1,
              "a check grants its item at index 1");
    // The same check again, and an unknown location, grant nothing.
    CHECK_MSG(server.SendText("[" + Session::BuildLocationChecks({locations[1].id, 42}) + "]"), "a repeat is sent");
    CHECK_MSG(Pump(server, session).empty(), "a repeated check grants nothing");
    CHECK_MSG(server.SendText("[" + Session::BuildLocationChecks({locations[3].id}) + "]"), "a second check is sent");
    next = Pump(server, session);
    CHECK_MSG(next.size() == 1 && next[0].itemId == items[3] && next[0].index == 2,
              "the next check grants at index 2");
    CHECK_MSG(server.CheckedLocations() == std::vector<int64_t>({locations[1].id, locations[3].id}),
              "the server keeps the checks in order");

    // Sync replays everything; the session has it already.
    CHECK_MSG(server.SendText("[" + Session::BuildSync() + "]"), "a sync is sent");
    CHECK_MSG(Pump(server, session).empty(), "a sync grants nothing twice");

    // Chat, commands, the goal and the data storage keys.
    CHECK_MSG(server.SendText(R"([{"cmd":"Say","text":"hello"}])"), "chat is sent");
    CHECK_MSG(server.SendText(R"([{"cmd":"Say","text":"!hint"}])"), "a command is sent");
    CHECK_MSG(server.SendText("[" + Session::BuildGoal() + "]"), "the goal is sent");
    Pump(server, session);
    PortAp::Protocol::ChatLine line;
    std::vector<std::string> chat;
    while (session.TakeChatLine(line))
      chat.push_back(line.type + "|" + line.text);
    const auto has = [&chat](const std::string& wanted) {
      return std::any_of(chat.begin(), chat.end(),
                         [&wanted](const std::string& entry) { return entry.find(wanted) != std::string::npos; });
    };
    CHECK_MSG(has("Chat|Samus: hello"), "chat is echoed");
    CHECK_MSG(has("CommandResult|Commands are not available in a solo game."), "commands are declined");
    CHECK_MSG(has("Goal|Samus has completed their goal."), "the goal is announced");

    saved = session.GetState();
    CHECK_MSG(saved.nextItemIndex == 3 && saved.seed == "solo-t1 /weird", "the session recorded the seed and index");
    server.Close();
    std::string message;
    CHECK_MSG(!server.IsOpen() && !server.SendText("[]") && !server.ReceiveText(message, 1),
              "a closed server takes and gives nothing");
  }

  // A relaunch: the state file brings the checks back in order.
  {
    PortApSolo::Server server;
    std::string error;
    CHECK_MSG(server.Open(seed.name, error), "the seed opens by name") ;
    CHECK_MSG(server.CheckedLocations() == std::vector<int64_t>({locations[1].id, locations[3].id}),
              "the checks come back from the state file");

    // The same client session reconnecting: the indices repeat, nothing is granted again.
    Session session = MakeSession(saved);
    CHECK_MSG(Pump(server, session).empty(), "a reconnect does not grant items twice");
    CHECK_MSG(session.GetState().nextItemIndex == 3, "the item index is unchanged by the reconnect");
    // The client repeats its checks after Connected; the server has them.
    CHECK_MSG(server.SendText("[" + Session::BuildLocationChecks(saved.checkedLocations) + "]"), "checks resent");
    CHECK_MSG(Pump(server, session).empty(), "resent checks grant nothing");
    // A new location after the reload carries on at the right index.
    CHECK_MSG(server.SendText("[" + Session::BuildLocationChecks({locations[0].id}) + "]"), "a new check is sent");
    const std::vector<ItemGrant> next = Pump(server, session);
    CHECK_MSG(next.size() == 1 && next[0].itemId == items[0] && next[0].index == 3,
              "a check after the reload grants at index 3");
  }
  {
    // A fresh client (a new save card) gets the whole inventory, in order.
    PortApSolo::Server server;
    std::string error;
    CHECK_MSG(server.Open(seed.name, error), "the seed opens again");
    Session session = MakeSession();
    const std::vector<ItemGrant> grants = Pump(server, session);
    CHECK_MSG(grants.size() == 4 && grants[0].itemId == seed.startItems[0] && grants[1].itemId == items[1] &&
                  grants[2].itemId == items[3] && grants[3].itemId == items[0],
              "a fresh client gets the start items, then every check's item in order");
    for (size_t i = 0; i < grants.size(); ++i)
      CHECK_MSG(grants[i].index == static_cast<int64_t>(i), "the indices count up from 0");
  }

  // Wrong slot and server strings.
  {
    PortApSolo::Server server;
    std::string error;
    CHECK_MSG(server.OpenSeed(seed, "", error), "a seed opens without a state file");
    std::string message;
    CHECK_MSG(server.ReceiveText(message, 20) && message.find("solo-t1 /weird") != std::string::npos &&
                  message.find("\"password\":false") != std::string::npos,
              "RoomInfo comes first");
    CHECK_MSG(server.SendText(R"([{"cmd":"Connect","name":"Someone","game":"Metroid Prime"}])"), "connect sent");
    CHECK_MSG(server.ReceiveText(message, 20) && message.find("ConnectionRefused") != std::string::npos,
              "another slot name is refused");
    CHECK_MSG(!server.ReceiveText(message, 5) && std::string(server.Error()) == "receive timed out",
              "an idle server times out");
    std::string seedName;
    CHECK_MSG(PortApSolo::ParseServer("solo:abc", seedName) && seedName == "abc" &&
                  !PortApSolo::ParseServer("solo:", seedName) && !PortApSolo::ParseServer("ws://solo:x", seedName),
              "solo server strings parse");
    PortApSolo::Server missing;
    CHECK_MSG(!missing.Open("no-such-seed", error) && !error.empty(), "a missing seed does not open");
  }

  std::filesystem::remove_all(testDir);
  if (port_test::Failures() == 0)
    std::printf("[solo-tests] OK\n");
  return PORT_TEST_RESULT();
}
