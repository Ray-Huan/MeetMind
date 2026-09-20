/*
 * MeetMind — mingw-w64 兼容声明注入（仅用于第三方 ggml 目标）
 *
 * 背景
 *   whisper.cpp 的 ggml-cpu 在 `_WIN32_WINNT >= 0x0602` 时会调用
 *   SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, ...)，
 *   目的是告诉 Windows 11 不要把这些线程降频/停核，从而在多线程推理时保住性能。
 *
 *   但 Qt 6.7.1 自带的 mingw-w64（GCC 13.1.0）只提供了 PROCESS_ 变体的结构体与常量，
 *   缺少 THREAD_POWER_THROTTLING_STATE / THREAD_POWER_THROTTLING_CURRENT_VERSION /
 *   THREAD_POWER_THROTTLING_EXECUTION_SPEED，导致编译失败。
 *
 * 做法
 *   由 CMake 在配置期做一次编译探测（check_c_source_compiles）：
 *     * 若工具链已具备这些声明 → 本文件不会被使用；
 *     * 若缺失 → 本文件以 `-include` 方式强制注入到 ggml-cpu 目标，
 *                仅补齐缺失的声明，不修改任何第三方源码。
 *
 * 数值来源：Windows SDK（winnt.h / processthreadsapi.h）
 *   THREAD_POWER_THROTTLING_CURRENT_VERSION = 1
 *   THREAD_POWER_THROTTLING_EXECUTION_SPEED = 0x1
 */
#ifndef MEETMIND_MM_WIN_THREAD_THROTTLING_SHIM_H
#define MEETMIND_MM_WIN_THREAD_THROTTLING_SHIM_H

#if defined(_WIN32)

#include <windows.h>

#if defined(_WIN32_WINNT) && _WIN32_WINNT >= 0x0602

#ifndef THREAD_POWER_THROTTLING_CURRENT_VERSION
#define THREAD_POWER_THROTTLING_CURRENT_VERSION 1
#endif

#ifndef THREAD_POWER_THROTTLING_EXECUTION_SPEED
#define THREAD_POWER_THROTTLING_EXECUTION_SPEED 0x1
#endif

#ifndef THREAD_POWER_THROTTLING_VALID_FLAGS
#define THREAD_POWER_THROTTLING_VALID_FLAGS THREAD_POWER_THROTTLING_EXECUTION_SPEED
#endif

typedef struct _MEETMIND_THREAD_POWER_THROTTLING_STATE {
    ULONG Version;
    ULONG ControlMask;
    ULONG StateMask;
} THREAD_POWER_THROTTLING_STATE, *PTHREAD_POWER_THROTTLING_STATE;

#endif  /* _WIN32_WINNT >= 0x0602 */

#endif  /* _WIN32 */

#endif  /* MEETMIND_MM_WIN_THREAD_THROTTLING_SHIM_H */
