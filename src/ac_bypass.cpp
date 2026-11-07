// Input-noise helper for the Steam external build.
//
// NEW DECOMP's QSec code validates its own executable regions and checks game
// recoil/environment state. It does not support the old module's claim that
// patching this external process's NtQueryInformationProcess/GetThreadContext
// would provide protection. Those hooks and their RWX trampolines are gone.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include "ac_bypass.hpp"

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
  // xoshiro256** (Blackman & Vigna), seeded once per process.
  std::uint64_t g_prng[4] = {0x123456789ABCDEF0ULL,
                             0xFEDCBA9876543210ULL,
                             0x0F1E2D3C4B5A6978ULL,
                             0x8796A5B4C3D2E1F0ULL};
  bool g_noiseReady = false;

  inline std::uint64_t rotl64(std::uint64_t x, int k)
  {
    return (x << k) | (x >> (64 - k));
  }

  std::uint64_t prng_next()
  {
    const std::uint64_t result = rotl64(g_prng[1] * 5, 7) * 9;
    const std::uint64_t t = g_prng[1] << 17;
    g_prng[2] ^= g_prng[0];
    g_prng[3] ^= g_prng[1];
    g_prng[1] ^= g_prng[2];
    g_prng[0] ^= g_prng[3];
    g_prng[2] ^= t;
    g_prng[3] = rotl64(g_prng[3], 45);
    return result;
  }

  float prng_float()
  {
    const std::uint64_t bits =
        (prng_next() >> 11) | 0x3FF0000000000000ULL;
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return static_cast<float>(value - 1.0);
  }

  void gaussian_pair(float sigma, float &outX, float &outY)
  {
    float u = 0.0f;
    float v = 0.0f;
    float s = 0.0f;
    do
    {
      u = prng_float() * 2.0f - 1.0f;
      v = prng_float() * 2.0f - 1.0f;
      s = u * u + v * v;
    } while (s >= 1.0f || s == 0.0f);

    const float multiplier =
        sigma * std::sqrt(-2.0f * std::log(s) / s);
    outX = u * multiplier;
    outY = v * multiplier;
  }

  inline float smoothstep(float t) { return t * t * (3.0f - 2.0f * t); }
} // namespace

namespace ac_bypass
{
  void SeedNoise(std::uint64_t seed)
  {
    // Splitmix64 expansion. Adding a different non-zero constant to each lane
    // also prevents the all-zero state if a caller supplies seed == 0.
    auto splitmix = [](std::uint64_t value)
    {
      value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ULL;
      value = (value ^ (value >> 27)) * 0x94D049BB133111EBULL;
      return value ^ (value >> 31);
    };

    constexpr std::uint64_t increment = 0x9E3779B97F4A7C15ULL;
    for (std::uint64_t &lane : g_prng)
    {
      seed += increment;
      lane = splitmix(seed);
    }
    g_noiseReady = true;
  }

  bool Init()
  {
    if (g_noiseReady)
      return true;

    LARGE_INTEGER counter{};
    QueryPerformanceCounter(&counter);
    std::uint64_t seed = static_cast<std::uint64_t>(counter.QuadPart);
    seed ^= static_cast<std::uint64_t>(__rdtsc());
    seed ^= static_cast<std::uint64_t>(GetTickCount64()) << 21;
    seed ^= static_cast<std::uint64_t>(GetCurrentProcessId()) << 32;
    seed ^= static_cast<std::uint64_t>(GetCurrentThreadId()) << 7;
    seed ^= reinterpret_cast<std::uintptr_t>(&counter);
    SeedNoise(seed);
    return true;
  }

  void Shutdown() { g_noiseReady = false; }

  bool InputNoiseReady() { return g_noiseReady; }

  void ApplyMouseNoise(float &dx, float &dy)
  {
    if (!g_noiseReady)
      Init();

    const float inputMagnitude = std::sqrt(dx * dx + dy * dy);
    // Do not turn an already-centered target into a random walk. Keep the
    // established noise on real corrections, fading it in over small deltas so
    // micro-aim remains stable without disabling the output backend.
    if (inputMagnitude < 1.0f)
      return;

    float noiseX = 0.0f;
    float noiseY = 0.0f;
    const float noiseScale = std::min(inputMagnitude / 4.0f, 1.0f);
    gaussian_pair(0.75f * noiseScale, noiseX, noiseY);
    dx += noiseX;
    dy += noiseY;

    // Preserve the live-tested response curve: only large corrections are
    // softened, and micro-aim is not put through an additional smoothing ramp.
    const float magnitude = std::sqrt(dx * dx + dy * dy);
    if (magnitude >= 4.0f)
    {
      const float t = smoothstep(std::min(magnitude / 40.0f, 1.0f));
      const float scale = 0.3f * t + 0.7f;
      dx *= scale;
      dy *= scale;
    }
  }
} // namespace ac_bypass
