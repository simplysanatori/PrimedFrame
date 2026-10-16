// Where the built-in Remastered import lives and how a mod scan picks it up
// (port_remastered_storage.h): the app's own root on Android against the mods folder on desktop,
// the private import against a legacy one in the shared mods folder, and the name order a scan
// applies them in. Runs on the host: the header is policy only.

#include "port_remastered_storage.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int sFailures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

void MakeDir(const fs::path& path) {
  fs::create_directories(path);
  std::ofstream(path / "a.CMDL", std::ios::binary) << "model";
}

using PortRemasteredStorage::ModFolder;
using PortRemasteredStorage::Root;
using PortRemasteredStorage::RootChoice;

RootChoice Resolve(bool android, const std::string& internal, const std::string& mods) {
  return PortRemasteredStorage::ResolveRoot(android, internal, mods);
}

std::vector<std::string> Names(const std::vector<ModFolder>& mods) {
  std::vector<std::string> names;
  for (const ModFolder& mod : mods) {
    names.push_back(mod.name);
  }
  return names;
}

size_t Count(const std::vector<ModFolder>& mods, const std::string& name) {
  size_t count = 0;
  for (const ModFolder& mod : mods) {
    count += mod.name == name ? 1 : 0;
  }
  return count;
}

const ModFolder& Find(const std::vector<ModFolder>& mods, const std::string& name) {
  static const ModFolder sNone{};
  for (const ModFolder& mod : mods) {
    if (mod.name == name) {
      return mod;
    }
  }
  return sNone;
}

} // namespace

int main(int argc, char** argv) {
  const std::string mods = "/sdcard/MetroidPrime/mods";
  const std::string internal = "/data/user/0/org.metroidprime.port/files";
  const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::temp_directory_path() / "port-remastered-storage-test";

  // Desktop: the import is a mod folder, and MP_MODS/MP_USER_PATH move it with the rest.
  {
    const RootChoice desktop = Resolve(false, internal, mods);
    Check(desktop.kind == Root::kModsFolder, "desktop writes to the mods folder");
    Check(desktop.folder == mods + "/", "desktop root is the mods folder");
    Check(desktop.error.empty(), "desktop root has no error");
    Check(desktop.folder.find("remastered-imports") == std::string::npos, "desktop root has no private folder");
  }

  // Android: the app's own storage, outside the mods folder, whatever the mods folder is.
  {
    const RootChoice android = Resolve(true, internal, mods);
    Check(android.kind == Root::kPrivateStorage, "Android writes to the app's storage");
    Check(android.folder == internal + "/remastered-imports/", "Android root is under the internal storage");
    Check(android.folder.find("/sdcard") == std::string::npos, "the shared mods folder is never the root");
    Check(android.error.empty(), "an available root has no error");
    Check(PortRemasteredStorage::InstalledFolder(android.folder) == android.folder + "remastered-models",
          "the installed mod is the root's remastered-models");
    Check(PortRemasteredStorage::StagingFolder(android.folder) == android.folder + ".remastered-models.importing",
          "the staging folder is the root's hidden one");
    Check(PortRemasteredStorage::HeldFolder(android.folder) != PortRemasteredStorage::StagingFolder(android.folder) &&
              PortRemasteredStorage::BackupFolder(android.folder) != PortRemasteredStorage::InstalledFolder(
                                                                    android.folder),
          "the import's own folders do not collide");
    Check(!PortRemasteredStorage::IsModFolderName(PortRemasteredStorage::FileName(
              PortRemasteredStorage::StagingFolder(android.folder))),
          "the staging folder is not a mod");
  }

  // No internal storage: refused, and never worked around through the shared folder.
  {
    const RootChoice nowhere = Resolve(true, "", mods);
    Check(nowhere.kind == Root::kPrivateStorage, "Android still answers with the app's storage");
    Check(nowhere.folder.empty(), "no internal storage means no root");
    Check(!nowhere.error.empty(), "and it says why");
  }

  // The last part of a path, whatever the separators around it.
  Check(PortRemasteredStorage::FileName("/a/b/remastered-models") == "remastered-models", "file name of a path");
  Check(PortRemasteredStorage::FileName("C:\\mods\\remastered-models\\") == "remastered-models", "on Windows");
  Check(PortRemasteredStorage::FileName("remastered-models") == "remastered-models", "a bare name");

  // The shared mods folder alone: a legacy import keeps standing in, and nothing is dropped.
  {
    const std::vector<ModFolder> shared = {{"zzz", mods + "/zzz", false},
                                           {"remastered-models", mods + "/remastered-models", false},
                                           {"aaa", mods + "/aaa", false}};
    const std::vector<ModFolder> modsList = PortRemasteredStorage::OrderMods(shared, "");
    Check(Names(modsList) == std::vector<std::string>({"aaa", "remastered-models", "zzz"}),
          "a legacy shared import stands in, in name order");
    Check(Find(modsList, "remastered-models").path == mods + "/remastered-models", "the legacy one is the one listed");
  }

  // The private import wins the name, and keeps its alphabetical place among the others.
  {
    const std::string builtin = internal + "/remastered-imports/remastered-models";
    const std::vector<ModFolder> shared = {{"zzz", mods + "/zzz", false},
                                           {"remastered-models", mods + "/remastered-models", false},
                                           {"aaa", mods + "/aaa", false}};
    const std::vector<ModFolder> modsList = PortRemasteredStorage::OrderMods(shared, builtin);
    Check(Names(modsList) == std::vector<std::string>({"aaa", "remastered-models", "zzz"}),
          "the private import keeps name order");
    Check(Count(modsList, "remastered-models") == 1, "one folder per name");
    Check(Find(modsList, "remastered-models").path == builtin, "the private one shadows the shared one");
    Check(Find(modsList, "remastered-models").builtin, "and is marked as the built-in import");
  }

  // Only installed mods: a staging folder, a held one or a trash one is never listed.
  {
    const std::string root = internal + "/remastered-imports/";
    Check(PortRemasteredStorage::OrderMods({}, PortRemasteredStorage::StagingFolder(root)).empty(),
          "the staging folder is not a mod");
    Check(PortRemasteredStorage::OrderMods({}, PortRemasteredStorage::HeldFolder(root)).empty(),
          "a held import is not a mod");
    Check(PortRemasteredStorage::OrderMods({}, root + ".remastered-models.trash-12").empty(),
          "an import on its way out is not a mod");
    const std::vector<ModFolder> hidden = {{".remastered-models.importing", root + ".remastered-models.importing",
                                            false},
                                           {".hidden-mod", mods + "/.hidden-mod", false}};
    Check(PortRemasteredStorage::OrderMods(hidden, "").empty(), "a hidden folder in the mods folder is not a mod");
  }

  // The same policy over real folders: what a scan reads off the disk, on both sides.
  {
    fs::remove_all(root);
    const fs::path user = root / "mods";
    const fs::path privateRoot = root / "files" / "remastered-imports";
    MakeDir(user / "aaa");
    MakeDir(user / "remastered-models");      // the legacy import in the shared mods folder
    MakeDir(user / "zzz");
    MakeDir(user / ".remastered-models.trash-1");
    MakeDir(user / ".hidden-mod");
    MakeDir(privateRoot / "remastered-models");  // the app's own, installed
    MakeDir(privateRoot / ".remastered-models.importing");  // and one being made
    std::ofstream(privateRoot / "stray-file") << "not a folder";

    const std::string legacyPath = (user / "remastered-models").string();
    const std::string privatePath = (privateRoot / "remastered-models").string();
    const std::string stagingPath = (privateRoot / ".remastered-models.importing").string();

    // Nothing installed privately: the mods folder alone, hidden folders left out.
    const std::vector<ModFolder> legacyOnly = PortRemasteredStorage::ScanFolders(user.string(), "");
    Check(Names(legacyOnly) == std::vector<std::string>({"aaa", "remastered-models", "zzz"}),
          "the shared mods folder is read in name order");
    Check(Find(legacyOnly, "remastered-models").path == legacyPath, "the legacy import stands in");

    // The app's own import is merged in and wins the name.
    const std::vector<ModFolder> both = PortRemasteredStorage::ScanFolders(user.string(), privatePath);
    Check(Names(both) == std::vector<std::string>({"aaa", "remastered-models", "zzz"}),
          "the private import joins the same order");
    Check(Find(both, "remastered-models").path == privatePath, "the private import wins the name");

    // Its staging folder is not a mod, however it is handed in.
    const std::vector<ModFolder> staged = PortRemasteredStorage::ScanFolders(user.string(), stagingPath);
    Check(Names(staged) == std::vector<std::string>({"aaa", "remastered-models", "zzz"}),
          "a staging folder never joins the scan");
    Check(Find(staged, "remastered-models").path == legacyPath, "and the legacy import still stands in");

    // A mods folder that is not there: only the app's own import.
    Check(Names(PortRemasteredStorage::ScanFolders((root / "nowhere").string(), privatePath)) ==
              std::vector<std::string>({"remastered-models"}),
          "a missing mods folder still loads the private import");

    fs::remove_all(root);
  }

  // Which import a run reuses its unchanged stages from: the installed one, else the legacy one.
  {
    Check(PortRemasteredStorage::ReuseFolder("/private/remastered-models", true, "/shared/remastered-models", true) ==
              "/private/remastered-models",
          "the private import is reused first");
    Check(PortRemasteredStorage::ReuseFolder("/private/remastered-models", false, "/shared/remastered-models", true) ==
              "/shared/remastered-models",
          "a legacy import is reused when there is no private one");
    Check(PortRemasteredStorage::ReuseFolder("/private/remastered-models", false, "/shared/remastered-models", false)
              .empty(),
          "nothing to reuse converts everything");
  }

  // The movies-only import: the staged import is the newer one, and on Android it never writes into
  // a legacy import in the shared mods folder.
  {
    const std::string installed = internal + "/remastered-imports/remastered-models";
    const std::string staged = internal + "/remastered-imports/.remastered-models.importing";
    const std::string legacy = mods + "/remastered-models";
    const PortRemasteredStorage::MovieTarget newest =
        PortRemasteredStorage::ChooseMovieTarget(true, staged, true, installed, true, legacy, true);
    Check(newest.folder == staged && newest.error.empty(), "a staged import is written first");
    const PortRemasteredStorage::MovieTarget installedOnly =
        PortRemasteredStorage::ChooseMovieTarget(true, staged, false, installed, true, legacy, true);
    Check(installedOnly.folder == installed && installedOnly.error.empty(), "else the installed one");
    const PortRemasteredStorage::MovieTarget legacyOnly =
        PortRemasteredStorage::ChooseMovieTarget(true, staged, false, installed, false, legacy, true);
    Check(legacyOnly.folder.empty(), "Android refuses a legacy import");
    Check(legacyOnly.error.find("full import") != std::string::npos, "and asks for a full import");
    const PortRemasteredStorage::MovieTarget nothing =
        PortRemasteredStorage::ChooseMovieTarget(true, staged, false, installed, false, legacy, false);
    Check(nothing.folder.empty() && nothing.error.find("Import the models first") != std::string::npos,
          "with no import at all the models come first");
    const PortRemasteredStorage::MovieTarget desktop =
        PortRemasteredStorage::ChooseMovieTarget(false, staged, false, installed, false, legacy, false);
    Check(desktop.folder.empty() && desktop.error.find("Import the models first") != std::string::npos,
          "desktop keeps its message");
  }

  if (sFailures == 0) {
    std::printf("port_remastered_storage_tests: all passed\n");
  }
  return sFailures == 0 ? 0 : 1;
}