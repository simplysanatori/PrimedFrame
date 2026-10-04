// Remastered's LITS (LightBleedScale), the scale of a back-facing pixel's light, as the
// converter and the port's material record carry it. Checked as the record's reader alone
// (every older trailer keeps its meaning and neutral factors) and through real conversions:
// a two-sided synthetic model is run through PortRemastered::Converter and what it wrote is
// read back, the back copy's normals and the factors of its 'PBR6' record included.

#include "port_pbr_record.h"
#include "port_remastered_convert.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {

int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

constexpr uint32_t FourCC(char a, char b, char c, char d) {
  return (uint32_t(uint8_t(a)) << 24) | (uint32_t(uint8_t(b)) << 16) | (uint32_t(uint8_t(c)) << 8) | uint8_t(d);
}

void PutBe(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) {
    out.push_back(uint8_t(v >> s));
  }
}

void PutFloat(std::vector<uint8_t>& out, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  PutBe(out, bits);
}

// A material's tail: `floats` floats counting up from 1, the extra words, then the tag.
std::vector<uint8_t> Record(int floats, bool wrap, bool scale, const char* tag, float diffuse = 1.f,
                            float f0 = 1.f) {
  std::vector<uint8_t> r;
  for (int i = 0; i < floats; ++i) {
    PutFloat(r, float(i + 1));
  }
  if (wrap) {
    PutBe(r, 0x11223344);
  }
  if (scale) {
    PutFloat(r, diffuse);
    PutFloat(r, f0);
  }
  r.insert(r.end(), tag, tag + 4);
  return r;
}

void TestReader() {
  float v[19], s[2];
  uint32_t wrap = 0;
  {
    const std::vector<uint8_t> r = Record(19, true, true, "PBR6", 0.4f, 0.f);
    Check(r.size() == 92, "a PBR6 record is 92 bytes");
    Check(PortPbrRecord::Read(r.data() + r.size(), r.size(), v, &wrap, s) == 19, "PBR6 holds 19 floats");
    Check(v[0] == 1.f && v[18] == 19.f, "PBR6 floats are read before the wrap word");
    Check(wrap == 0x11223344, "PBR6 wrap word");
    Check(s[0] == 0.4f && s[1] == 0.f, "PBR6 diffuse and F0 factors");
  }
  struct Old {
    int floats;
    bool wrap;
    const char* tag;
  };
  const Old older[] = {{19, true, "PBR5"}, {19, false, "PBR4"}, {13, false, "PBR3"}, {8, false, "PBR2"}, {6, false, "PBRM"}};
  for (const Old& o : older) {
    const std::vector<uint8_t> r = Record(o.floats, o.wrap, false, o.tag);
    // A longer material around it, as in a CMDL: the reader looks at the end only.
    std::vector<uint8_t> m(40, 0xAA);
    m.insert(m.end(), r.begin(), r.end());
    s[0] = s[1] = 7.f;
    Check(PortPbrRecord::Read(m.data() + m.size(), m.size(), v, &wrap, s) == o.floats, o.tag);
    Check(v[o.floats - 1] == float(o.floats), "the last float of an older record is unmoved");
    Check(s[0] == 1.f && s[1] == 1.f, "an older record has neutral light factors");
    Check(wrap == (o.wrap ? 0x11223344u : 0x55555555u), "an older record's wrap is unchanged");
  }
  {
    const std::vector<uint8_t> none(40, 0x55);
    s[0] = s[1] = 7.f;
    Check(PortPbrRecord::Read(none.data() + none.size(), none.size(), v, &wrap, s) == 0, "no record");
    Check(s[0] == 1.f && s[1] == 1.f && v[0] == 1.f && v[3] == 0.f, "no record is neutral");
  }
  {  // A TEV material's wrap word alone.
    std::vector<uint8_t> m(40, 0xAA);
    const uint8_t word[] = {0x11, 0x22, 0x33, 0x44, 'W', 'R', 'A', 'P'};
    m.insert(m.end(), word, word + 8);
    s[0] = s[1] = 7.f;
    Check(PortPbrRecord::Read(m.data() + m.size(), m.size(), v, &wrap, s) == 0, "WRAP holds no floats");
    Check(wrap == 0x11223344u && v[0] == 1.f && s[0] == 1.f, "WRAP word, the rest neutral");
  }
  {  // A tag the material is too short to hold is not read.
    const std::vector<uint8_t> r = Record(0, false, false, "PBR6");
    s[0] = s[1] = 7.f;
    Check(PortPbrRecord::Read(r.data() + r.size(), r.size(), v, &wrap, s) == 0 && s[0] == 1.f,
          "a PBR6 tag with no room for the record is ignored");
  }
}

ModelMaterial Material(double lits, bool hasLits) {
  ModelMaterial material;
  material.name = "litstest";
  material.unk1 = 0;
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
  if (hasLits) {
    ModelMaterialData l;
    l.usage = FourCC('L', 'I', 'T', 'S');
    l.kind = ModelMaterialData::Kind::Scalar;
    l.scalar = float(lits);
    material.data.push_back(l);
  }
  return material;
}

Model BuildModel(const ModelMaterial& material, bool twoSided) {
  Model model;
  model.materials.push_back(material);
  ModelVertexBuffer vb;
  vb.vertexCount = 3;
  for (int v = 0; v < 3; ++v) {
    vb.positions.insert(vb.positions.end(), {float(v), float(v * v), 0.f});
    vb.normals.insert(vb.normals.end(), {0.f, 0.f, 1.f});
  }
  vb.uvs.resize(1);
  for (int v = 0; v < 3; ++v) {
    vb.uvs[0].insert(vb.uvs[0].end(), {float(v), 0.f});
  }
  model.vertexBuffers.push_back(vb);
  ModelMesh mesh;
  mesh.material = 0;
  mesh.vertexBuffer = 0;
  mesh.twoSided = twoSided;
  mesh.indices = {0, 1, 2};
  model.meshes.push_back(mesh);
  return model;
}

bool Convert(const Model& model, std::vector<uint8_t>& cmdl) {
  ConvertIO io;
  io.texture = [](const ModelUuid&, Image& out, std::string&) {
    out.width = out.height = 4;
    out.rgba.assign(64, 200);
    return true;
  };
  io.write = [&cmdl](const std::string& name, const std::vector<uint8_t>& data) {
    if (name.size() > 5 && name.compare(name.size() - 5, 5, ".CMDL") == 0) {
      cmdl = data;
    }
    return true;
  };
  ConvertOptions opt;
  opt.standalone = true;
  opt.retail = 0xABCD1234;
  opt.skip.clear();
  Converter converter(io);
  std::string error;
  if (!converter.Convert(model, opt, error)) {
    std::fprintf(stderr, "FAIL: the conversion failed: %s\n", error.c_str());
    ++sFailures;
    return false;
  }
  return true;
}

uint32_t Be32(const std::vector<uint8_t>& d, size_t o) {
  return (uint32_t(d[o]) << 24) | (uint32_t(d[o + 1]) << 16) | (uint32_t(d[o + 2]) << 8) | d[o + 3];
}

float BeFloat(const std::vector<uint8_t>& d, size_t o) {
  const uint32_t bits = Be32(d, o);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

struct Result {
  uint32_t materials = 0;
  int towardFront = 0;  // normals pointing +z, the mesh's own
  int turned = 0;       // normals pointing -z, the older back copy's
  std::vector<std::pair<float, float>> scales;  // diffuse, F0 of every 'PBR6' record
  bool ok = false;
};

// What a conversion wrote: the material count, the normals section's directions and the
// factors of each PBR6 record, found by its tag.
Result Read(const std::vector<uint8_t>& d) {
  Result r;
  if (d.size() < 48) {
    return r;
  }
  const uint32_t nsec = Be32(d, 36);
  if (nsec < 3 || 44 + 4 * size_t(nsec) > d.size()) {
    return r;
  }
  size_t base = (44 + 4 * size_t(nsec) + 31) & ~size_t(31);
  const uint32_t ntex = Be32(d, base);
  r.materials = Be32(d, base + 4 + 4 * size_t(ntex));
  const size_t normals = base + Be32(d, 44) + Be32(d, 48);
  const size_t normalBytes = Be32(d, 52);

  if (normals + normalBytes > d.size()) {
    return r;
  }
  for (size_t o = normals; o + 12 <= normals + normalBytes; o += 12) {
    const float z = BeFloat(d, o + 4);  // the converter writes Y up: the mesh's +z is +y
    r.towardFront += z > 0.5f;
    r.turned += z < -0.5f;
  }
  for (size_t o = 8; o + 4 <= d.size(); ++o) {
    if (std::memcmp(&d[o], "PBR6", 4) == 0) {
      r.scales.emplace_back(BeFloat(d, o - 8), BeFloat(d, o - 4));
    }
  }
  r.ok = true;
  return r;
}

Result Run(const ModelMaterial& material, bool twoSided) {
  std::vector<uint8_t> cmdl;
  if (!Convert(BuildModel(material, twoSided), cmdl)) {
    return {};
  }
  Result r = Read(cmdl);
  Check(r.ok, "the converted CMDL is readable");
  return r;
}

void TestConverter() {
  {  // No LITS: the older back copy, one material, no factors.
    const Result r = Run(Material(0, false), true);
    Check(r.materials == 1 && r.scales.empty(), "no LITS: one material, no PBR6");
    Check(r.turned > 0 && r.towardFront > 0, "no LITS: the back copy's normals are turned round");
  }
  {  // -1: the same as no LITS (the flipped normal is what the older copy has).
    const Result r = Run(Material(-1.0, true), true);
    Check(r.materials == 1 && r.scales.empty(), "LITS -1: one material, no PBR6");
    Check(r.turned > 0, "LITS -1: the older back copy's turned normals");
  }
  {  // 0 and a negative other than -1 are not implemented: the older copy, unchanged.
    for (const double lits : {0.0, -0.5}) {
      const Result r = Run(Material(lits, true), true);
      Check(r.materials == 1 && r.scales.empty() && r.turned > 0, "LITS 0 or another negative keeps the older copy");
    }
  }
  {  // A positive LITS: a back primitive and material of its own, diffuse |LITS| and F0 zero;
     // its vertices keep the older copy's turned normals (the shader turns them back).
    const Result r = Run(Material(0.4, true), true);
    Check(r.materials == 2, "LITS 0.4: the back copy is its own material");
    Check(r.turned > 0 && r.towardFront > 0, "LITS 0.4: the back copy has the turned normals, as legacy");
    Check(r.scales.size() == 1, "LITS 0.4: one PBR6 record, the back's");
    if (r.scales.size() == 1) {
      Check(std::fabs(r.scales[0].first - 0.4f) < 1e-6f && r.scales[0].second == 0.f,
            "LITS 0.4: diffuse 0.4, F0 factor 0");
    }
  }
  {  // 1 is a positive LITS too: diffuse 1, F0 0 on the back.
    const Result r = Run(Material(1.0, true), true);
    Check(r.materials == 2 && r.scales.size() == 1 && r.turned > 0, "LITS 1: a split with a PBR6 record");
    if (!r.scales.empty()) {
      Check(r.scales[0].first == 1.f && r.scales[0].second == 0.f, "LITS 1: factors 1 and 0");
    }
  }
  {  // One-sided meshes have no back copy, whatever LITS says.
    const Result r = Run(Material(0.4, true), false);
    Check(r.materials == 1 && r.scales.empty() && r.turned == 0, "a one-sided mesh ignores LITS");
  }
}

// The shader's cotangent frame (Schueler), as written in shader.cpp: T and B of a pixel from
// the screen derivatives of position and UV and the stored normal.
struct V3 {
  double x, y, z;
};
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

void Frame(V3 dp1, V3 dp2, double uv1[2], double uv2[2], V3 ngs, V3& t, V3& b) {
  const V3 dp2perp = Cross(dp2, ngs), dp1perp = Cross(ngs, dp1);
  t = dp2perp * uv1[0] + dp1perp * uv2[0];
  b = dp2perp * uv1[1] + dp1perp * uv2[1];
}

// A front pixel with the surface's normal, and the same surface point seen from behind: the
// screen's handedness flips (one derivative negated), and a back copy stores the turned
// normal. The back must get the front's T and B, for any UV orientation. Derived by hand,
// and shown here with numbers: the shader's `ngs` is the stored normal, `ng` the restored.
void TestFrame() {
  const V3 pu = {1.0, 0.2, 0.0}, pv = {-0.1, 0.9, 0.3};
  for (const double mirror : {1.0, -1.0}) {  // mirrored UVs: V grows against the surface's winding
    const V3 pvm = pv * mirror;
    const V3 n = Cross(pu, pv) * (1.0 / std::sqrt(Dot(Cross(pu, pv), Cross(pu, pv))));
    // Screen derivatives as (du, dv) per pixel step, front viewer.
    const double a1[2] = {0.7, 0.2}, a2[2] = {-0.3, 0.9};
    const V3 dp1 = pu * a1[0] + pvm * a1[1], dp2 = pu * a2[0] + pvm * a2[1];
    V3 tf, bf;
    Frame(dp1, dp2, const_cast<double*>(a1), const_cast<double*>(a2), n, tf, bf);
    // From behind: the same surface, the screen's second axis runs the other way round.
    const double c2[2] = {-a2[0], -a2[1]};
    const V3 dq2 = pu * c2[0] + pvm * c2[1];
    V3 tb, bb, tr, br;
    Frame(dp1, dq2, const_cast<double*>(a1), const_cast<double*>(c2), -n, tb, bb);  // stored: turned round
    Frame(dp1, dq2, const_cast<double*>(a1), const_cast<double*>(c2), n, tr, br);   // the restored normal
    auto same = [](V3 a, V3 c) { return std::fabs(a.x - c.x) + std::fabs(a.y - c.y) + std::fabs(a.z - c.z) < 1e-9; };
    Check(same(tb, tf) && same(bb, bf), "a back copy's frame on the stored normal is the front's T and B");
    Check(same(tr, -tf) && same(br, -bf), "the restored normal in the frame would mirror T and B (why it is not used)");
    Check(Dot(tf, n) < 1e-9 && Dot(tb, n) < 1e-9, "T is in the surface's plane");
  }
}

} // namespace

int main() {
  TestReader();
  TestConverter();
  TestFrame();
  if (sFailures == 0) {
    std::printf("port_remastered_lits_tests: ok\n");
  }
  return sFailures == 0 ? 0 : 1;
}
