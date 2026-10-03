// Room environments at run time: which areas have one, their cubes on the GPU, and the
// cube and ambient for a model. See port_room_env.h.
#include "port_room_env.h"

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_remastered_txtr.h"

#include <dolphin/gx/GXExtra.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace PortRoomEnv {
namespace {

struct GpuCube {
  uint32_t id = 0;      // 0: not made yet (the worker is decoding it), or it failed
  bool failed = false;
  float average = 0.f;  // luminance over every direction, as stored
  float peak = 0.f;     // the largest channel of the colour over every direction
  uint32_t mipCount = 0;
};

struct GpuVolume {
  uint32_t id = 0; // 0: not made yet (the worker is filling it in)
};

struct Area {
  File file;
  bool hasFile = false; // the file was read
  uint32_t serial = 0;  // of this load, which the worker's jobs for it carry
  std::vector<GpuCube> cubes;
  std::vector<GpuVolume> volumes; // one a grid
  float exposure = 0.f; // what takes the room's radiance to the display's range; 0: unknown
  float tone[3][4] = {}; // its tone curve
  bool hasGeo = false;   // the mod replaces its geometry
};

// The frame's exposure and tone curve, as CPostFXManager::UpdateTonemapping moves them.
struct FrameState {
  bool started = false;
  uint32_t area = 0; // the camera's room last frame
  std::vector<uint32_t> loaded; // the areas in memory last frame
  Convergence ev;
  float sigma = -1.f;
  float targetEv = 0.f;
  bool measuring = false; // this frame is measured (MeasureExposure)
  bool measured = false;  // targetEv is a measurement
  float measuredEv = 0.f;
  uint32_t serial = 0;      // of the last measurement seen
  uint32_t ignoreUntil = 0; // measurements up to here are of before a jump
  // The tonemap's EV, mid, contrast, toe and shoulder, and the static EV (see GlowScale),
  // moving linearly from `from` to `to`.
  float from[6] = {};
  float to[6] = {};
  float shown[6] = {};
  std::chrono::steady_clock::time_point start;
  std::chrono::steady_clock::time_point last;
  float carry = 0.f; // seconds not stepped yet
  float exposure = 0.f; // 2^(3 - EV); 0: none yet
  bool hasTone = false;
  float tone[3][4] = {};
};
FrameState sFrame;
std::vector<uint32_t> sLoadedSpare; // FrameState::loaded of the frame before last
int sAuto = -1;
int sStatic = -1;
int sAreaLights = -1;

// Areas in memory; one without a file has an empty File.
std::unordered_map<uint32_t, Area> sAreas;
uint32_t sNextSerial = 1;

// Work kept off the render thread: decoding a cube's BC6H into the RGBA16F that
// GXCreatePBRCube takes, and filling a grid's empty points in for GXCreatePBRVolume. Load
// queues every cube and volume of an area as it reads the file; one thread works through
// them in order, and UpdateFrame hands what is done to the GPU, one a frame. Last comes
// Keep: a copy of the parts of the file still read once the cubes are made, which takes
// the file's place, so the cubes' blocks (most of it) are freed. A job reads its area's
// File, which stays as it is until the job is done: Free cancels the area's jobs, and
// waits for the one running to stop, before the file goes, and Keep is the area's last.
// Defined after sAreas so that it is destroyed, and its thread joined, first.
enum class JobKind { Cube, Volume, Keep };

struct Job {
  JobKind kind = JobKind::Cube;
  uint32_t area = 0;
  uint32_t serial = 0; // Area::serial
  size_t index = 0;    // of the cube or grid in the file
  const File* file = nullptr;
};

struct Result {
  JobKind kind = JobKind::Cube;
  uint32_t area = 0;
  uint32_t serial = 0;
  size_t index = 0;
  std::vector<uint16_t> cube;  // RGBA16F, as GXCreatePBRCube takes it
  std::vector<uint8_t> volume; // as GXCreatePBRVolume takes it, or Keep's bytes
  std::vector<size_t> offsets; // Keep: where each grid's points, then each grade's LUT, are
};

struct Worker {
  // Decoded cubes waiting for the GPU; the thread waits while this many are.
  static constexpr size_t kMaxResults = 2;

  ~Worker();
  void Submit(const Job& job);
  // Drops the jobs and results of a load, and waits for its job that is running to stop.
  void Cancel(uint32_t serial);
  // The oldest result, if there is one; a cube or volume only when `gpu`.
  bool Take(Result& out, bool gpu);

  void Run();
  // False when cancelled.
  bool Do(const Job& job, Result& out);

  std::mutex mutex;
  std::condition_variable wake; // the thread: a job, room for its result, or stop
  std::condition_variable done; // Cancel: the running job ended
  std::deque<Job> jobs;
  std::deque<Result> results;
  bool running = false;
  uint32_t runningSerial = 0;
  std::atomic<bool> cancel{false};
  bool stop = false;
  bool noThread = false; // the thread could not start: jobs run in Submit
  std::thread thread;
};
Worker sWorker;

uint32_t sNextCube = 1;
uint32_t sNextVolume = 1;
int sVolumes = -1;
float sAmbientScale = -1.f;
float sVolumeView = -1.f;
bool sHint = false;
uint32_t sHintArea = 0;
float sHintCentre[3];
int sEnabled = -1;
int sExposure = -1;
int sBloom = -1;
uint32_t sViewArea = 0;
int sGrade = -1;
// The colour grade on screen: fading from one LUT to another (0 is the identity).
struct GradeFade {
  bool started = false;
  uint32_t from = 0;
  uint32_t to = 0;
  float seconds = 0.f; // how long the fade to `to` takes
  float fadeOut = 0.f; // of `to`, for when the next room has no grade
  std::chrono::steady_clock::time_point start;
};
GradeFade sGradeFade;
// LUTs handed to Aurora already; they are kept there for the run.
std::unordered_set<uint32_t> sGradeLuts;
// Model draws come in runs at one position; the last answer is kept until the frame or a
// setting moves on.
bool sLastValid = false;
bool sLastFound = false;
Selection sLast;
// Counts the changes to what Select finds for a point (see Invalidate).
uint32_t sEpoch = 1;

// The areas, their cubes or volumes on the GPU, or a setting that picks among them changed:
// every point Select has looked from is looked at again.
void Invalidate() {
  sLastValid = false;
  ++sEpoch;
}

// The reflection probes blended around the camera (UpdateBlend), and the cube Aurora draws
// them into. That cube is bound by its id, and a draw only binds a new one when the id
// changes, so a new blend goes into the other of two ids.
struct ProbeBlendState {
  int enabled = -1;
  bool hasView = false;
  float view[3] = {};
  Blend blend;
  uint32_t dst[2] = {};
  int shown = -1; // which dst holds `keys`/`weights`; -1: none
  uint64_t keys[kMaxBlend] = {};
  float weights[kMaxBlend] = {};
  size_t count = 0;
};
ProbeBlendState sBlend;

float HalfToFloat(uint16_t h) {
  const int exponent = (h >> 10) & 0x1F;
  const int mantissa = h & 0x3FF;
  float value;
  if (exponent == 0) {
    value = std::ldexp(float(mantissa), -24);
  } else if (exponent == 31) {
    value = mantissa == 0 ? 65504.f : 0.f; // infinity is clamped, a NaN is dropped
  } else {
    value = std::ldexp(float(mantissa | 0x400), exponent - 25);
  }
  return (h & 0x8000) != 0 ? -value : value;
}

float EnvFloat(const char* name, float fallback) {
  const char* const text = std::getenv(name);
  return text != nullptr && text[0] != '\0' ? float(std::atof(text)) : fallback;
}

void Free(Area& area) {
  // Before the file goes: the worker may be reading it.
  sWorker.Cancel(area.serial);
  for (GpuCube& cube : area.cubes) {
    if (cube.id != 0) {
      GXDestroyPBRCube(cube.id);
    }
  }
  area.cubes.clear();
  for (GpuVolume& volume : area.volumes) {
    if (volume.id != 0) {
      GXDestroyPBRVolume(volume.id);
    }
  }
  area.volumes.clear();
}

uint16_t FloatToHalf(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  const uint16_t sign = uint16_t((bits >> 16) & 0x8000);
  const int exponent = int((bits >> 23) & 0xFF) - 127 + 15;
  const uint32_t mantissa = bits & 0x7FFFFF;
  if (exponent <= 0) {
    return exponent < -10 ? sign : uint16_t(sign | ((mantissa | 0x800000) >> (14 - exponent)));
  }
  if (exponent >= 31) {
    return uint16_t(sign | 0x7BFF); // the largest half, for anything past it
  }
  return uint16_t(sign | (exponent << 10) | (mantissa >> 13));
}


// A grid as the textures of GXCreatePBRVolume. The points inside walls are empty, and a
// surface sits between those and the lit ones, so the texture filter would darken every
// wall; the empty points take the light of their lit neighbours first, layer by layer.
// On the worker; false when cancelled.
bool FillVolume(const File& file, const Grid& grid, std::vector<uint8_t>& texels, const std::atomic<bool>& cancel) {
  constexpr size_t kPoint = 24;
  constexpr int kLayers = 16;
  const size_t sx = grid.size[0], sy = grid.size[1], sz = grid.size[2];
  const size_t count = sx * sy * sz;
  std::vector<uint8_t> points(file.data.begin() + grid.offset, file.data.begin() + grid.offset + count * kPoint);
  const auto lit = [&points](size_t index) {
    const uint8_t* p = points.data() + index * kPoint;
    // Means are not negative, so any set bit but the sign is light.
    return ((p[0] | p[2] | p[4]) != 0) || (((p[1] | p[3] | p[5]) & 0x7F) != 0);
  };
  std::vector<uint8_t> state(count); // 1: lit, 2: filled in this layer
  for (size_t i = 0; i < count; ++i) {
    state[i] = lit(i) ? 1 : 0;
  }
  const size_t step[3] = {1, sx, sx * sy};
  const size_t size[3] = {sx, sy, sz};
  for (int layer = 0; layer < kLayers; ++layer) {
    size_t filled = 0;
    size_t index = 0;
    for (size_t z = 0; z < sz; ++z) {
      if (cancel.load(std::memory_order_relaxed)) {
        return false;
      }
      for (size_t y = 0; y < sy; ++y) {
        for (size_t x = 0; x < sx; ++x, ++index) {
          if (state[index] != 0) {
            continue;
          }
          const size_t at[3] = {x, y, z};
          float halves[6] = {};
          float bytes[12] = {};
          int total = 0;
          for (int axis = 0; axis < 3; ++axis) {
            for (int side = 0; side < 2; ++side) {
              if (side == 0 ? at[axis] == 0 : at[axis] + 1 == size[axis]) {
                continue;
              }
              const size_t other = side == 0 ? index - step[axis] : index + step[axis];
              if (state[other] != 1) {
                continue;
              }
              const uint8_t* p = points.data() + other * kPoint;
              for (int i = 0; i < 6; ++i) {
                halves[i] += HalfToFloat(uint16_t(p[i * 2] | (p[i * 2 + 1] << 8)));
              }
              for (int i = 0; i < 12; ++i) {
                bytes[i] += float(p[12 + i]);
              }
              ++total;
            }
          }
          if (total == 0) {
            continue;
          }
          uint8_t* p = points.data() + index * kPoint;
          for (int i = 0; i < 6; ++i) {
            const uint16_t half = FloatToHalf(halves[i] / float(total));
            p[i * 2] = uint8_t(half);
            p[i * 2 + 1] = uint8_t(half >> 8);
          }
          for (int i = 0; i < 12; ++i) {
            p[12 + i] = uint8_t(bytes[i] / float(total) + 0.5f);
          }
          state[index] = 2;
          ++filled;
        }
      }
    }
    if (filled == 0) {
      break;
    }
    for (uint8_t& value : state) {
      value = value != 0 ? 1 : 0;
    }
  }
  texels.assign(count * 28, 0);
  uint8_t* mean = texels.data();
  uint8_t* lobe = mean + count * 8;
  uint8_t* direction = lobe + count * 8;
  for (size_t i = 0; i < count; ++i) {
    const uint8_t* p = points.data() + i * kPoint;
    std::memcpy(mean + i * 8, p, 6);
    std::memcpy(lobe + i * 8, p + 6, 6);
    mean[i * 8 + 7] = lobe[i * 8 + 7] = 0x3C; // alpha 1.0
    for (int channel = 0; channel < 3; ++channel) {
      uint8_t* out = direction + (count * channel + i) * 4;
      std::memcpy(out, p + 15 + channel * 3, 3);
      out[3] = p[12 + channel];
    }
  }
  return true;
}

// How far outside a grid a point is, in metres; 0 inside.
float GridDistance(const Grid& grid, const float pos[3]) {
  const float* m = grid.worldToGrid;
  const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
  float sum = 0.f;
  for (int row = 0; row < 3; ++row) {
    const float* r = m + row * 4;
    const float at = r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3];
    const float out = std::max(std::max(-0.5f - at, at - (float(grid.size[row]) - 0.5f)), 0.f);
    sum += out * out;
  }
  return scale > 1e-12f ? std::sqrt(sum) / scale : 3.4e38f;
}

// A cube's average luminance, from its last mip (a texel or four a face), and the largest
// channel of its average colour.
float CubeAverage(const File& file, const Cube& cube, float& peak) {
  const uint32_t edge = std::max(cube.size >> (cube.mipCount - 1), 1u);
  const size_t blockBytes = size_t((edge + 3) / 4) * ((edge + 3) / 4) * 16;
  const uint8_t* blocks = file.data.data() + cube.offset + cube.length - blockBytes * 6;
  std::vector<uint16_t> texels(size_t(edge) * edge * 4);
  double sum[3] = {};
  for (uint32_t face = 0; face < 6; ++face) {
    PortRemastered::DecodeBc6hFace(blocks + blockBytes * face, edge, cube.isSigned, texels.data());
    for (size_t i = 0; i < size_t(edge) * edge; ++i) {
      for (int c = 0; c < 3; ++c) {
        sum[c] += HalfToFloat(texels[i * 4 + c]);
      }
    }
  }
  const double count = 6.0 * edge * edge;
  peak = float(std::max(std::max(sum[0], sum[1]), sum[2]) / count);
  return float((0.2126 * sum[0] + 0.7152 * sum[1] + 0.0722 * sum[2]) / count);
}

// What Remastered multiplies the room's radiance by before its tone curve: 2^(3 - EV).
// Without auto exposure EV is the Tonemap's own. With it (CPostFXManager::
// UpdateTonemapping), EV = log2(L / grey) + 3 + bias, held to the hint's range, where L is
// the largest channel of the last frame's average colour and grey is sRGB 128. The port
// has no HDR frame to average, so L is the middle one of the room's probes instead; over
// the 275 rooms that lands inside the hint's range for most.
float RoomExposure(const Area& area) {
  constexpr float kGrey = 0.2158605f; // sRGB 128, linear
  const File& file = area.file;
  float ev = file.tonemap[0];
  if (file.exposure[0] != 0.f || file.exposure[1] != 0.f) {
    std::vector<float> levels;
    for (const Probe& probe : file.probes) {
      const float level = area.cubes[probe.cube].peak * probe.scale;
      if (level > 0.f && std::isfinite(level)) {
        levels.push_back(level);
      }
    }
    float level = 0.f;
    if (!levels.empty()) {
      std::nth_element(levels.begin(), levels.begin() + levels.size() / 2, levels.end());
      level = levels[levels.size() / 2];
    } else {
      // The grid's points are the same radiance.
      for (const Grid& grid : file.grids) {
        level = std::max(level, grid.average);
      }
    }
    if (!(level > 0.f)) {
      return 0.f;
    }
    ev = std::log2(level / kGrey) + 3.f + file.exposureBias;
    ev = std::min(std::max(ev, file.exposure[0]), file.exposure[1]);
  }
  if (!std::isfinite(ev)) {
    return 0.f;
  }
  return std::exp2(3.f - ev);
}

// The whole file, in one read when its size is known. The stream is left as a read
// through istreambuf_iterator leaves it: failed only when the file did not open.
std::vector<uint8_t> ReadAll(std::ifstream& in) {
  std::vector<uint8_t> data;
  if (!in) {
    return data;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  in.seekg(0, std::ios::beg);
  if (in && size > 0) {
    data.resize(size_t(size));
    in.read(reinterpret_cast<char*>(data.data()), std::streamsize(size));
    data.resize(size_t(in.gcount()));
  } else {
    in.clear();
    data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  in.clear();
  return data;
}

void Load(uint32_t mrea, Area& area) {
  area.hasGeo = !PortMods::RoomGeoPath(mrea).empty();
  const std::string path = PortMods::RoomEnvPath(mrea);
  if (path.empty()) {
    return;
  }
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  std::vector<uint8_t> data = ReadAll(in);
  std::string error;
  if (!in || !Parse(std::move(data), area.file, error)) {
    PortLog::Write("room env: %s: %s\n", path.c_str(), error.empty() ? "cannot read" : error.c_str());
    area.file = {};
    return;
  }
  area.cubes.resize(area.file.cubes.size());
  for (size_t i = 0; i < area.cubes.size(); ++i) {
    area.cubes[i].average = CubeAverage(area.file, area.file.cubes[i], area.cubes[i].peak);
    // Black: nothing to reflect, and no exposure to set by it.
    area.cubes[i].failed = !(area.cubes[i].average > 1e-6f);
  }
  area.volumes.resize(area.file.grids.size());
  area.exposure = RoomExposure(area);
  const float* const t = area.file.tonemap;
  if (area.exposure > 0.f && t[1] > 0.f && t[1] < 1.f) {
    BuildTone(t[1], area.file.contrast, t[2], t[3], area.tone);
  }
  PortLog::Write("room env: %08X exposure %g (EV %g, hint %g..%g bias %g), tone mid %g contrast %g toe %g shoulder %g\n",
                 mrea, area.exposure, area.exposure > 0.f ? 3.f - std::log2(area.exposure) : 0.f,
                 area.file.exposure[0], area.file.exposure[1], area.file.exposureBias, t[1], area.file.contrast, t[2],
                 t[3]);
  area.hasFile = true;
  area.serial = sNextSerial++;
  if (sNextSerial == 0) {
    sNextSerial = 1;
  }
  // The worker makes them all now, so no draw waits for one. The volumes first: until its
  // area's are made, room geometry is lit as without volumes (see HasVolume).
  for (size_t i = 0; i < area.file.grids.size(); ++i) {
    if (area.file.grids[i].average > 0.f) {
      sWorker.Submit({JobKind::Volume, mrea, area.serial, i, &area.file});
    }
  }
  for (size_t i = 0; i < area.cubes.size(); ++i) {
    if (!area.cubes[i].failed) {
      sWorker.Submit({JobKind::Cube, mrea, area.serial, i, &area.file});
    }
  }
  if (!area.file.cubes.empty()) {
    sWorker.Submit({JobKind::Keep, mrea, area.serial, 0, &area.file});
  }
}

// The frame's exposure, for radiance from `area`: one exposure covers the whole picture
// (see UpdateFrame), or the area's own before the frame has one.
float FrameExposure(const Area& area) {
  return sFrame.exposure > 0.f ? sFrame.exposure : area.exposure;
}

// What an area keeps of its file once its cubes are made: the grids' points, which
// SampleGrid reads for models every frame, and the grades' LUTs, handed to Aurora when one
// is first shown; with where each starts in the copy. The cubes' blocks are left behind.
void KeepData(const File& file, std::vector<uint8_t>& kept, std::vector<size_t>& offsets) {
  constexpr size_t kPoint = 24;
  size_t bytes = 0;
  for (const Grid& grid : file.grids) {
    bytes += size_t(grid.size[0]) * grid.size[1] * grid.size[2] * kPoint;
  }
  bytes += file.grades.size() * kGradeLutBytes;
  kept.clear();
  kept.reserve(bytes);
  offsets.clear();
  const auto keep = [&](size_t offset, size_t length) {
    offsets.push_back(kept.size());
    kept.insert(kept.end(), file.data.begin() + offset, file.data.begin() + offset + length);
  };
  for (const Grid& grid : file.grids) {
    keep(grid.offset, size_t(grid.size[0]) * grid.size[1] * grid.size[2] * kPoint);
  }
  for (const Grade& grade : file.grades) {
    keep(grade.offset, kGradeLutBytes);
  }
}

// Decodes a cube into RGBA16Float, every mip of face 0, then face 1 (GXCreatePBRCube); the
// file has every face of mip 0, then mip 1. On the worker; false when cancelled.
bool DecodeCube(const File& file, const Cube& cube, std::vector<uint16_t>& texels, const std::atomic<bool>& cancel) {
  std::vector<size_t> mipOffset(cube.mipCount);
  size_t perFace = 0;
  for (uint32_t mip = 0; mip < cube.mipCount; ++mip) {
    const size_t edge = std::max(cube.size >> mip, 1u);
    mipOffset[mip] = perFace;
    perFace += edge * edge * 8;
  }
  texels.assign(perFace * 6 / 2, 0);
  const uint8_t* blocks = file.data.data() + cube.offset;
  for (uint32_t mip = 0; mip < cube.mipCount; ++mip) {
    const uint32_t edge = std::max(cube.size >> mip, 1u);
    const size_t blockBytes = size_t((edge + 3) / 4) * ((edge + 3) / 4) * 16;
    for (uint32_t face = 0; face < 6; ++face) {
      if (cancel.load(std::memory_order_relaxed)) {
        return false;
      }
      uint16_t* out = texels.data() + (perFace * face + mipOffset[mip]) / 2;
      PortRemastered::DecodeBc6hFace(blocks, edge, cube.isSigned, out);
      blocks += blockBytes;
    }
  }
  return true;
}

Worker::~Worker() {
  {
    std::lock_guard<std::mutex> lock(mutex);
    stop = true;
    cancel = true;
  }
  wake.notify_all();
  if (thread.joinable()) {
    thread.join();
  }
}

void Worker::Submit(const Job& job) {
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (!thread.joinable() && !noThread) {
      try {
        thread = std::thread(&Worker::Run, this);
      } catch (...) {
        noThread = true;
        PortLog::Write("room env: no worker thread; cubes and volumes are made as areas load\n");
      }
    }
    if (!noThread) {
      jobs.push_back(job);
      wake.notify_one();
      return;
    }
  }
  Result result;
  if (Do(job, result)) {
    std::lock_guard<std::mutex> lock(mutex);
    results.push_back(std::move(result));
  }
}

void Worker::Cancel(uint32_t serial) {
  std::unique_lock<std::mutex> lock(mutex);
  jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [serial](const Job& job) { return job.serial == serial; }),
             jobs.end());
  results.erase(std::remove_if(results.begin(), results.end(),
                               [serial](const Result& result) { return result.serial == serial; }),
                results.end());
  if (running && runningSerial == serial) {
    cancel = true;
    done.wait(lock, [this, serial] { return !running || runningSerial != serial; });
  }
  wake.notify_all();
}

bool Worker::Take(Result& out, bool gpu) {
  std::lock_guard<std::mutex> lock(mutex);
  if (results.empty() || (!gpu && results.front().kind != JobKind::Keep)) {
    return false;
  }
  out = std::move(results.front());
  results.pop_front();
  wake.notify_all();
  return true;
}

void Worker::Run() {
  for (;;) {
    Job job;
    {
      std::unique_lock<std::mutex> lock(mutex);
      wake.wait(lock, [this] { return stop || (!jobs.empty() && results.size() < kMaxResults); });
      if (stop) {
        return;
      }
      job = jobs.front();
      jobs.pop_front();
      running = true;
      runningSerial = job.serial;
      cancel = false;
    }
    Result result;
    const bool finished = Do(job, result);
    {
      std::lock_guard<std::mutex> lock(mutex);
      running = false;
      if (finished && !cancel) {
        results.push_back(std::move(result));
      }
    }
    done.notify_all();
  }
}

bool Worker::Do(const Job& job, Result& out) {
  out.kind = job.kind;
  out.area = job.area;
  out.serial = job.serial;
  out.index = job.index;
  switch (job.kind) {
  case JobKind::Cube:
    return DecodeCube(*job.file, job.file->cubes[job.index], out.cube, cancel);
  case JobKind::Volume:
    return FillVolume(*job.file, job.file->grids[job.index], out.volume, cancel);
  case JobKind::Keep:
    KeepData(*job.file, out.volume, out.offsets);
    return true;
  }
  return false;
}

// Hands the worker's decoded cubes and filled volumes to the GPU, one a frame: each is
// megabytes for Aurora to copy and upload, and an area brings several at once. While the
// room environment is off they wait (nothing would use them), and so does the worker.
void TakeResults() {
  bool gpu = Enabled();
  Result result;
  while (sWorker.Take(result, gpu)) {
    const auto found = sAreas.find(result.area);
    if (found == sAreas.end() || found->second.serial != result.serial) {
      continue; // Free drops an area's results, so this does not happen
    }
    Area& area = found->second;
    File& file = area.file;
    if (result.kind == JobKind::Keep) {
      // The worker has finished with the old bytes: Keep is the area's last job.
      file.data.swap(result.volume);
      for (size_t i = 0; i < file.grids.size(); ++i) {
        file.grids[i].offset = result.offsets[i];
      }
      for (size_t i = 0; i < file.grades.size(); ++i) {
        file.grades[i].offset = result.offsets[file.grids.size() + i];
      }
      for (Cube& cube : file.cubes) {
        cube.offset = cube.length = 0; // its blocks are gone
      }
      continue;
    }
    if (result.kind == JobKind::Cube) {
      const Cube& cube = file.cubes[result.index];
      GpuCube& gpuCube = area.cubes[result.index];
      gpuCube.id = sNextCube++;
      if (sNextCube == 0) {
        sNextCube = 1;
      }
      gpuCube.mipCount = cube.mipCount;
      GXCreatePBRCube(gpuCube.id, cube.size, cube.mipCount, result.cube.data(), uint32_t(result.cube.size() * 2));
    } else {
      const Grid& grid = file.grids[result.index];
      GpuVolume& gpuVolume = area.volumes[result.index];
      gpuVolume.id = sNextVolume++;
      if (sNextVolume == 0) {
        sNextVolume = 1;
      }
      GXCreatePBRVolume(gpuVolume.id, grid.size[0], grid.size[1], grid.size[2], result.volume.data(),
                        uint32_t(result.volume.size()));
      PortLog::Write("room env: %08X volume %u %ux%ux%u average %g\n", result.area, gpuVolume.id, grid.size[0],
                     grid.size[1], grid.size[2], grid.average);
    }
    // What Select finds changes with what is on the GPU.
    Invalidate();
    gpu = false;
  }
}

// A probe's GPU cube; null when it is black or the worker has not made it yet.
GpuCube* ProbeCube(Area& area, const Probe& probe) {
  GpuCube& gpu = area.cubes[probe.cube];
  return gpu.id != 0 ? &gpu : nullptr;
}

// The blend's keys are an area's MREA (high half) and the probe's index in its file.
uint64_t ProbeKey(uint32_t mrea, size_t index) { return (uint64_t(mrea) << 32) | uint64_t(index); }

bool FindProbe(uint64_t key, Area*& area, const Probe*& probe) {
  const auto found = sAreas.find(uint32_t(key >> 32));
  const size_t index = size_t(key & 0xFFFFFFFFu);
  if (found == sAreas.end() || index >= found->second.file.probes.size()) {
    return false;
  }
  area = &found->second;
  probe = &found->second.file.probes[index];
  return true;
}

// The blend's probes that have a cube: a black one adds nothing to the blended cube.
struct BlendSource {
  uint64_t key;
  Area* area;
  const Probe* probe;
  GpuCube* gpu;
  float weight;
};
size_t BlendSources(BlendSource out[kMaxBlend]) {
  size_t count = 0;
  for (const BlendEntry& entry : sBlend.blend.entries) {
    Area* area = nullptr;
    const Probe* probe = nullptr;
    if (count == kMaxBlend || !FindProbe(entry.key, area, probe)) {
      continue;
    }
    GpuCube* const gpu = ProbeCube(*area, *probe);
    if (gpu != nullptr) {
      out[count++] = {entry.key, area, probe, gpu, entry.weight};
    }
  }
  return count;
}

void FreeBlend() {
  for (uint32_t& id : sBlend.dst) {
    if (id != 0) {
      GXDestroyPBRCube(id);
      id = 0;
    }
  }
  sBlend.shown = -1;
  sBlend.count = 0;
  sBlend.blend = {};
}

// Moves the blend to the camera, and has Aurora draw a new blended cube when its probes or
// their weights changed.
void UpdateProbeBlend() {
  ProbeBlendState& s = sBlend;
  if (!Enabled() || !ProbeBlend() || !s.hasView) {
    s.blend = {};
    return;
  }
  std::vector<BlendCandidate> candidates;
  for (auto& [mrea, area] : sAreas) {
    for (size_t i = 0; i < area.file.probes.size(); ++i) {
      candidates.push_back({ProbeKey(mrea, i), &area.file.probes[i]});
    }
  }
  UpdateBlend(candidates.data(), candidates.size(), s.view, s.blend);
  sLastValid = false;
  BlendSource sources[kMaxBlend];
  const size_t count = BlendSources(sources);
  if (count < 2) {
    return; // a lone cube is bound as it is
  }
  bool same = s.shown >= 0 && s.count == count;
  for (size_t i = 0; same && i < count; ++i) {
    same = s.keys[i] == sources[i].key && std::fabs(s.weights[i] - sources[i].weight) < 1e-3f;
  }
  if (same) {
    return;
  }
  const int next = s.shown == 0 ? 1 : 0;
  if (s.dst[next] == 0) {
    s.dst[next] = sNextCube++;
    if (sNextCube == 0) {
      sNextCube = 1;
    }
  }
  uint32_t ids[kMaxBlend];
  float weights[kMaxBlend];
  for (size_t i = 0; i < count; ++i) {
    ids[i] = sources[i].gpu->id;
    weights[i] = sources[i].weight;
  }
  // It fails outside a frame's drawing; the next frame tries again.
  if (GXPortBlendPBRCube(s.dst[next], ids, weights, uint32_t(count))) {
    s.shown = next;
    s.count = count;
    for (size_t i = 0; i < count; ++i) {
      s.keys[i] = sources[i].key;
      s.weights[i] = sources[i].weight;
    }
  }
}

} // namespace

void BuildTone(float mid, float contrast, float toe, float shoulder, float rows[3][4]) {
  const float a25 = std::atan2(0.25f, mid);
  const float a75 = std::atan2(0.75f, mid);
  const float slope = std::tan(a25 + contrast * (a75 - a25));
  const float lineEnd = mid + 0.75f * shoulder / slope;
  // The toe: a cubic through the origin that meets the line at `mid`, in value and slope.
  const float half = 0.5f * (0.75f / mid - slope);
  const float c = (half - slope >= 0.f ? slope : half) * (1.f - toe);
  rows[0][0] = (slope + c) / (mid * mid) - 0.5f / (mid * mid * mid);
  rows[0][1] = 0.75f / (mid * mid) - (slope + 2.f * c) / mid;
  rows[0][2] = c;
  rows[0][3] = 0.f;
  rows[1][0] = slope;
  rows[1][1] = 0.25f - slope * mid;
  rows[1][2] = mid;
  rows[1][3] = lineEnd;
  // The shoulder: from the line's end towards 1.
  const float top = 0.25f + 0.75f * shoulder;
  const float k = 1.f - top > 0.f ? slope / (1.f - top) : 0.f;
  rows[2][0] = 1.f - top;
  rows[2][1] = k;
  rows[2][2] = -lineEnd * k;
  rows[2][3] = top;
}

bool Tone(float rows[3][4]) {
  if (!Enabled() || !RoomExposed()) {
    return false;
  }
  if (!sFrame.hasTone) {
    return false;
  }
  std::memcpy(rows, sFrame.tone, sizeof(sFrame.tone));
  return true;
}

void UpdateFrame(bool roomGeoDrawing) {
  constexpr float kGrey = 0.2158605f; // sRGB 128, linear
  constexpr uint32_t kInFlight = 3;   // readbacks Aurora may have queued
  using Clock = std::chrono::steady_clock;
  TakeResults();
  // The exposure moves every frame, so a model's last answer is stale.
  sLastValid = false;
  FrameState& f = sFrame;
  const auto now = Clock::now();
  // Built in last frame's spare list and swapped in, so no frame allocates.
  std::vector<uint32_t>& loaded = sLoadedSpare;
  loaded.clear();
  for (const auto& entry : sAreas) {
    loaded.push_back(entry.first);
  }
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || !view->second.hasFile) {
    // A room without an environment keeps the frame as it was, unless the one it came from
    // is gone too.
    if (f.started && f.area != sViewArea && sAreas.find(f.area) == sAreas.end()) {
      f = {};
    }
    f.last = now;
    f.loaded.swap(loaded);
    return;
  }
  const Area& area = view->second;
  const File& file = area.file;
  const float* const t = file.tonemap;
  const bool hint = file.exposure[0] != 0.f || file.exposure[1] != 0.f;
  // CSceneTonemapParams' static exposure: a fixed point in the hint's range, or the
  // tonemap's own EV without one.
  const float staticEv = hint ? file.exposure[0] + (file.exposure[1] - file.exposure[0]) * file.staticLerp : t[0];
  const float target[6] = {t[0], t[1], file.contrast, t[2], t[3], staticEv};
  float radiance[3] = {};
  uint32_t serial = 0;
  const bool hasRadiance = GXPortFrameRadiance(radiance, &serial);
  // Walking into a room that was already loaded eases, even when the room left is gone at
  // once; arriving in one that wasn't (a world load, a warp) starts there.
  const bool jump = !f.started || (f.area != sViewArea &&
                                   std::find(f.loaded.begin(), f.loaded.end(), sViewArea) == f.loaded.end());
  f.loaded.swap(loaded);
  if (jump) {
    f.started = true;
    std::memcpy(f.from, target, sizeof(target));
    std::memcpy(f.to, target, sizeof(target));
    f.start = f.last = now;
    f.carry = 0.f;
    f.measured = false;
    f.ignoreUntil = serial + kInFlight;
  } else if (std::memcmp(target, f.to, sizeof(target)) != 0) {
    std::memcpy(f.from, f.shown, sizeof(f.shown));
    std::memcpy(f.to, target, sizeof(target));
    f.start = now;
  }
  f.area = sViewArea;
  const float moved = std::min(std::chrono::duration<float>(now - f.start).count(), 1.f);
  for (int i = 0; i < 6; ++i) {
    f.shown[i] = f.from[i] + (f.to[i] - f.from[i]) * moved;
  }
  if (f.sigma != file.exposureSigma) {
    f.sigma = file.exposureSigma;
    f.ev.SetSigma(f.sigma);
  }
  // Measuring settles only where the picture follows the exposure: room geometry, lit by
  // the room's own light. The retail world keeps its level whatever the exposure, and
  // measuring it would push the exposure to an end of the hint's range.
  f.measuring = hint && roomGeoDrawing && area.hasGeo && AutoExposure() && Enabled() && RoomExposed();
  if (hasRadiance && serial != f.serial) {
    f.serial = serial;
    const float peak = std::max(std::max(radiance[0], radiance[1]), radiance[2]);
    if (f.measuring && int32_t(serial - f.ignoreUntil) > 0 && peak > 0.f && std::isfinite(peak)) {
      const float ev = std::log2(peak / kGrey) + 3.f + file.exposureBias;
      f.measuredEv = std::min(std::max(ev, file.exposure[0]), file.exposure[1]);
      f.measured = true;
    }
  }
  if (!f.measuring) {
    f.measured = false;
  }
  if (!hint) {
    f.targetEv = f.shown[0];
  } else if (f.measured) {
    f.targetEv = f.measuredEv;
  } else {
    f.targetEv = area.exposure > 0.f ? 3.f - std::log2(area.exposure) : f.shown[0];
  }
  // Without a hint the exposure is the tonemap's, moving linearly with it; with one it eases
  // through the Gaussian, a step every 60th of a second.
  f.carry += std::chrono::duration<float>(now - f.last).count();
  f.last = now;
  if (jump || !hint) {
    f.ev.SetValue(f.targetEv);
    f.carry = 0.f;
  } else {
    const int steps = int(f.carry * 60.f);
    f.carry -= float(steps) / 60.f;
    for (int i = std::min(steps, 30); i > 0; --i) {
      f.ev.Step(f.targetEv);
    }
  }
  const float exposure = std::exp2(3.f - f.ev.value);
  f.exposure = std::isfinite(exposure) && exposure > 0.f ? exposure : 0.f;
  f.hasTone = f.exposure > 0.f && f.shown[1] > 0.f && f.shown[1] < 1.f;
  if (f.hasTone) {
    BuildTone(f.shown[1], f.shown[2], f.shown[3], f.shown[4], f.tone);
  }
}

float MeasureExposure() { return sFrame.measuring ? sFrame.exposure : 0.f; }

float GlowScale() {
  if (sStatic < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV_STATIC_EXPOSURE");
    sStatic = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  if (sStatic == 0 || !sFrame.hasTone || !Enabled() || !RoomExposed()) {
    return 1.f;
  }
  // The light is divided by the static exposure and the frame then multiplied by its own:
  // 2^(3 - EV) / 2^(3 - static EV).
  const float scale = std::exp2(sFrame.shown[5] - sFrame.ev.value);
  return std::isfinite(scale) && scale > 0.f ? scale : 1.f;
}

void SetStaticExposure(bool on) { sStatic = on ? 1 : 0; }

bool StaticExposure() {
  GlowScale();
  return sStatic != 0;
}

bool AreaLights() {
  if (sAreaLights < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV_AREA_LIGHTS");
    sAreaLights = env != nullptr && env[0] == '1' ? 1 : 0;
  }
  return sAreaLights != 0;
}

void SetAreaLights(bool on) { sAreaLights = on ? 1 : 0; }

bool AutoExposure() {
  if (sAuto < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV_AUTO_EXPOSURE");
    sAuto = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sAuto != 0;
}

void SetAutoExposure(bool on) { sAuto = on ? 1 : 0; }

bool BloomEnabled() {
  if (sBloom < 0) {
    const char* const env = std::getenv("MP_BLOOM");
    sBloom = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sBloom != 0;
}

void SetBloomEnabled(bool on) { sBloom = on ? 1 : 0; }

bool Bloom(float& threshold, float tints[5][3]) {
  if (!Enabled() || !RoomExposed() || !BloomEnabled()) {
    return false;
  }
  const auto view = sAreas.find(sViewArea);
  if (view == sAreas.end() || !(view->second.tone[1][0] > 0.f) || view->second.file.bloomTints.size() < 20) {
    return false;
  }
  const std::vector<float>& rgba = view->second.file.bloomTints;
  for (int i = 0; i < 5; ++i) {
    for (int c = 0; c < 3; ++c) {
      tints[i][c] = rgba[i * 4 + c];
    }
  }
  threshold = view->second.file.bloomThreshold;
  return true;
}

bool ColorGradeEnabled() {
  if (sGrade < 0) {
    const char* const env = std::getenv("MP_COLOR_GRADE");
    sGrade = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sGrade != 0;
}

void SetColorGradeEnabled(bool on) { sGrade = on ? 1 : 0; }

bool ColorGrade(LayerActive layerActive, void* context, uint32_t& a, uint32_t& b, float& weight) {
  a = b = 0;
  weight = 0.f;
  if (!Enabled() || !ColorGradeEnabled()) {
    sGradeFade.started = false;
    return false;
  }
  // The room's grade: the last one whose layer is active. A room without a file keeps
  // whatever the frame had, as the doors between two rooms do.
  const auto view = sAreas.find(sViewArea);
  if (view != sAreas.end() && view->second.hasFile) {
    const File& file = view->second.file;
    const Grade* pick = nullptr;
    for (const Grade& grade : file.grades) {
      if (grade.layer < 0 || layerActive == nullptr || layerActive(grade.layer, context)) {
        pick = &grade;
      }
    }
    const uint32_t target = pick != nullptr ? pick->id : 0;
    if (target != 0 && sGradeLuts.insert(target).second) {
      GXPortColorGradeLut(target, file.data.data() + pick->offset);
    }
    GradeFade& f = sGradeFade;
    const auto now = std::chrono::steady_clock::now();
    if (!f.started) {
      f = {true, target, target, 0.f, 0.f, now};
    } else if (target != f.to) {
      // Mid-fade, the new fade starts from whichever grade showed more.
      const float shown = f.seconds > 0.f
          ? std::chrono::duration<float>(now - f.start).count() / f.seconds : 1.f;
      f.from = shown >= 0.5f ? f.to : f.from;
      f.to = target;
      f.seconds = pick != nullptr ? pick->fadeIn : f.fadeOut;
      f.start = now;
    }
    f.fadeOut = pick != nullptr ? pick->fadeOut : 0.f;
  }
  GradeFade& f = sGradeFade;
  if (!f.started) {
    return false;
  }
  float t = 1.f;
  if (f.seconds > 0.f) {
    t = std::chrono::duration<float>(std::chrono::steady_clock::now() - f.start).count() / f.seconds;
  }
  if (t >= 1.f) {
    f.from = f.to;
    t = 1.f;
  }
  a = f.from;
  b = f.to;
  weight = t;
  return a != 0 || b != 0;
}

void SetViewArea(uint32_t mrea) {
  if (sViewArea != mrea) {
    sViewArea = mrea;
    sLastValid = false;
  }
}

bool ProbeBlend() {
  if (sBlend.enabled < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV_BLEND");
    sBlend.enabled = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sBlend.enabled != 0;
}

void SetProbeBlend(bool on) {
  sBlend.enabled = on ? 1 : 0;
  sBlend.blend = {};
  sLastValid = false;
}

void SetViewPoint(const float pos[3]) {
  std::memcpy(sBlend.view, pos, sizeof(sBlend.view));
  sBlend.hasView = true;
  UpdateProbeBlend();
}

bool VolumesEnabled() {
  if (sVolumes < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV_VOLUME");
    sVolumes = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sVolumes != 0;
}

void SetVolumesEnabled(bool on) {
  sVolumes = on ? 1 : 0;
  Invalidate();
}

float AmbientScale() {
  if (sAmbientScale < 0.f) {
    sAmbientScale = std::max(EnvFloat("MP_ROOM_ENV_AMBIENT", 1.f), 0.f);
  }
  return sAmbientScale;
}

void SetAmbientScale(float scale) {
  sAmbientScale = std::max(scale, 0.f);
  Invalidate();
}

int VolumeView() {
  if (sVolumeView < 0.f) {
    sVolumeView = std::max(EnvFloat("MP_ROOM_ENV_VOLUME_SHOW", 0.f), 0.f);
  }
  return static_cast<int>(sVolumeView);
}

void SetVolumeView(int view) {
  sVolumeView = static_cast<float>(std::max(view, 0));
  sLastValid = false;
}

bool Enabled() {
  if (sEnabled < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV");
    sEnabled = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sEnabled != 0;
}

bool RoomExposed() {
  if (sExposure < 0) {
    const char* const env = std::getenv("MP_ROOM_ENV_EXPOSURE");
    sExposure = env != nullptr && env[0] == '0' ? 0 : 1;
  }
  return sExposure != 0;
}

void SetRoomExposed(bool on) {
  sExposure = on ? 1 : 0;
  sLastValid = false;
}

void SetEnabled(bool enabled) {
  sEnabled = enabled ? 1 : 0;
  Invalidate();
}

void SetVolumeHint(uint32_t mrea, const float centre[3]) {
  sHint = true;
  sHintArea = mrea;
  std::memcpy(sHintCentre, centre, sizeof(sHintCentre));
}

void ClearVolumeHint() { sHint = false; }

bool HasVolume(uint32_t mrea) {
  if (!Enabled() || !VolumesEnabled()) {
    return false;
  }
  const auto found = sAreas.find(mrea);
  if (found == sAreas.end()) {
    return false;
  }
  // Only once the worker has made every volume the area has: until then its models are lit
  // as they are without volumes.
  const Area& area = found->second;
  bool lit = false;
  for (size_t i = 0; i < area.file.grids.size(); ++i) {
    if (area.file.grids[i].average > 0.f) {
      if (area.volumes[i].id == 0) {
        return false;
      }
      lit = true;
    }
  }
  return lit;
}

void Reset() {
  for (auto& [mrea, area] : sAreas) {
    Free(area);
  }
  sAreas.clear();
  FreeBlend();
  sFrame = {};
  Invalidate();
}

void SetLoadedAreas(const uint32_t* mreas, size_t count) {
  bool changed = false;
  for (auto it = sAreas.begin(); it != sAreas.end();) {
    if (std::find(mreas, mreas + count, it->first) == mreas + count) {
      Free(it->second);
      it = sAreas.erase(it);
      changed = true;
    } else {
      ++it;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (sAreas.find(mreas[i]) == sAreas.end()) {
      Load(mreas[i], sAreas[mreas[i]]);
      changed = true;
    }
  }
  if (changed) {
    Invalidate();
  }
}

namespace {

// Where a model's cube, volume and ambient come from, before any exposure: what the point
// and the hint pick among the loaded areas. It stays good while the areas, their cubes and
// volumes on the GPU and the settings that pick (see Invalidate) stay as they are.
struct Located {
  const Area* cubeArea = nullptr; // the probe's area, when its cube is on the GPU
  const Probe* probe = nullptr;
  const GpuCube* cube = nullptr;
  const Area* volumeArea = nullptr; // the hint's area, when its nearest grid has a volume
  const Grid* grid = nullptr;
  uint32_t volume = 0;
  const Area* ambientArea = nullptr; // whose grid gave `sample`, when one did
  float average = 0.f;               // that grid's
  Ambient sample;
};

// What else a Located depends on: the point Select looks from, and the volume hint.
struct SelectKey {
  uint32_t pos[3] = {}; // the point's bits
  uint32_t hintArea = 0; // 0 without a hint
  bool hint = false;
  bool operator==(const SelectKey& other) const {
    return std::memcmp(pos, other.pos, sizeof(pos)) == 0 && hintArea == other.hintArea && hint == other.hint;
  }
};

struct SelectKeyHash {
  size_t operator()(const SelectKey& key) const {
    uint64_t hash = 1469598103934665603ull;
    for (const uint32_t value : {key.pos[0], key.pos[1], key.pos[2], key.hintArea, uint32_t(key.hint)}) {
      hash = (hash ^ value) * 1099511628211ull;
      hash ^= hash >> 29;
    }
    return size_t(hash);
  }
};

// Every point Select has looked from since the last Invalidate, so a model that stays put
// (room geometry, most of a room's actors) is found again without a search. When `sLocated`
// fills it becomes `sLocatedOld`, whose entries move back as they are asked for again.
constexpr size_t kMaxLocated = 4096;
std::unordered_map<SelectKey, Located, SelectKeyHash> sLocated;
std::unordered_map<SelectKey, Located, SelectKeyHash> sLocatedOld;
uint32_t sLocatedEpoch = 0;
SelectKey sLastKey;

// The probe, volume and ambient for a point. A cube or volume the worker has not made yet
// is left out, as a black cube is.
void Locate(const float pos[3], Located& out) {
  out = {};
  Area* bestArea = nullptr;
  Pick best;
  for (auto& [mrea, area] : sAreas) {
    const Pick pick = PickProbe(area.file, pos);
    if (pick.Better(best)) {
      best = pick;
      bestArea = &area;
    }
  }
  if (bestArea != nullptr) {
    const Probe& probe = bestArea->file.probes[best.probe];
    const GpuCube& gpu = bestArea->cubes[probe.cube];
    if (gpu.id != 0) {
      out.cubeArea = bestArea;
      out.probe = &probe;
      out.cube = &gpu;
    }
  }
  if (sHint && VolumesEnabled()) {
    const auto found = sAreas.find(sHintArea);
    if (found != sAreas.end()) {
      Area& area = found->second;
      int pick = -1;
      float nearest = 0.f;
      for (size_t i = 0; i < area.file.grids.size(); ++i) {
        const float distance = GridDistance(area.file.grids[i], pos);
        if (area.file.grids[i].average > 0.f && (pick < 0 || distance < nearest)) {
          pick = int(i);
          nearest = distance;
        }
      }
      if (pick >= 0) {
        const GpuVolume& gpu = area.volumes[pick];
        if (gpu.id != 0) {
          out.volumeArea = &area;
          out.grid = &area.file.grids[pick];
          out.volume = gpu.id;
        }
      }
    }
  }
  if (AmbientScale() > 0.f) {
    // A model's origin is often on the floor, where the grid has no point for it, so the
    // spot a metre up counts too. The first grid with light at the first spot is the one.
    const float above[3] = {pos[0], pos[1], pos[2] + 1.f};
    for (const float* spot : {pos, above}) {
      for (auto& [mrea, area] : sAreas) {
        for (const Grid& grid : area.file.grids) {
          if (grid.average > 0.f && SampleGrid(area.file, grid, spot, out.sample)) {
            out.ambientArea = &area;
            out.average = grid.average;
            return;
          }
        }
      }
    }
  }
}

// The Selection for what Locate found, at the frame's exposure and the settings now.
bool Compose(const Located& located, Selection& out) {
  static const float gain = EnvFloat("MP_ROOM_ENV_GAIN", 1.f);
  static const float lod = EnvFloat("MP_ROOM_ENV_LOD", 5.f);
  static const float volumeBias = EnvFloat("MP_ROOM_ENV_VOLUME_BIAS", 0.25f);
  const float ambient = AmbientScale();
  const float grey = 0.18f * gain;
  out = {};
  BlendSource sources[kMaxBlend];
  const size_t sourceCount = ProbeBlend() ? BlendSources(sources) : 0;
  if (sourceCount != 0) {
    // Remastered's blend around the camera: every model reflects the same cube, scaled by
    // the blended intensity (a lone probe's cube by its share of it, the rest being black).
    const Blend& blend = sBlend.blend;
    const BlendSource* heaviest = &sources[0];
    float average = 0.f;
    uint32_t mips = sources[0].gpu->mipCount;
    for (size_t i = 0; i < sourceCount; ++i) {
      heaviest = sources[i].weight > heaviest->weight ? &sources[i] : heaviest;
      average += sources[i].weight * sources[i].gpu->average;
      mips = std::min(mips, sources[i].gpu->mipCount);
    }
    float share = 1.f;
    const ProbeBlendState& s = sBlend;
    if (sourceCount == 1) {
      out.cube = sources[0].gpu->id;
      share = sources[0].weight;
      average = sources[0].gpu->average;
    } else if (s.shown >= 0) {
      // The last blend drawn, which is this one but for a frame where Aurora could not draw.
      out.cube = s.dst[s.shown];
    } else {
      out.cube = heaviest->gpu->id;
      average = heaviest->gpu->average;
      mips = heaviest->gpu->mipCount;
    }
    const float exposure = RoomExposed() ? FrameExposure(*heaviest->area) : 0.f;
    const bool room = exposure > 0.f;
    out.params[0] = room ? exposure * blend.intensity * share * gain : grey / average;
    out.params[1] = std::min(lod, float(mips - 1));
    out.params[2] = float(mips > 2 ? mips - 2 : 0);
    out.params[3] = ambient > 0.f ? 1.f / (room ? average * out.params[0] : grey) : 0.f;
    // Each probe's cube may be turned its own way; the blend takes the heaviest one's.
    std::memcpy(out.worldToCube, heaviest->probe->worldToCube, sizeof(out.worldToCube));
    if (room && blend.max > 1e-6f && std::fabs(blend.intensity) >= 1e-5f) {
      out.occlusionMin = blend.min / blend.intensity;
      out.occlusionInvMax = 1.f / blend.max;
    }
  } else if (located.cube != nullptr) {
    const Probe& probe = *located.probe;
    const GpuCube& gpu = *located.cube;
    // The cube is exposed so that its average direction is middle grey, which is what
    // Remastered's auto exposure aims for (its Tonemap's key is 0.18 too); the lamps in
    // it then come out many times brighter than white, as they should.
    // With the room's exposure the cube keeps its level instead: a probe in a dark
    // corner reflects a dark corner.
    const float exposure = RoomExposed() ? FrameExposure(*located.cubeArea) : 0.f;
    const bool room = exposure > 0.f;
    out.cube = gpu.id;
    out.params[0] = room ? exposure * probe.scale * gain : grey / gpu.average;
    out.params[1] = std::min(lod, float(gpu.mipCount - 1));
    out.params[2] = float(gpu.mipCount > 2 ? gpu.mipCount - 2 : 0);
    out.params[3] = ambient > 0.f ? 1.f / (room ? gpu.average * out.params[0] : grey) : 0.f;
    std::memcpy(out.worldToCube, probe.worldToCube, sizeof(out.worldToCube));
  }
  if (located.volume != 0) {
    const Grid& grid = *located.grid;
    const float* m = grid.worldToGrid;
    const float scale = std::sqrt(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    out.volume = located.volume;
    for (int row = 0; row < 3; ++row) {
      // Point i is the middle of texel i.
      const float size = float(grid.size[row]);
      for (int col = 0; col < 4; ++col) {
        out.worldToVolume[row * 4 + col] = (m[row * 4 + col] + (col == 3 ? 0.5f : 0.f)) / size;
      }
      for (int col = 0; col < 3; ++col) {
        out.worldToAxes[row * 3 + col] = m[row * 4 + col] / scale;
      }
    }
    // The baked light is the level, at the frame's exposure; without one, the grid's
    // average comes out at the key. The grid holds irradiance, and a diffuse surface
    // sends 1/pi of that back.
    const float exposure = RoomExposed() ? FrameExposure(*located.volumeArea) : 0.f;
    out.volumeLevel = (exposure > 0.f ? exposure * gain : 0.18f / grid.average) / 3.14159265f;
    out.volumeBias = volumeBias;
    out.volumeDiagnostic = static_cast<float>(VolumeView());
  }
  if (located.ambientArea != nullptr) {
    const Ambient& sample = located.sample;
    const float average = located.average;
    const float roomExposure = FrameExposure(*located.ambientArea);
    out.hasAmbient = true;
    // The grid gives the light's colour and direction; how bright it is stays the game's
    // ambient, which the shader multiplies in. The baked levels are HDR that Remastered
    // exposes by what is on screen (one room spans 0.0001 to 100), and the world around
    // the model is still lit the retail way. Of the level only this is kept: a spot
    // darker or brighter than its room is, within a factor of two.
    const float luminance = 0.2126f * sample.mean[0] + 0.7152f * sample.mean[1] + 0.0722f * sample.mean[2];
    const float level = std::min(std::max(std::sqrt(luminance / average), 0.5f), 2.f);
    float exposure = luminance > 0.f ? level / luminance * ambient : 0.f;
    if (RoomExposed() && roomExposure > 0.f) {
      // Or the baked level itself, at the room's exposure: the game's ambient is left out.
      exposure = roomExposure * ambient * gain;
      out.ambientAbsolute = true;
    }
    for (int i = 0; i < 3; ++i) {
      out.ambient[0][i] = (sample.mean[i] - sample.lobe[i]) * exposure;
      out.ambient[1][i] = 2.f * sample.lobe[i] * (1.f + sample.sharpness[i]) * exposure;
      out.ambient[2][i] = 1.f + 2.f * sample.sharpness[i];
      std::memcpy(out.ambient[3 + i], sample.direction[i], sizeof(sample.direction[i]));
    }
    if (out.volume == 0 && out.occlusionInvMax > 0.f) {
      // The shader occludes the reflection by the grid at each pixel; without the grid on
      // the GPU, the model's spot stands for all of it. The ambient the cube shapes stays.
      const float brightest = std::max(std::max(sample.mean[0], sample.mean[1]), sample.mean[2]);
      const float seen = std::min(std::max(brightest * out.occlusionInvMax, 0.f), 1.f);
      // Not 0: the shader takes an exposure of 0 for a cube that is not HDR.
      const float factor = std::max(out.occlusionMin + (1.f - out.occlusionMin) * seen, 1e-4f);
      out.params[0] *= factor;
      out.params[3] /= factor;
      out.occlusionMin = 0.f;
      out.occlusionInvMax = 0.f;
    }
  }
  return out.cube != 0 || out.hasAmbient || out.volume != 0;
}

} // namespace

bool Select(const float origin[3], Selection& out) {
  const float* const pos = sHint ? sHintCentre : origin;
  SelectKey key;
  std::memcpy(key.pos, pos, sizeof(key.pos));
  key.hint = sHint;
  key.hintArea = sHint ? sHintArea : 0;
  if (sLastValid && key == sLastKey) {
    out = sLast;
    return sLastFound;
  }
  sLastKey = key;
  sLastValid = true;
  sLastFound = false;
  if (!Enabled()) {
    return false;
  }
  if (sLocatedEpoch != sEpoch) {
    sLocated.clear();
    sLocatedOld.clear();
    sLocatedEpoch = sEpoch;
  }
  const Located* located = nullptr;
  const auto found = sLocated.find(key);
  if (found != sLocated.end()) {
    located = &found->second;
  } else {
    if (sLocated.size() >= kMaxLocated) {
      sLocatedOld.swap(sLocated);
      sLocated.clear();
    }
    const auto old = sLocatedOld.find(key);
    if (old != sLocatedOld.end()) {
      located = &sLocated.emplace(key, old->second).first->second;
    } else {
      Located fresh;
      Locate(pos, fresh);
      located = &sLocated.emplace(key, fresh).first->second;
    }
  }
  sLastFound = Compose(*located, sLast);
  out = sLast;
  return sLastFound;
}

void Stats(int& areas, int& probes, int& cubes, int& grids) {
  areas = probes = cubes = grids = 0;
  for (const auto& [mrea, area] : sAreas) {
    if (!area.file.probes.empty() || !area.file.grids.empty()) {
      ++areas;
      probes += int(area.file.probes.size());
      grids += int(area.file.grids.size());
    }
    for (const GpuCube& cube : area.cubes) {
      cubes += cube.id != 0 ? 1 : 0;
    }
  }
}

std::string Info(const float pos[3]) {
  std::string out;
  char line[320];
  if (sFrame.started) {
    const FrameState& f = sFrame;
    std::snprintf(line, sizeof(line),
                  "frame: EV %g towards %g (%s), exposure %g, sigma %g, measurement %u%s, tone EV %g mid %g "
                  "contrast %g toe %g shoulder %g, static EV %g (glow x%g)\n",
                  f.ev.value, f.targetEv, f.measured ? "measured" : "probes", f.exposure, f.sigma, f.serial,
                  f.measuring ? " (measuring)" : "", f.shown[0], f.shown[1], f.shown[2], f.shown[3], f.shown[4],
                  f.shown[5], GlowScale());
    out += line;
  }
  if (ProbeBlend()) {
    const ProbeBlendState& s = sBlend;
    BlendSource sources[kMaxBlend];
    const size_t count = BlendSources(sources);
    char cube[48] = "";
    if (count >= 2 && s.shown >= 0) {
      std::snprintf(cube, sizeof(cube), ", blended cube %u", s.dst[s.shown]);
    }
    std::snprintf(line, sizeof(line), "probe blend at %.2f %.2f %.2f: intensity %g, occlusion %g..%g%s%s\n",
                  s.view[0], s.view[1], s.view[2], s.blend.intensity, s.blend.min, s.blend.max, cube,
                  s.blend.entries.empty() ? " (no probe: each model picks)" : "");
    out += line;
    for (const BlendEntry& entry : s.blend.entries) {
      Area* area = nullptr;
      const Probe* probe = nullptr;
      if (FindProbe(entry.key, area, probe)) {
        bool inside = false;
        const float fade = ProbeFade(*probe, s.view, inside);
        std::snprintf(line, sizeof(line), "  %08X probe %u: weight %g, fade %g%s, priority %d\n",
                      uint32_t(entry.key >> 32), uint32_t(entry.key), entry.weight, fade, inside ? " (inside)" : "",
                      probe->priority);
        out += line;
      }
    }
  } else {
    out += "probe blend off: each model picks a probe\n";
  }
  for (const auto& [mrea, area] : sAreas) {
    const File& file = area.file;
    if (file.probes.empty() && file.grids.empty()) {
      continue;
    }
    const float* const t = file.tonemap;
    std::snprintf(line, sizeof(line),
                  "%08X%s: exposure %g (EV %g, hint %g..%g, bias %g), tone EV %g mid %g contrast %g toe %g "
                  "shoulder %g, %zu probe(s), %zu cube(s), %zu grid(s)\n",
                  mrea, mrea == sViewArea ? " (camera)" : "", area.exposure,
                  area.exposure > 0.f ? 3.f - std::log2(area.exposure) : 0.f, file.exposure[0], file.exposure[1],
                  file.exposureBias, t[0], t[1], file.contrast, t[2], t[3], file.probes.size(), file.cubes.size(),
                  file.grids.size());
    out += line;
    for (size_t i = 0; i < file.grades.size(); ++i) {
      const Grade& grade = file.grades[i];
      std::snprintf(line, sizeof(line), "  grade %zu: layer %d, fade in %g out %g, LUT %08X%s\n", i, grade.layer,
                    grade.fadeIn, grade.fadeOut, grade.id, grade.id == 0 ? " (identity)" : "");
      out += line;
    }
    const Pick pick = PickProbe(file, pos);
    for (size_t i = 0; i < file.probes.size(); ++i) {
      const Probe& probe = file.probes[i];
      const GpuCube& cube = area.cubes[probe.cube];
      // The box's centre solves worldToBox * p = 0.
      const float* const m = probe.worldToBox;
      const float a[9] = {m[0], m[1], m[2], m[4], m[5], m[6], m[8], m[9], m[10]};
      const float det = a[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (a[3] * a[8] - a[5] * a[6]) +
                        a[2] * (a[3] * a[7] - a[4] * a[6]);
      float centre[3] = {};
      if (std::fabs(det) > 1e-12f) {
        const float b[3] = {-m[3], -m[7], -m[11]};
        centre[0] = (b[0] * (a[4] * a[8] - a[5] * a[7]) - a[1] * (b[1] * a[8] - a[5] * b[2]) +
                     a[2] * (b[1] * a[7] - a[4] * b[2])) / det;
        centre[1] = (a[0] * (b[1] * a[8] - a[5] * b[2]) - b[0] * (a[3] * a[8] - a[5] * a[6]) +
                     a[2] * (a[3] * b[2] - b[1] * a[6])) / det;
        centre[2] = (a[0] * (a[4] * b[2] - b[1] * a[7]) - a[1] * (a[3] * b[2] - b[1] * a[6]) +
                     b[0] * (a[3] * a[7] - a[4] * a[6])) / det;
      }
      bool inside = false;
      const float fade = ProbeFade(probe, pos, inside);
      std::snprintf(line, sizeof(line),
                    "  probe %zu%s: centre %.1f %.1f %.1f, fade %g, cube %u, scale %g, padding %g, priority %d, "
                    "intensity %g..%g, average %g, peak %g%s\n",
                    i, int(i) == pick.probe ? " (picked)" : "", centre[0], centre[1], centre[2],
                    fade, probe.cube, probe.scale, probe.padding, probe.priority,
                    probe.intensityMin, probe.intensityMax, cube.average * probe.scale, cube.peak * probe.scale,
                    cube.failed ? ", black" : "");
      out += line;
    }
    for (size_t i = 0; i < file.grids.size(); ++i) {
      const Grid& grid = file.grids[i];
      const float distance = GridDistance(grid, pos);
      Ambient ambient;
      const bool lit = distance == 0.f && SampleGrid(file, grid, pos, ambient);
      int used = std::snprintf(line, sizeof(line), "  grid %zu: %u x %u x %u, average %g, ", i, grid.size[0],
                               grid.size[1], grid.size[2], grid.average);
      if (lit) {
        std::snprintf(line + used, sizeof(line) - used, "here mean %g %g %g, lobe %g %g %g\n", ambient.mean[0],
                      ambient.mean[1], ambient.mean[2], ambient.lobe[0], ambient.lobe[1], ambient.lobe[2]);
      } else if (distance == 0.f) {
        std::snprintf(line + used, sizeof(line) - used, "no lit point here\n");
      } else {
        std::snprintf(line + used, sizeof(line) - used, "%.1f m away\n", distance);
      }
      out += line;
    }
  }
  return out;
}

} // namespace PortRoomEnv
