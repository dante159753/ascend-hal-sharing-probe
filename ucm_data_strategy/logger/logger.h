#pragma once

#include <cstdio>
#include <fmt/format.h>
#include <unistd.h>

namespace UC::Demo {

inline size_t rank = 0;

template <typename... Args>
void Log(const char* level, fmt::format_string<Args...> message, Args&&... args)
{
    fmt::print(stderr, "[pid={} rank={}] {} {}\n", getpid(), rank, level,
               fmt::format(message, std::forward<Args>(args)...));
    std::fflush(stderr);
}

}  // namespace UC::Demo

#define UC_INFO(...) ::UC::Demo::Log("INFO", __VA_ARGS__)
#define UC_WARN(...) ::UC::Demo::Log("WARN", __VA_ARGS__)
#define UC_ERROR(...) ::UC::Demo::Log("ERROR", __VA_ARGS__)
