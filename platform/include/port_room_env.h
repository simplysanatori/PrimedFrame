#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// A room's lighting environment for PBR models: reflection probes, each a box of the world
// and a prefiltered HDR cube map of what surrounds it. A mod supplies one per area as
// <MREA id>.roomenv; the port reflects the cube of the probe a model stands in, in place
// of its own live probe. It can also hold the room's baked ambient light: a grid of
// points, each with the light arriving there by direction, which replaces the game's one
// ambient colour.
//
// The file is little endian:
//   'MPEV', u32 version (1 to 7), f32 tonemap[4], u32 probes, u32 cubes
//   probe: f32 worldToBox[12], f32 worldToCube[9], s32 layer, u32 cube, f32 scale, f32 blend
//   cube:  u32 size, u32 mips, u32 signed, u32 bytes, then BC6H blocks, every face of
//          mip 0, then of mip 1 and so on
// Version 2 goes on:
//   u32 grids
//   grid:  f32 worldToGrid[12], u32 size[3], then size[0] * size[1] * size[2] points of 24
//          bytes, x fastest and z slowest
//   point: half mean[3], half lobe[3], u8 sharpness[3], u8 direction[3][3] (of red, green
//          and blue, along the grid's axes; 0..255 is -1..1). A mean of zero is no point
//          (inside a wall).
// Version 3 goes on:
//   f32 exposure[2], the lowest and highest exposure value the room's auto exposure may
//          settle on (0, 0: the room has no auto exposure)
// Version 4 goes on:
//   f32 bias, what the room's auto exposure adds to the exposure value it measures
//   f32 contrast, of the tonemap (0 to 1)
// Version 5 goes on:
//   f32 bloom threshold, of the exposed luminance
//   u32 tints, then that many f32 RGBA: the bloom's colour per level (0 tints: no bloom)
// Version 6 goes on:
//   u32 grades
//   grade: s32 layer (-1: every layer), f32 fadeIn, f32 fadeOut (seconds), then a colour
//          grade LUT of 33^3 RGBA8, red fastest, blue slowest; the frame's tonemapped
//          colour looks itself up in it. Of the grades whose layer is active, the last
//          one is the room's.
// Version 7 goes on:
//   f32 sigma, of the Gaussian that eases the auto exposure towards what it measures, in
//          frames at 60 Hz
//   f32 static lerp, where in the hint's range the exposure value of emissive and unlit
//          surfaces sits (0 the lowest, 1 the highest)
// The tonemap is Remastered's: the exposure value without auto exposure, the radiance
// that comes out as middle grey once exposed, and how far the curve's toe and shoulder
// are pulled in.
// The cubes are stored normalised; a probe's scale times its cube is the radiance, in the
// same units as the grid's points.
namespace PortRoomEnv {

struct Probe {
  // Rows of world -> box; a point is inside when every coordinate is within -1..1.
  float worldToBox[12];
  // Rows of world direction -> cube lookup direction.
  float worldToCube[9];
  int32_t layer;
  uint32_t cube;
  float scale;
  float blend;
  // Worked out from worldToBox when the file is parsed (SetExtents): the box's half extent
  // along each row (0 for a row of length 0), and its volume, 8 times their product.
  float half[3];
  float volume;
};

struct Cube {
  uint32_t size = 0;
  uint32_t mipCount = 0;
  bool isSigned = false;
  size_t offset = 0; // of the blocks, in File::data
  size_t length = 0;
};

struct Grid {
  // Rows of world -> grid; point (i, j, k) is at grid coordinate (i, j, k).
  float worldToGrid[12];
  uint32_t size[3] = {};
  size_t offset = 0; // of the points, in File::data
  float average = 0.f; // geometric mean of the lit points' luminance
};

constexpr uint32_t kGradeLutSize = 33;
constexpr size_t kGradeLutBytes = size_t(kGradeLutSize) * kGradeLutSize * kGradeLutSize * 4;

struct Grade {
  int32_t layer = -1;
  float fadeIn = 0.f;
  float fadeOut = 0.f;
  size_t offset = 0; // of the LUT, in File::data
  uint32_t id = 0;   // a hash of the LUT, never 0; 0 here means the LUT is the identity
};

struct File {
  float tonemap[4] = {};
  float exposure[2] = {}; // EV range; both 0 when the room has no auto exposure
  float exposureBias = 0.f;
  float exposureSigma = 32.f; // Remastered's default
  float staticLerp = 0.5f;
  float contrast = 0.f;
  float bloomThreshold = 0.9f;
  std::vector<float> bloomTints; // RGBA; empty: the room has no bloom
  std::vector<Grade> grades;
  std::vector<Probe> probes;
  std::vector<Cube> cubes;
  std::vector<Grid> grids;
  std::vector<uint8_t> data;
};

// --- The file (port_room_env_file.cpp; no game or GX dependencies) -------------

// "1A2B3C4D.roomenv" (any case) -> 0x1A2B3C4D.
bool ParseFileName(const std::string& fileName, uint32_t& id);
bool Parse(std::vector<uint8_t>&& data, File& out, std::string& error);
// Bytes of BC6H a cube of this size has.
size_t CubeBytes(uint32_t size, uint32_t mipCount);

struct Pick {
  int probe = -1;
  bool inside = false;
  // Inside: the box's volume. Outside: the distance to the box. Smaller is better.
  float score = 0.f;
  bool Better(const Pick& other) const {
    return probe >= 0 && (other.probe < 0 || (inside != other.inside ? inside : score < other.score));
  }
};
// The probe for a point: the smallest box that holds it, else the nearest one. Reads the
// probes' half extents and volumes, which Parse fills in.
Pick PickProbe(const File& file, const float pos[3]);
// Fills in a probe's half extents and volume from its worldToBox.
void SetExtents(Probe& probe);

// The baked ambient at a point, as Remastered's shaders evaluate it: per colour channel c
// and for a surface normal n,
//   mean[c] - lobe[c] + 2 * lobe[c] * (1 + sharpness[c]) * q ^ (1 + 2 * sharpness[c]),
//   q = clamp(0.5 + 0.5 * dot(n, direction[c]), 0, 1)
// which averages to the mean over all n. The directions are in world space and not unit
// length.
struct Ambient {
  float mean[3];
  float lobe[3];
  float sharpness[3];
  float direction[3][3];
};
// Blends the points around `pos`, skipping the empty ones; false when there are none.
bool SampleGrid(const File& file, const Grid& grid, const float pos[3], Ambient& out);

// Remastered's CGaussianConvergence: a recursive Gaussian (Young and van Vliet) that a
// value follows its target through, one step a 60 Hz frame,
//   y = B x + b1 y1 + b2 y2 + b3 y3
// so a step in the target becomes an S curve about `sigma` frames long.
struct Convergence {
  float value = 0.f;
  // Doubles: with sigma 32, B is ~1e-4 against feedback terms near 3, so float rounding
  // leaves the value short of its target.
  double history[3] = {}; // the last three values, newest first
  double coeff[4] = {1.0, 0.0, 0.0, 0.0}; // B, b1, b2, b3
  void SetSigma(float sigma);
  // Jumps there, with no easing.
  void SetValue(float v);
  void Step(float target);
};

// --- The game side (port_room_env.cpp) -----------------------------------------

// The areas in memory now. Loads the files of new ones and frees those of areas that left.
void SetLoadedAreas(const uint32_t* mreas, size_t count);

struct Selection {
  uint32_t cube = 0;       // for GXSetPBRCube; 0 when the room has none
  float params[4] = {};    // for GXSetPBRCube
  float worldToCube[9] = {};
  bool hasAmbient = false;
  // For GXSetPBRAmbient: scaled so that the game's ambient level multiplies it, with the
  // directions still in world space.
  float ambient[6][3] = {};
  // The ambient is the light itself, at the room's exposure; the game's level stays out.
  bool ambientAbsolute = false;
  // For GXSetPBRVolume, when the model was announced with SetVolumeHint: the grid as
  // textures, which the shader reads per pixel in place of the one sample above.
  uint32_t volume = 0;
  float worldToVolume[12] = {}; // rows of world -> texture coordinates
  float worldToAxes[9] = {};    // rows of world direction -> the grid's axes
  float volumeLevel = 0.f;      // what the baked light is multiplied by
  float volumeBias = 0.f;       // metres off the surface the sample is taken
  float volumeDiagnostic = 0.f; // MP_ROOM_ENV_VOLUME_SHOW: 1 texture coordinates, 2 the light
};
// The room cube and baked ambient for a model at `pos`; false when no loaded area has
// either (or MP_ROOM_ENV=0).
bool Select(const float pos[3], Selection& out);
// Room geometry is lit by the baked ambient alone, per pixel. It announces the area and
// the middle of what it draws next, and the following Select answers with that area's
// grid as a volume (and picks the cube by that point, not the model's origin); Clear
// when it is done. MP_ROOM_ENV_VOLUME=0 turns volumes off.
void SetVolumeHint(uint32_t mrea, const float centre[3]);
void ClearVolumeHint();
// Whether a model announced for this area would get a volume.
bool HasVolume(uint32_t mrea);
// MP_ROOM_ENV_VOLUME, the console's `roomenv volume`.
void SetVolumesEnabled(bool on);
bool VolumesEnabled();
// What the baked ambient is multiplied by; 0 leaves the game's own ambient
// (MP_ROOM_ENV_AMBIENT, the console's `roomenv ambient`).
void SetAmbientScale(float scale);
float AmbientScale();
// What volume-lit surfaces show, for debugging: 0 the shaded surface, 1 the volume's
// texture coordinates, 2 the baked light alone (MP_ROOM_ENV_VOLUME_SHOW, the console's
// `roomenv show`).
void SetVolumeView(int view);
int VolumeView();
// Forgets everything (the mods folder changed).
void Reset();
// 0 off, 1 on; the console's `roomenv`.
void SetEnabled(bool enabled);
bool Enabled();
// Whether cubes and ambient are exposed the way Remastered exposes a frame, by the
// radiance of the room the camera is in and that room's exposure hint, and shaped by its
// tone curve (MP_ROOM_ENV_EXPOSURE=0 turns it off, as does the console's `roomenv
// exposure`). Otherwise each cube is exposed to middle grey and the ambient takes the
// game's level.
void SetRoomExposed(bool on);
bool RoomExposed();
// The frame's bloom (Remastered's CRenderPass_Bloom): the threshold of exposed luminance
// above which light blooms, and the five tints (rgb), the last for the bright pass and the
// others for the four levels it is spread over, coarsest first. False when the camera's
// room has none, the frame has no tone curve (see Tone), or MP_BLOOM=0.
bool Bloom(float& threshold, float tints[5][3]);
// MP_BLOOM, the console's `bloom`.
void SetBloomEnabled(bool on);
bool BloomEnabled();
// The frame's colour grade (Remastered's ColorGrade + ColorGradeHint): the LUTs to blend,
// for GXPortPostProcess (0: the identity) and how far towards `b` (0 to 1). `layerActive`
// says whether a layer of the camera's area is active. Moving between grades fades over
// the new one's fade-in. False when there is nothing to grade, or MP_COLOR_GRADE=0.
using LayerActive = bool (*)(int32_t layer, void* context);
bool ColorGrade(LayerActive layerActive, void* context, uint32_t& a, uint32_t& b, float& weight);
// MP_COLOR_GRADE, the console's `grade`.
void SetColorGradeEnabled(bool on);
bool ColorGradeEnabled();
// The area the camera is in: its exposure and tone curve are the frame's.
void SetViewArea(uint32_t mrea);
// Once a frame, after SetViewArea: moves the frame's exposure and tone curve on
// (CPostFXManager::UpdateTonemapping). The tonemap moves linearly to the camera room's
// over a second; with an auto exposure hint, the exposure value eases through a
// Convergence towards the one measured from the last frames' average radiance
// (GXPortFrameRadiance) when `roomGeoDrawing` (only then does the picture follow the
// exposure, so measuring it can settle), else towards the one of the room's probes. It
// jumps when the camera's previous room is gone (a world load or a teleport).
void UpdateFrame(bool roomGeoDrawing);
// The exposure to measure the frame at, for GXPortPostProcess; 0 when nothing would use
// the measurement.
float MeasureExposure();
// Whether the exposure follows the frame (MP_ROOM_ENV_AUTO_EXPOSURE, the console's
// `roomenv auto`); otherwise it follows the room's probes.
void SetAutoExposure(bool on);
bool AutoExposure();
// What emissive and unlit light in the opaque pass is multiplied by: Remastered draws it
// at the room's static exposure (its InverseTonemapExposure, CSceneTonemapParams' static
// value: the hint's range at the static lerp) while the frame is exposed at the moving
// one, so it is 2^(static EV - frame EV); the sorted pass uses the frame's, so 1 there.
// 1 when rooms are not exposed or MP_ROOM_ENV_STATIC_EXPOSURE=0 (the console's `roomenv
// static`).
float GlowScale();
void SetStaticExposure(bool on);
bool StaticExposure();
// Whether a model lit by the baked light (an absolute ambient or a volume) also takes the
// area's lights. Remastered's actors have none: the bake holds that light, and only runtime
// lights (beam, projectiles, the ball's glow) are added. Off unless MP_ROOM_ENV_AREA_LIGHTS=1
// (the console's `roomenv arealights`).
void SetAreaLights(bool on);
bool AreaLights();
// The frame's tone curve, for GXSetPBRTone; false when rooms are not exposed or the
// camera's room has no environment.
bool Tone(float rows[3][4]);
// The curve of Remastered's tonemap (NTonemap::build_tonemap_eval_params): `mid` is the
// exposed radiance that comes out at 0.25, with the slope `contrast` sets (0 to 1: between
// the lines from the origin through 0.25 and through 0.75 there), and `toe` and `shoulder`
// say how soon the curve leaves that line at either end.
void BuildTone(float mid, float contrast, float toe, float shoulder, float rows[3][4]);
// Areas with an environment, cubes on the GPU, ambient grids.
void Stats(int& areas, int& probes, int& cubes, int& grids);
// What every loaded area's environment gives a model at `pos` (the console's `roomenv
// info`): exposure and tone curve, the probe it would reflect, the baked ambient there.
std::string Info(const float pos[3]);

} // namespace PortRoomEnv
