#pragma once
#include <array>
#include <cstdint>
#include <cstring>

// Compile-time XOR string obfuscation.
// Key must be nonzero; change it per build to prevent static signature matching.
inline constexpr uint8_t kObfKey = 0x5C;

template<std::size_t N>
struct ObfStr {
    std::array<char, N> data;
    constexpr ObfStr(const char (&s)[N]) {
        for (std::size_t i = 0; i < N; ++i)
            data[i] = static_cast<char>(s[i] ^ kObfKey);
    }
    // Decoded into a stack buffer; valid for the duration of the calling scope.
    // Usage: auto s = OBF("BloodStrike.exe"); use s.c_str();
    std::array<char, N> decode() const {
        std::array<char, N> out;
        for (std::size_t i = 0; i < N; ++i)
            out[i] = static_cast<char>(data[i] ^ kObfKey);
        return out;
    }
};

#ifdef COMET_OBFUSCATE
  // Returns a stack-allocated decoded char array.
  // MUST be used in an expression that keeps it alive (assign to auto).
  #define OBF(s) ([]{ static constexpr ObfStr<sizeof(s)> _o(s); return _o.decode(); }())
#else
  // Pass-through: returns the literal as a std::array<char,N> for consistent API.
  #define OBF(s) ([](){ std::array<char,sizeof(s)> _a{}; memcpy(_a.data(),(s),sizeof(s)); return _a; }())
#endif
