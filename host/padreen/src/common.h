#pragma once
// Padreen common helpers: clock, logging, process tuning.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ph {

// Monotonic nanoseconds on the same clock as Python's time.perf_counter_ns() (QueryPerformanceCounter).
int64_t nowNs();
inline double ms(int64_t ns) { return ns / 1e6; }

void setLogFile(const wchar_t* path);
void logf(const char* fmt, ...);

std::wstring widen(const std::string& s);
std::string narrow(const std::wstring& s);
std::wstring exeDir();

// Opt out of EcoQoS / timer coalescing, request a 1 ms timer, raise the priority class.  Process-local only.
std::string boostProcess();
// Latency-critical thread: time-critical priority + MMCSS "Games" class.  Returns a handle to release with endThreadBoost.
void* beginThreadBoost(const wchar_t* task);
void endThreadBoost(void* h);

}  // namespace ph
