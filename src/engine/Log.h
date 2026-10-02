#pragma once

// Console logging: LOG_INFO("Loaded " << count << " models\n").
// Release builds have no console window, so there the arguments are never evaluated and no code is emitted.
#ifdef NDEBUG

struct LogNullStream {
    template <typename T>
    const LogNullStream& operator<<(const T&) const { return *this; }
};

// sizeof keeps the arguments referenced (no unused-variable warnings) without evaluating them.
#define LOG_INFO(...) ((void)sizeof(LogNullStream{} << __VA_ARGS__))
#define LOG_ERROR(...) LOG_INFO(__VA_ARGS__)

#else

#include <iostream>

#define LOG_INFO(...) (std::cout << __VA_ARGS__)
#define LOG_ERROR(...) (std::cerr << __VA_ARGS__)

#endif
