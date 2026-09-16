// probe_common.cpp — implementation of the shared helpers declared in
// probe_common.h.
//
// Everything here is deliberately dependency-free (no C++/WinRT, no D3D) so it
// can be linked into any TU of the probe without dragging headers along.
//
// Two conventions the rest of the probe relies on:
//   * Log() writes to stdout, prefixes every line with milliseconds since the
//     first use of the clock (which main() forces to be its own first
//     statement) and fflush()es. The Python harness synchronises on these
//     lines, so buffering them would deadlock a --stdin-sync run.
//   * No helper here ever swallows a failure silently: HrString() always
//     produces a string, falling back to FormatMessageA and finally to a
//     "no description" marker rather than returning empty.

#include "probe_common.h"

#include <dxgi.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace np {

namespace {

// QueryPerformanceFrequency is constant for the lifetime of the process
// (documented since Windows XP), so it is queried exactly once.
struct QpcClock {
    int64_t freq = 1;
    int64_t start = 0;

    QpcClock() {
        LARGE_INTEGER f{};
        if (QueryPerformanceFrequency(&f) && f.QuadPart != 0) {
            freq = f.QuadPart;
        }
        LARGE_INTEGER c{};
        if (QueryPerformanceCounter(&c)) {
            start = c.QuadPart;
        }
    }
};

const QpcClock& Clock() {
    // Function-local static: thread-safe initialisation, and the "process
    // start" baseline is the first call rather than a global-ctor ordering
    // accident.
    static const QpcClock clock;
    return clock;
}

void VLogTo(FILE* stream, const char* fmt, va_list args) {
    // Read the baseline FIRST: on the very first log line the argument
    // evaluation order would otherwise construct Clock() after QpcNow() ran and
    // print a small negative timestamp.
    const int64_t start = Clock().start;
    const double ms = QpcToMs(QpcNow() - start);
    std::fprintf(stream, "[%10.3f] ", ms);
    std::vfprintf(stream, fmt, args);
    std::fputc('\n', stream);
    // The Python side reads these pipes line by line; without this flush a
    // --stdin-sync run deadlocks (we wait for a line, it waits for ours).
    std::fflush(stream);
}

}  // namespace

int64_t QpcNow() {
    LARGE_INTEGER c{};
    if (!QueryPerformanceCounter(&c)) {
        return 0;
    }
    return c.QuadPart;
}

double QpcToMs(int64_t ticks) {
    return static_cast<double>(ticks) * 1000.0 / static_cast<double>(Clock().freq);
}

const char* CompositionModeName(int m) {
    switch (m) {
        case 0: return "COMPOSED";              // DXGI_FRAME_PRESENTATION_MODE_COMPOSED
        case 1: return "OVERLAY";               // DXGI_FRAME_PRESENTATION_MODE_OVERLAY
        case 2: return "NONE";                  // DXGI_FRAME_PRESENTATION_MODE_NONE
        case 3: return "COMPOSITION_FAILURE";   // DXGI_FRAME_PRESENTATION_MODE_COMPOSITION_FAILURE
        default: return "n/a";                  // -1: IDXGISwapChainMedia unavailable
    }
}

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    VLogTo(stdout, fmt, args);
    va_end(args);
}

void LogErr(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    VLogTo(stderr, fmt, args);
    va_end(args);
}

std::string HrString(HRESULT hr) {
    const char* name = nullptr;
    switch (hr) {
        case S_OK:                                 name = "S_OK"; break;
        case S_FALSE:                              name = "S_FALSE"; break;
        case E_FAIL:                               name = "E_FAIL"; break;
        case E_INVALIDARG:                         name = "E_INVALIDARG"; break;
        case E_ACCESSDENIED:                       name = "E_ACCESSDENIED"; break;
        case E_NOINTERFACE:                        name = "E_NOINTERFACE"; break;
        case E_NOTIMPL:                            name = "E_NOTIMPL"; break;
        case E_OUTOFMEMORY:                        name = "E_OUTOFMEMORY"; break;
        case E_POINTER:                            name = "E_POINTER"; break;
        case E_HANDLE:                             name = "E_HANDLE"; break;
        case DXGI_ERROR_DEVICE_REMOVED:            name = "DXGI_ERROR_DEVICE_REMOVED"; break;
        case DXGI_ERROR_DEVICE_RESET:              name = "DXGI_ERROR_DEVICE_RESET"; break;
        case DXGI_ERROR_DEVICE_HUNG:               name = "DXGI_ERROR_DEVICE_HUNG"; break;
        case DXGI_ERROR_DRIVER_INTERNAL_ERROR:     name = "DXGI_ERROR_DRIVER_INTERNAL_ERROR"; break;
        case DXGI_ERROR_INVALID_CALL:              name = "DXGI_ERROR_INVALID_CALL"; break;
        case DXGI_ERROR_UNSUPPORTED:               name = "DXGI_ERROR_UNSUPPORTED"; break;
        case DXGI_ERROR_FRAME_STATISTICS_DISJOINT: name = "DXGI_ERROR_FRAME_STATISTICS_DISJOINT"; break;
        case DXGI_ERROR_WAS_STILL_DRAWING:         name = "DXGI_ERROR_WAS_STILL_DRAWING"; break;
        case DXGI_ERROR_NOT_FOUND:                 name = "DXGI_ERROR_NOT_FOUND"; break;
        case DXGI_ERROR_MORE_DATA:                 name = "DXGI_ERROR_MORE_DATA"; break;
        case DXGI_ERROR_ACCESS_DENIED:             name = "DXGI_ERROR_ACCESS_DENIED"; break;
        case DXGI_ERROR_ACCESS_LOST:               name = "DXGI_ERROR_ACCESS_LOST"; break;
        case DXGI_ERROR_WAIT_TIMEOUT:              name = "DXGI_ERROR_WAIT_TIMEOUT"; break;
        case RPC_E_CHANGED_MODE:                   name = "RPC_E_CHANGED_MODE"; break;
        default: break;
    }

    char head[32];
    std::snprintf(head, sizeof(head), "0x%08lX", static_cast<unsigned long>(hr));

    std::string out(head);
    if (name != nullptr) {
        out += " (";
        out += name;
        out += ")";
        return out;
    }

    // Unknown code: ask the system. Never leave the caller with a bare number
    // if Windows can describe it.
    char* buf = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&buf),
        0,
        nullptr);

    if (n != 0 && buf != nullptr) {
        std::string msg(buf, n);
        while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n' ||
                                msg.back() == ' ' || msg.back() == '.')) {
            msg.pop_back();
        }
        out += " (";
        out += msg;
        out += ")";
    } else {
        out += " (no system description)";
    }
    if (buf != nullptr) {
        LocalFree(buf);
    }
    return out;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) {
        return std::string();
    }
    const int need = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                         nullptr, 0, nullptr, nullptr);
    if (need <= 0) {
        return std::string();
    }
    std::string out(static_cast<size_t>(need), '\0');
    const int got = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                                        out.data(), need, nullptr, nullptr);
    if (got <= 0) {
        return std::string();
    }
    out.resize(static_cast<size_t>(got));
    return out;
}

}  // namespace np
