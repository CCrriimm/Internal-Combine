#pragma once

// Input-noise helper for the external build.
//
// The Steam NEW DECOMP material does not justify the old experimental inline
// hooks, RWX trampolines, debug-port spoofing, or RUNASINVOKER relaunch. This
// module therefore has a deliberately small attack surface: it seeds and applies
// input noise, and performs no game-memory writes or code hooks.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <cstdint>

namespace ac_bypass
{
  // Seeds the noise generator. Safe to call more than once; only the first call
  // has an effect. ApplyMouseNoise also performs this initialization lazily.
  bool Init();

  // Lifecycle symmetry only. There are no hooks or allocated trampolines to
  // remove in the Steam-safe implementation.
  void Shutdown();

  bool InputNoiseReady();

  // Adds a small, non-deterministic perturbation to meaningful corrections and
  // softens large velocity steps before the output backend rounds the delta to
  // integer pixels. Sub-pixel/centered corrections do not random-walk.
  void ApplyMouseNoise(float &dx, float &dy);

  // Exposed for deterministic tests. Runtime code should use Init().
  void SeedNoise(std::uint64_t seed);
} // namespace ac_bypass
