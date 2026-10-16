#include "port_remastered_collision_material.h"
#include "port_remastered_ground_world.h"

// These checks must also run when the enclosing build defines NDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>

using namespace PortRemasteredCollisionMaterial;

namespace {
struct Expect { int bit; uint32_t filter, behavior; int orient, type; };
// Independent hand table of the 0xc59c20 stores (one list bit each).
constexpr Expect kExpect[] = {
    {19, 0x8000, 0, -1, -1},  {48, 0x2000, 0, -1, -1}, {50, 0x10000, 0, -1, -1},
    {32, 0x4, 0, -1, -1},     {33, 0x1, 0, -1, -1},    {35, 0x8, 0, -1, -1},
    {49, 0x2, 0, -1, -1},     {47, 0x40, 0, -1, -1},   {42, 0x80000, 0, -1, -1},
    {26, 0x1000, 0, -1, -1},  {44, 0x100000, 0, -1, -1}, {38, 0x410, 0, -1, -1},
    {41, 0x800, 0, -1, -1},   {37, 0x100, 0, -1, -1},  {13, 0, 0x4, -1, -1},
    {43, 0, 0x8, -1, -1},     {58, 0, 0x110, -1, -1},  {5, 0, 0x200, -1, -1},
    {29, 0, 0, 1, -1},        {30, 0, 0, 2, -1},       {31, 0, 0, 0, -1},
    {0, 0, 0, -1, 0},         {1, 0, 0, -1, 0x1b},     {2, 0, 0, -1, 0xc},
    {3, 0, 0, -1, 7},         {4, 0, 0, -1, 8},        {6, 0, 0, -1, 0xd},
    {7, 0, 0, -1, 0x11},      {8, 0, 0, -1, 3},        {9, 0, 0, -1, 9},
    {10, 0, 0, -1, 0xa},      {11, 0, 0, -1, 0x19},    {12, 0, 0, -1, 0xf},
    {14, 0, 0, -1, 0xb},      {15, 0, 0, -1, 0x12},    {16, 0, 0, -1, 0x18},
    {17, 0, 0, -1, 0x16},     {22, 0, 0, -1, 0x1c},    {23, 0, 0, -1, 0x10},
};
bool Mapped(int bit) {
  for (const Expect& e : kExpect) if (e.bit == bit) return true;
  return false;
}
bool Same(const CollisionMaterial& a, const CollisionMaterial& b) {
  return a.orientation == b.orientation && a.type == b.type && a.world == b.world &&
         a.behavior == b.behavior && a.filter == b.filter;
}
CollisionMaterial Mat(int32_t o, uint32_t b, uint32_t f, int32_t t = -1) {
  CollisionMaterial m;
  m.orientation = o; m.behavior = b; m.filter = f; m.type = t;
  return m;
}
}  // namespace

int main() {
  // Empty list.
  assert(Same(TranslatePositive(0), CollisionMaterial{}));
  assert(Same(TranslateNegative(0), CollisionMaterial{}));

  // All 64 singletons: positive translation and (non-solid) base translation.
  for (int bit = 0; bit < 64; ++bit) {
    const uint64_t l = Bit(bit);
    CollisionMaterial want;
    for (const Expect& e : kExpect) {
      if (e.bit != bit) continue;
      want.filter = e.filter; want.behavior = e.behavior;
      want.orientation = e.orient; want.type = e.type;
    }
    assert(Same(TranslatePositive(l), want));
    CollisionMaterial base = want;
    if (bit == 19) base.filter |= 0x36000;  // Solid synthesises all four channels
    assert(Same(TranslateMaterial(l), base));
    // Negative: only the four passthrough bits.
    CollisionMaterial neg;
    if (bit == 28) neg.filter = 0x2000;
    if (bit == 21) neg.filter = 0x4000;
    if (bit == 18) neg.filter = 0x10000;
    if (bit == 27) neg.filter = 0x20000;
    assert(Same(TranslateNegative(l), neg));
    // Bits with no mapping produce nothing at all (positive).
    if (!Mapped(bit)) assert(Same(TranslatePositive(l), CollisionMaterial{}));
    // Actor rule: Character alone adds 0x110; SolidCharacter alone nothing.
    CollisionMaterial act = base;
    if (bit == 33) act.behavior |= 0x110;
    assert(Same(TranslateActor(l), act));
  }

  // Solid + passthrough combinations (every subset of the four passthrough bits).
  for (int s = 0; s < 16; ++s) {
    uint64_t l = Bit(19);
    uint32_t pass = 0;
    if (s & 1) { l |= Bit(28); pass |= 0x2000; }
    if (s & 2) { l |= Bit(21); pass |= 0x4000; }
    if (s & 4) { l |= Bit(18); pass |= 0x10000; }
    if (s & 8) { l |= Bit(27); pass |= 0x20000; }
    assert(TranslateMaterial(l).filter == (0x8000u | (0x36000u & ~pass)));
    assert(TranslatePositive(l).filter == 0x8000u);  // synthesis is not positive
    // Non-solid with the same passthrough bits: nothing synthesised.
    assert(TranslateMaterial(l & ~Bit(19)).filter == 0);
    assert(TranslateNegative(l).filter == pass);
  }
  // AIBlock / NonSolidDamageable alias the same channels as solid blocking.
  assert(TranslateMaterial(Bit(48)).filter == 0x2000u);
  assert((TranslateMaterial(Bit(19)).filter & 0x2000u) == 0x2000u);
  assert(TranslateMaterial(Bit(50)).filter == 0x10000u);
  // Actor rule: Character && !SolidCharacter only; combination with SolidCharacter off.
  assert((TranslateActor(Bit(33) | Bit(55)).behavior) == 0);
  assert((TranslateActor(Bit(33)).behavior) == 0x110);
  assert((TranslateActor(Bit(33) | Bit(5)).behavior) == 0x310);
  assert(TranslateMaterial(Bit(33)).behavior == 0);  // base translation is unchanged

  // Orientation priority and type priority.
  assert(TranslatePositive(Bit(29) | Bit(30) | Bit(31)).orientation == 0);
  assert(TranslatePositive(Bit(29) | Bit(30)).orientation == 2);
  assert(TranslatePositive(Bit(29)).orientation == 1);
  assert(TranslatePositive(Bit(23) | Bit(22)).type == 0x1c);
  assert(TranslatePositive(Bit(23) | Bit(1) | Bit(8)).type == 0x1b);
  assert(TranslatePositive(Bit(23) | Bit(0) | Bit(1)).type == 0);
  assert(TranslatePositive(Bit(5)).type == -1);  // Pillar/HalfPipe are not types
  assert(TranslatePositive(~uint64_t{0}).world == -1);
  assert(OrientationFlags(Bit(31) | Bit(30) | Bit(29)) == 7);
  assert(OrientationFlags(Bit(29)) == 4 && OrientationFlags(Bit(30)) == 2 &&
         OrientationFlags(Bit(31)) == 1 && OrientationFlags(0) == 0);

  // Orientation filters: material orientation -1 / 3 never pass the MP1 wrapper.
  for (uint32_t fl = 0; fl < 8; ++fl) {
    assert(!OrientationPasses(fl, -1));
    assert(!OrientationPasses(fl, 3));
    assert(OrientationPasses(fl, 0) == ((fl & 1) != 0));
    assert(OrientationPasses(fl, 2) == ((fl & 2) != 0));
    assert(OrientationPasses(fl, 1) == ((fl & 4) != 0));
    assert(OrientationPassesBase(fl, -1));  // inner filter, before the MP1 guard
  }

  // Normal thresholds, exact and adjacent floats.
  const float t85 = 0.85f, t7 = 0.70710677f;
  const float up85 = std::nextafter(t85, 2.f), dn85 = std::nextafter(t85, -2.f);
  const float up7 = std::nextafter(t7, 2.f), dn7 = std::nextafter(t7, -2.f);
  const float nt7 = -t7, nup7 = std::nextafter(nt7, 2.f), ndn7 = std::nextafter(nt7, -2.f);
  for (int o : {-1, 3}) {
    assert(!IsFloor(o, t7) && IsFloor(o, up7) && !IsFloor(o, dn7));
    assert(IsFloor(o, up85) && IsFloor(o, t85));  // > 0.7071 already
    assert(IsWall(o, t7) && !IsWall(o, up7) && !IsWall(o, up85));
    assert(IsWall(o, nt7) && !IsWall(o, ndn7) && IsWall(o, nup7));
    assert(IsCeiling(o, ndn7) && !IsCeiling(o, nt7) && !IsCeiling(o, nup7));
  }
  assert(IsFloor(0, -1.f) && !IsWall(0, 0.f) && !IsCeiling(0, -1.f));
  assert(!IsFloor(1, t85) && IsFloor(1, up85) && IsFloor(1, 0.9f));
  assert(!IsFloor(2, t85) && IsFloor(2, up85));
  assert(IsCeiling(1, 1.f) && !IsWall(1, 0.f));
  assert(IsWall(2, -1.f) && IsWall(2, t85) && !IsWall(2, up85) && IsWall(2, dn85) &&
         !IsCeiling(2, -1.f));
  assert(!IsWall(1, 0.f) && !IsWall(0, 0.f));

  // Surface orientation: guard-free for -1 and 3.
  assert(OrientationSurfacePasses(2, -1, 0.f));
  assert(OrientationSurfacePasses(1, 3, 0.8f));
  assert(!OrientationSurfacePasses(1, -1, t7));
  assert(OrientationSurfacePasses(1, -1, up7));
  assert(OrientationSurfacePasses(4, -1, ndn7) && !OrientationSurfacePasses(4, -1, nt7));
  assert(!OrientationSurfacePasses(0, 0, 1.f));
  assert(!OrientationSurfacePasses(4, 0, -1.f));  // floor orientation is never ceiling

  // Base list filters, empty / singleton / aliased channels.
  const CollisionMaterial any = Mat(-1, 0x4, 0x2000);
  assert(!IncludePasses({}, any) && ExcludePasses({}, any));
  assert(IncludeAllPasses({}, any) && !ExcludeSomePasses({}, any));
  assert(!ExcludeSomePasses({}, CollisionMaterial{}) && IncludeAllPasses({}, CollisionMaterial{}));
  assert(IncludePasses({0x4, 0}, any) && IncludePasses({0, 0x2000}, any));
  assert(!IncludePasses({0x8, 0x1}, any));
  assert(!ExcludePasses({0x4, 0}, any) && !ExcludePasses({0, 0x2000}, any));
  assert(ExcludePasses({0x8, 0x1}, any));
  assert(IncludeAllPasses({0x4, 0x2000}, any) && !IncludeAllPasses({0x4, 0x6000}, any));
  assert(!IncludeAllPasses({0x5, 0x2000}, any));
  assert(ExcludeSomePasses({0x4, 0x6000}, any) && ExcludeSomePasses({0x5, 0}, any));
  assert(!ExcludeSomePasses({0x4, 0x2000}, any));

  // Include / Exclude MP1 on lists. Passthrough inversion: Include{AIPassthrough}
  // passes any material lacking the AI blocking channel, including every non-solid.
  const uint64_t aiPass = Bit(28);
  const CollisionMaterial solidBlocks = TranslateMaterial(Bit(19));
  const CollisionMaterial solidPassesAI = TranslateMaterial(Bit(19) | aiPass);
  const CollisionMaterial nonSolid = TranslateMaterial(0);
  assert(!IncludeSurfacePasses(aiPass, solidBlocks, 0.f));
  assert(IncludeSurfacePasses(aiPass, solidPassesAI, 0.f));
  assert(IncludeSurfacePasses(aiPass, nonSolid, 0.f));
  assert(IncludeMaterialPasses(aiPass, nonSolid) && !IncludeMaterialPasses(aiPass, solidBlocks));
  assert(ExcludeSurfacePasses(aiPass, solidBlocks, 0.f));
  assert(!ExcludeSurfacePasses(aiPass, solidPassesAI, 0.f));
  assert(!ExcludeSurfacePasses(aiPass, nonSolid, 0.f));
  assert(ExcludeMaterialPasses(aiPass, solidBlocks) && !ExcludeMaterialPasses(aiPass, nonSolid));
  // Empty list: Include never passes; Exclude passes (empty negative IncludeAll).
  assert(!IncludeSurfacePasses(0, solidBlocks, 0.f) && ExcludeSurfacePasses(0, solidBlocks, 0.f));
  assert(!IncludeMaterialPasses(0, any) && ExcludeMaterialPasses(0, any));
  // Unmapped-bit-only lists behave as empty (NoPlatformCollision 20, Trigger 34).
  for (int bit : {20, 24, 25, 34, 36, 39, 40, 45, 46, 51, 52, 53, 54, 55, 56, 57, 59, 63}) {
    assert(!IncludeSurfacePasses(Bit(bit), solidBlocks, 0.f));
    assert(ExcludeSurfacePasses(Bit(bit), solidBlocks, 0.f));
  }
  // Shared alias: AIBlock (48) and a Solid material block the same channel 0x2000.
  assert(!ExcludePasses(PositiveList(Bit(48)), solidBlocks));
  assert(!ExcludePasses(PositiveList(Bit(48)), TranslateMaterial(Bit(48))));
  assert(ExcludePasses(PositiveList(Bit(48)), solidPassesAI));
  // Orientation inside lists, surface form vs material form.
  const CollisionMaterial walled = TranslateMaterial(Bit(30));
  assert(IncludeSurfacePasses(Bit(30), walled, 0.f) && IncludeMaterialPasses(Bit(30), walled));
  assert(IncludeSurfacePasses(Bit(31), nonSolid, 0.9f));  // classifies by normal
  assert(!IncludeMaterialPasses(Bit(31), nonSolid));      // material orient -1
  assert(!ExcludeSurfacePasses(Bit(31), nonSolid, 0.9f));

  // MaterialFilter modes 0..3.
  const MaterialFilter f0{Bit(28), Bit(48), 0};
  const MaterialFilter f1{Bit(28), Bit(48), 1};
  const MaterialFilter f2{Bit(28), Bit(48), 2};
  const MaterialFilter f3{Bit(28), Bit(48), 3};
  const CollisionMaterial aiBlockMat = TranslateMaterial(Bit(48));
  const CollisionMaterial solidAI = TranslateMaterial(Bit(19) | Bit(28));
  for (const CollisionMaterial* m : {&solidBlocks, &solidPassesAI, &nonSolid, &aiBlockMat}) {
    assert(SurfacePasses(f0, *m, 0.f));
    assert(SurfacePasses(f1, *m, 0.f) == IncludeSurfacePasses(Bit(28), *m, 0.f));
    assert(SurfacePasses(f2, *m, 0.f) == ExcludeSurfacePasses(Bit(48), *m, 0.f));
    assert(SurfacePasses(f3, *m, 0.f) ==
           (IncludeSurfacePasses(Bit(28), *m, 0.f) && ExcludeSurfacePasses(Bit(48), *m, 0.f)));
  }
  // solidAI lacks the 0x2000 channel (AI passthrough): Include and Exclude both pass.
  assert(SurfacePasses(f3, solidAI, 0.f));
  assert(SurfacePasses(f3, solidPassesAI, 0.f));
  assert(!SurfacePasses(f3, aiBlockMat, 0.f));
  // Unused list is ignored in modes 1 and 2 even if it would reject.
  assert(SurfacePasses({0, Bit(48), 2}, solidBlocks, 0.f) == false);
  assert(SurfacePasses({Bit(31), Bit(48), 2}, nonSolid, 0.f));
  assert(!SurfacePasses({Bit(31), 0, 1}, nonSolid, 0.f));
  assert(SurfacePasses({Bit(31), 0, 1}, nonSolid, 0.9f));

  // Outer non-surface Passes, 0xcb7084: every mode equals the composition of the
  // guarded Include/Exclude forms; modes >= 4 pass everything.
  for (uint32_t mode : {0u, 1u, 2u, 3u}) {
    for (const CollisionMaterial* m : {&solidBlocks, &solidPassesAI, &nonSolid, &aiBlockMat, &walled}) {
      for (uint64_t inc : {uint64_t{0}, Bit(28), Bit(31), Bit(30), Bit(20)}) {
        for (uint64_t exc : {uint64_t{0}, Bit(48), Bit(31), Bit(30), Bit(20)}) {
          const bool want = ((mode | 2) != 3 || IncludeMaterialPasses(inc, *m)) &&
                            ((mode & ~1u) != 2 || ExcludeMaterialPasses(exc, *m));
          assert(Passes({inc, exc, mode}, *m) == want);
        }
      }
    }
  }
  for (uint32_t mode : {4u, 5u, 0xffffffffu, 0x80000000u}) {
    assert(Passes({Bit(31), Bit(31), mode}, nonSolid) && SurfacePasses({Bit(31), Bit(31), mode}, nonSolid, 0.f));
  }
  // Orientation -1/3 materials: Passes rejects through the guard, SurfacePasses
  // classifies by normal.
  assert(!Passes({Bit(31), 0, 1}, nonSolid) && SurfacePasses({Bit(31), 0, 1}, nonSolid, 0.9f));
  assert(!Passes({Bit(31) | Bit(30), 0, 1}, Mat(3, 0, 0)));
  assert(SurfacePasses({Bit(30), 0, 1}, Mat(3, 0, 0), 0.f));
  for (float up : {-1.f, 0.f, 0.9f}) assert(!Passes({Bit(31), 0, 1}, nonSolid));
  assert(Passes({Bit(31), 0, 1}, walled) == false && Passes({Bit(30), 0, 1}, walled));
  // High flag bits are inert; unknown orientations follow the unsigned compares.
  for (uint32_t high : {8u, 0x10u, 0x80000000u, 0xfffffff8u}) {
    for (uint32_t fl = 0; fl < 8; ++fl) {
      for (int32_t o : {-1, 0, 1, 2, 3, 4, -2, std::numeric_limits<int32_t>::min(),
                        std::numeric_limits<int32_t>::max()}) {
        assert(OrientationPasses(fl | high, o) == OrientationPasses(fl, o));
        assert(OrientationPassesBase(fl | high, o) == OrientationPassesBase(fl, o));
        assert(OrientationSurfacePasses(fl | high, o, 0.8f) == OrientationSurfacePasses(fl, o, 0.8f));
      }
    }
  }
  for (int32_t o : {4, -2, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()}) {
    assert(!OrientationPasses(7, o));        // neither 0/1/2 nor 3
    assert(!OrientationPassesBase(7, o));    // falls to o == 3
    assert(IsFloor(o, up7) && !IsFloor(o, t7) && !IsFloor(o, dn7));
    assert(IsWall(o, t7) && !IsWall(o, up7) && IsWall(o, nt7) && !IsWall(o, ndn7));
    assert(IsCeiling(o, ndn7) && !IsCeiling(o, nt7));
  }
  // NaN: floor iff o==0, wall iff o==2, ceiling iff o==1.
  const float nan = std::numeric_limits<float>::quiet_NaN();
  for (int32_t o : {-1, 0, 1, 2, 3, 4, -2, std::numeric_limits<int32_t>::min()}) {
    assert(IsFloor(o, nan) == (o == 0));
    assert(IsWall(o, nan) == (o == 2));
    assert(IsCeiling(o, nan) == (o == 1));
    assert(OrientationSurfacePasses(7, o, nan) == (o == 0 || o == 2 || o == 1));
  }

  // Production helpers now delegate here: compare against the former formulas
  // (encoded independently) for every singleton and deterministic random masks.
  auto oldOrientation = [](uint64_t b) {
    const bool floor = (b >> 31) & 1, wall = (b >> 30) & 1, ceiling = (b >> 29) & 1;
    return floor ? 0 : wall ? 2 : ceiling ? 1 : -1;
  };
  auto oldFlag100 = [](uint64_t b) {
    if ((b >> 58) & 1) return true;
    return ((b >> 33) & 1) && !((b >> 55) & 1);
  };
  auto check = [&](uint64_t b) {
    assert(PortRemasteredGroundWorld::Orientation(b) == oldOrientation(b));
    assert(PortRemasteredGroundWorld::Flag100(b) == oldFlag100(b));
    assert(TranslatePositive(b).orientation == oldOrientation(b));
  };
  check(0);
  check(~uint64_t{0});
  for (int bit = 0; bit < 64; ++bit) check(Bit(bit));
  uint64_t s = 0x9e3779b97f4a7c15ull;
  for (int i = 0; i < 2000000; ++i) {
    s ^= s << 13; s ^= s >> 7; s ^= s << 17;  // xorshift64
    check(s);
    check(s & (s >> 1));  // sparser masks
    check(s & (s >> 1) & (s >> 3));
  }
  return 0;
}
