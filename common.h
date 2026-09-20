#pragma once
#include <acl/acl.h>
#include <ascend_hal.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

#define LOG_INFO(...) do { \
    std::fprintf(stdout, "[INFO] "); \
    std::fprintf(stdout, __VA_ARGS__); \
    std::fprintf(stdout, "\n"); \
    std::fflush(stdout); \
} while (false)

#define LOG_ERROR(...) do { \
    std::fprintf(stderr, "[ERROR] "); \
    std::fprintf(stderr, __VA_ARGS__); \
    std::fprintf(stderr, "\n"); \
} while (false)

#define CHECK(call) do { \
    const auto check_ret = (call); \
    if (check_ret != 0) { \
        LOG_ERROR("%s failed: ret=%ld", #call, static_cast<long>(check_ret)); \
        return static_cast<int32_t>(check_ret); \
    } \
} while (false)

#define CHECK_GOTO(call, label) do { \
    const auto check_ret = (call); \
    if (check_ret != 0) { \
        LOG_ERROR("%s failed: ret=%ld", #call, static_cast<long>(check_ret)); \
        goto label; \
    } \
} while (false)
