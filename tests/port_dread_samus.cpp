// The dread-samus import (platform/port_dread_samus.cpp): the converter's explicit-weights path
// (ConvertOptions::boneWeights) against its usual nearest-retail-vertex votes, the synthetic Model
// a re-skinned body becomes, the retail skin reader and the mod order rule (ModPrecedence), all
// over synthetic fixtures.
//
// The whole import on real data is opt-in: PORT_DREAD_ROMFS names an extracted Metroid Dread RomFS
// (build/dread/romfs) and PORT_PRIME_SAMUS_DIR a directory of Prime resources dumped from the
// user's disc as <ID>.<TYPE> (CINF, CMDL, CSKR, TXTR of the bodies;
// build/s-dread6-20261024/dump_prime.py). It runs the import into a temporary folder and checks
// that every target's CMDL and CSKRs were written. Nothing of either game is committed.

#include "port_dread_samus.h"
#include "port_remastered_convert.h"
#include "port_remastered_storage.h"
#include "port_test.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(v >> shift));
  }
}

void Pad32(std::vector<uint8_t>& out) { out.resize((out.size() + 31) & ~size_t(31), 0); }

uint32_t FloatBits(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  return bits;
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

float BeF(const std::vector<uint8_t>& d, size_t o) {
  const uint32_t u = Be32(d, o);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

// A disc body (as port_remastered_blend_tests' disc model): one set of one untextured opaque
// material and one surface over three vertices at (-v, 0, v*v).
std::vector<uint8_t> RetailModel() {
  std::vector<uint8_t> mat;
  Put32(mat, 0);    // flags
  Put32(mat, 0);    // textures
  Put32(mat, 0x3);  // vertex attributes: a u16 position index
  Put32(mat, 0);    // group
  Put32(mat, 0);    // blend GX_BL_ZERO / GX_BL_ONE as (dst << 16) | src: one, zero
  mat.back() = 1;
  Put32(mat, 1);  // colour channels
  Put32(mat, 0);
  Put32(mat, 1);  // one TEV stage, no texture
  for (uint32_t v : {0x7A14Fu, 0x21CE7u, 0x100u, 0x100u, 0u}) {
    Put32(mat, v);
  }
  Put32(mat, 0x0000FFFF);
  Put32(mat, 0);  // texgens
  Put32(mat, 4);  // UV animations: none
  Put32(mat, 0);
  std::vector<std::vector<uint8_t>> secs(8);
  Put32(secs[0], 0);
  Put32(secs[0], 1);
  Put32(secs[0], uint32_t(mat.size()));
  secs[0].insert(secs[0].end(), mat.begin(), mat.end());
  for (int v = 0; v < 3; ++v) {
    for (float c : {-float(v), 0.f, float(v * v)}) {
      Put32(secs[1], FloatBits(c));
    }
  }
  std::vector<uint8_t>& surface = secs[7];
  surface.resize(12, 0);
  Put32(surface, 0);
  Put32(surface, 32);
  surface.resize(28, 0);
  Put32(surface, 0);
  surface.resize(64, 0);
  for (uint8_t b : {0x90, 0x00, 0x03, 0x00, 0x00, 0x00, 0x01, 0x00, 0x02}) {
    surface.push_back(b);
  }
  Put32(secs[6], 1);
  Put32(secs[6], 96);
  for (std::vector<uint8_t>& sec : secs) {
    Pad32(sec);
  }
  std::vector<uint8_t> out;
  Put32(out, 0xDEADBABE);
  Put32(out, 2);
  Put32(out, 0);
  out.resize(0x24, 0);
  Put32(out, uint32_t(secs.size()));
  Put32(out, 1);
  for (const std::vector<uint8_t>& sec : secs) {
    Put32(out, uint32_t(sec.size()));
  }
  Pad32(out);
  for (const std::vector<uint8_t>& sec : secs) {
    out.insert(out.end(), sec.begin(), sec.end());
  }
  return out;
}

// One group of three vertices, all on bone 5.
std::vector<uint8_t> RetailSkin() {
  std::vector<uint8_t> out;
  Put32(out, 1);
  Put32(out, 1);
  Put32(out, 5);
  Put32(out, FloatBits(1.f));
  Put32(out, 3);
  return out;
}

ModelMaterial Material() {
  ModelMaterial material;
  material.name = "dreadtest";
  material.types.push_back(FourCC('R', 'L', 'T', 'G'));
  ModelMaterialData d;
  d.usage = FourCC('D', 'I', 'F', 'T');
  d.kind = ModelMaterialData::Kind::Texture;
  d.texture.usage = d.usage;
  d.texture.hasUsage = true;
  d.texture.id[3] = 0x11;
  d.texture.wrapX = 1;
  d.texture.wrapY = 1;
  material.data.push_back(d);
  return material;
}

// The retail body's three vertices, already in the GameCube frame (orient identity), one mesh.
Model BodyModel() {
  Model model;
  model.form = FourCC('C', 'M', 'D', 'L');
  model.skinned = true;
  ModelVertexBuffer vb;
  vb.vertexCount = 3;
  for (int v = 0; v < 3; ++v) {
    vb.positions.insert(vb.positions.end(), {-float(v), 0.f, float(v * v)});
    vb.normals.insert(vb.normals.end(), {0.f, -1.f, 0.f});
  }
  vb.uvs.resize(1);
  for (int v = 0; v < 3; ++v) {
    vb.uvs[0].insert(vb.uvs[0].end(), {float(v), 0.f});
  }
  model.vertexBuffers.push_back(vb);
  model.materials.push_back(Material());
  ModelMesh mesh;
  mesh.indices = {0, 1, 2};
  mesh.indexCount = 3;
  model.meshes.push_back(mesh);
  return model;
}

constexpr uint32_t kBody = 0xABCD1234;
constexpr uint32_t kSkin = 0x11112222;

// Converts BodyModel over the disc body, with `weights` as ConvertOptions::boneWeights (none: the
// votes), and returns the CSKR written: per group, its (bone, weight) list and vertex count.
bool ConvertSkin(const std::vector<std::vector<std::vector<std::pair<uint32_t, float>>>>& weights,
                 std::vector<std::pair<std::vector<std::pair<uint32_t, float>>, uint32_t>>& groups) {
  const std::vector<uint8_t> cmdl = RetailModel(), cskr = RetailSkin();
  std::vector<uint8_t> written;
  ConvertIO io;
  io.texture = [](const ModelUuid&, Image& out, std::string&) {
    out.width = out.height = 4;
    out.rgba.assign(64, 200);
    return true;
  };
  io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    if (type == FourCC('C', 'M', 'D', 'L') && id == kBody) {
      out = cmdl;
      return true;
    }
    if (type == FourCC('C', 'S', 'K', 'R') && id == kSkin) {
      out = cskr;
      return true;
    }
    return false;
  };
  io.retailId = [](uint32_t) { return false; };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    if (name == "11112222.CSKR") {
      written = data;
    }
    return true;
  };
  ConvertOptions opt;
  opt.retail = kBody;
  opt.skins = {kSkin};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      opt.orient[r][c] = r == c ? 1.0 : 0.0;
    }
  }
  opt.skip.clear();
  opt.boneWeights = weights;
  Converter converter(io);
  std::string error;
  if (!CHECK_MSG(converter.Convert(BodyModel(), opt, error), "conversion: " + error) ||
      !CHECK_MSG(written.size() >= 4, "no CSKR written")) {
    return false;
  }
  groups.clear();
  size_t o = 4;
  for (uint32_t g = Be32(written, 0); g > 0; --g) {
    std::vector<std::pair<uint32_t, float>> ws;
    for (uint32_t n = Be32(written, o), i = (o += 4, 0); i < n; ++i, o += 8) {
      ws.emplace_back(Be32(written, o), BeF(written, o + 4));
    }
    groups.emplace_back(ws, Be32(written, o));
    o += 4;
  }
  return true;
}

void TestBoneWeights() {
  // Unset: the static model takes the nearest retail vertex's weights, bone 5.
  std::vector<std::pair<std::vector<std::pair<uint32_t, float>>, uint32_t>> groups;
  if (ConvertSkin({}, groups)) {
    std::set<uint32_t> bones;
    uint32_t verts = 0;
    for (const auto& [ws, count] : groups) {
      for (const auto& w : ws) {
        bones.insert(w.first);
      }
      verts += count;
    }
    CHECK_MSG(bones == std::set<uint32_t>{5}, "votes: every vertex on bone 5");
    CHECK(verts == 3);
  }
  // Set: written as given (quantised to 1/64), whatever the retail skin says.
  const std::vector<std::vector<std::vector<std::pair<uint32_t, float>>>> given = {
      {{{7, 0.75f}, {9, 0.25f}}, {{7, 1.f}}, {{9, 0.6f}, {7, 0.4f}}}};
  if (ConvertSkin(given, groups)) {
    std::map<std::vector<std::pair<uint32_t, float>>, uint32_t> seen;
    for (const auto& [ws, count] : groups) {
      seen[ws] += count;
    }
    CHECK_MSG(seen.size() == 3, "three distinct weight groups, got " + std::to_string(seen.size()));
    const std::vector<std::pair<uint32_t, float>> a = {{7, 0.75f}, {9, 0.25f}}, b = {{7, 1.f}};
    CHECK_MSG(seen.count(a) == 1 && seen[a] == 1, "vertex 0 keeps (7 .75, 9 .25)");
    CHECK_MSG(seen.count(b) == 1 && seen[b] == 1, "vertex 1 keeps (7 1)");
    bool quantised = false;
    for (const auto& [ws, count] : seen) {
      if (ws.size() == 2 && ws[0].first == 7 && ws[1].first == 9 && std::fabs(ws[0].second - 0.40625f) < 1e-6f &&
          std::fabs(ws[1].second - 0.59375f) < 1e-6f) {
        quantised = true;  // 0.4 and 0.6 to the nearest 1/64, sorted by bone
      }
    }
    CHECK_MSG(quantised, "vertex 2's (9 .6, 7 .4) quantised to 26/64 and 38/64");
    for (const auto& [ws, count] : groups) {
      for (const auto& w : ws) {
        CHECK_MSG(w.first != 5, "no vote weight leaks in");
      }
    }
  }
  // Too few lists for a buffer: refused, not read past.
  {
    ConvertIO io;
    io.retail = [](uint32_t type, uint32_t, std::vector<uint8_t>& out) {
      out = type == FourCC('C', 'M', 'D', 'L') ? RetailModel() : RetailSkin();
      return true;
    };
    io.retailId = [](uint32_t) { return false; };
    io.texture = [](const ModelUuid&, Image& out, std::string&) {
      out.width = out.height = 4;
      out.rgba.assign(64, 200);
      return true;
    };
    io.write = [](const std::string&, const std::vector<uint8_t>&) { return true; };
    ConvertOptions opt;
    opt.retail = kBody;
    opt.skins = {kSkin};
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) {
        opt.orient[r][c] = r == c ? 1.0 : 0.0;
      }
    }
    opt.boneWeights = {{{{7, 1.f}}}};
    Converter converter(io);
    std::string error;
    CHECK(!converter.Convert(BodyModel(), opt, error));
    CHECK(error.find("do not cover") != std::string::npos);
  }
}

void TestRetailSkin() {
  PortDread::RetailSkinRef ref;
  std::string error;
  if (CHECK_MSG(PortDreadSamus::RetailSkinFromCmdl(RetailModel(), RetailSkin(), ref, &error), error)) {
    CHECK(ref.weights.size() == 3 && ref.pos.size() == 9);
    CHECK(ref.pos[3] == -1.f && ref.pos[8] == 4.f);
    CHECK(ref.weights[2].size() == 1 && ref.weights[2][0].first == 5 && ref.weights[2][0].second == 1.f);
  }
  CHECK(!PortDreadSamus::RetailSkinFromCmdl({1, 2, 3}, RetailSkin(), ref, &error));
  std::vector<uint8_t> cut = RetailSkin();
  cut.resize(10);
  CHECK(!PortDreadSamus::RetailSkinFromCmdl(RetailModel(), cut, ref, &error));
}

void TestBuildModel() {
  PortDread::ReskinResult r;
  for (int m = 0; m < 3; ++m) {
    PortDread::ReskinMesh& mesh = r.meshes.emplace_back();
    mesh.name = "mesh" + std::to_string(m);
    mesh.vertex_count = 4;
    for (int v = 0; v < 4; ++v) {
      mesh.pos.insert(mesh.pos.end(), {float(v), float(m), float(v * m)});
      mesh.normal.insert(mesh.normal.end(), {0.f, 0.f, 1.f});
      mesh.weights.push_back({{uint32_t(3 + m), 1.f}});
      mesh.source.push_back(PortDread::WeightSource::Table);
    }
    mesh.uv[1].assign(8, 0.5f);  // a second set only
    mesh.submeshes.push_back({{0, 1, 2}});
    mesh.submeshes.push_back({{2, 3, 0, 1}});  // a stray index past the last triangle is dropped
  }
  PortDread::SuitMaterial a, b;
  a.material.name = "a";
  b.material.name = "b";
  PortRemastered::Model model;
  std::vector<std::vector<std::vector<std::pair<uint32_t, float>>>> weights;
  std::string error;
  if (!CHECK_MSG(PortDreadSamus::BuildModel(r, {&a, nullptr, &a}, model, weights, &error), error)) {
    return;
  }
  CHECK(model.skinned && model.form == FourCC('C', 'M', 'D', 'L'));
  CHECK_MSG(model.vertexBuffers.size() == 2, "the material-less mesh is left out");
  CHECK_MSG(model.materials.size() == 1 && model.materials[0].name == "a", "one material for the shared one");
  CHECK(model.meshes.size() == 4);
  CHECK(model.meshes[2].vertexBuffer == 1 && model.meshes[2].material == 0);
  CHECK(model.meshes[1].indices.size() == 3 && model.meshes[1].indexCount == 3);
  CHECK(weights.size() == 2 && weights[1].size() == 4 && weights[1][0][0].first == 5);
  CHECK_MSG(model.vertexBuffers[0].uvs.size() == 2 && model.vertexBuffers[0].uvs[0].size() == 8 &&
                model.vertexBuffers[0].uvs[1][0] == 0.5f,
            "a missing first uv set is zero-filled");
  CHECK(model.boundsMax[0] == 3.f && model.boundsMax[2] == 6.f && model.boundsMin[1] == 0.f);
  CHECK(!PortDreadSamus::BuildModel(r, {nullptr, nullptr, nullptr}, model, weights, &error));
  r.meshes[0].submeshes[0].indices[1] = 9;
  CHECK(!PortDreadSamus::BuildModel(r, {&b, nullptr, nullptr}, model, weights, &error));
}

void TestModOrder() {
  using PortRemasteredStorage::ModFolder;
  const std::vector<ModFolder> shared = {
      {"zz-last", "/m/zz-last", false}, {"dread-samus", "/m/dread-samus", false}, {"aa", "/m/aa", false}};
  const std::vector<ModFolder> order = PortRemasteredStorage::OrderMods(shared, "/data/remastered-models");
  std::vector<std::string> names;
  for (const ModFolder& m : order) {
    names.push_back(m.name);
  }
  CHECK_MSG((names == std::vector<std::string>{"aa", "remastered-models", "zz-last", "dread-samus"}),
            "dread-samus loads after every other mod");
  CHECK(PortRemasteredStorage::ModPrecedence(PortDreadSamus::kModName) == 1);
}

// ---- real data ----

void TestRealData() {
  const char* romfs = std::getenv("PORT_DREAD_ROMFS");
  const char* prime = std::getenv("PORT_PRIME_SAMUS_DIR");
  if (romfs == nullptr || *romfs == '\0' || prime == nullptr || *prime == '\0') {
    std::printf("SKIP: real-data Dread Samus import (PORT_DREAD_ROMFS / PORT_PRIME_SAMUS_DIR not set)\n");
    return;
  }
  namespace fs = std::filesystem;
  const fs::path out = fs::temp_directory_path() / ("port_dread_samus_" + std::to_string(std::rand()));
  fs::create_directories(out);
  auto readFile = [](const fs::path& path, std::vector<uint8_t>& data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      return false;
    }
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
  };
  PortDreadSamus::Io io;
  io.dread = [&](const std::string& path, std::vector<uint8_t>& data, std::string& why) {
    if (!readFile(fs::path(romfs) / path, data)) {
      why = "missing";
      return false;
    }
    return true;
  };
  io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& data) {
    char name[32];
    std::snprintf(name, sizeof(name), "%08X.%c%c%c%c", id, char(type >> 24), char(type >> 16), char(type >> 8),
                  char(type));
    return readFile(fs::path(prime) / name, data);
  };
  io.retailId = [&](uint32_t id) {
    char stem[16];
    std::snprintf(stem, sizeof(stem), "%08X.", id);
    for (const auto& e : fs::directory_iterator(prime)) {
      if (e.path().filename().string().rfind(stem, 0) == 0) {
        return true;
      }
    }
    return false;
  };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    std::ofstream f(out / name, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(f);
  };
  io.log = [](const std::string& line) { std::printf("  %s\n", line.c_str()); };
  io.detail = [](const std::string&) {};
  PortDreadSamus::Result result;
  std::string error;
  const bool ok = PortDreadSamus::Import(io, result, error);
  CHECK_MSG(ok, "import: " + error);
  CHECK_MSG(result.failed == 0, std::to_string(result.failed) + " targets failed");
  for (const PortDreadSamus::Target& t : PortDreadSamus::kTargets) {
    char name[32];
    std::snprintf(name, sizeof(name), "%08X.CMDL", t.cmdl);
    std::error_code ec;
    CHECK_MSG(fs::file_size(out / name, ec) > 1000 && !ec, std::string("missing ") + name);
    for (uint32_t s : t.cskr) {
      if (s != 0) {
        std::snprintf(name, sizeof(name), "%08X.CSKR", s);
        CHECK_MSG(fs::exists(out / name), std::string("missing ") + name);
      }
    }
  }
  size_t textures = 0;
  for (const std::string& f : result.files) {
    textures += f.size() > 5 && f.compare(f.size() - 5, 5, ".TXTR") == 0;
  }
  std::printf("real data: %zu targets, %zu files (%zu TXTR) in %s\n", result.converted, result.files.size(), textures,
              out.string().c_str());
  CHECK(textures > 0);
  CHECK(result.measured);
  if (std::getenv("PORT_DREAD_SAMUS_KEEP") == nullptr) {
    fs::remove_all(out);
  }
}

}  // namespace

int main() {
  TestBoneWeights();
  TestRetailSkin();
  TestBuildModel();
  TestModOrder();
  TestRealData();
  return PORT_TEST_RESULT();
}
