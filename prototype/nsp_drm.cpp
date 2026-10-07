// nsp_drm.cpp — see nsp_drm.h for the three contracts this file exists to keep.
//
// The process half is a line-for-line port of tools/drm-probe/drm_probe.py's `tree`
// command: the same child selection, the same PID-reuse guard, the same matcher. The
// probe exists to MEASURE what the engine will ship, so any drift between the two makes
// the probe's numbers describe a rule the product does not use.

#include "nsp_drm.h"

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

// The PEB walk below hard-codes the x64 layout. A 32-bit build would read a PEB32
// through 64-bit offsets and get plausible garbage, which is the one failure mode this
// module must never have — fail at compile time instead.
static_assert(sizeof(void*) == 8, "nsp_drm.cpp reads the x64 PEB layout; build x64");

namespace nsp {
namespace {

// ----------------------------------------------------------------- tuning

// Contract 2. A cold full scan of a 29-child Chrome MEASURED 11 ms (2026-09-20,
// tools/drm-probe) — nearly two frames at 165 Hz. Once a second on a thread of its own
// it is nothing; in the tick loop it would be a dropped frame every second.
constexpr DWORD kScanPeriodMs = 1000;

// Contract 3. How long a protected verdict is HELD after the last sighting of the
// signal that produced it. Two reasons, and the second is the one that matters: Chrome
// keeps the CDM utility process alive for a while after the user navigates away, and it
// tears one down and builds another between two protected items — a verdict that
// tracked the process exactly would open a generation window inside that gap.
// `unverified`: the design assumes 10 s and the probe's Q3 (`drm_probe.py watch`) is
// what would turn that into a measurement. It has not been run.
constexpr double kCdmGoneDwellSec = 10.0;

// ---- signal (b), the dead video rect. EVERY number here is `unverified`. Nobody has
// measured them against real protected content on this machine, because the owner has
// no subscription to any (stated 2026-09-20); they are reasoning, and a log line that
// says kDeadPicture is a hypothesis until `drm_probe.py band` has run on a real
// protected frame and on the three false-positive cases the probe's README names.
//
// kDeadDarkFrac is the SAME quantity the probe's README calls kDeadBandFrac, seen from
// the other side: the probe prints the bright-block fraction, the engine hands us the
// dark one, so kDeadDarkFrac == 1 - kDeadBandFrac. 0.998 leaves two blocks in a
// thousand alive before the picture counts as dead.
//
// Why this must not fire on genuine darkness, and why the dwell carries most of that
// weight rather than the threshold:
//   - A night scene keeps specular highlights, and even where it does not, codec noise
//     and film grain lift whole blocks above 16/255. It also does not last: a scene that
//     is that dark for three unbroken seconds is a fade, not a scene.
//   - A fade to black passes THROUGH the threshold rather than sitting on it. kDeadSec
//     at 3.0 s is far longer than any fade a film cuts on, and the probe's README is
//     explicit that it stays at 3.0 s so no fade can reach it.
//   - Letterbox bars are outside the band by construction: the band is y 0.20..0.72 of
//     the video rect, and a 2.39:1 film inside a 16:9 rect puts its bars at roughly
//     0.10 and 0.90.
// If the probe ever shows these are wrong, raise kDeadSec before lowering
// kDeadDarkFrac: the dwell costs latency on protected content we would skip anyway,
// the threshold costs a false skip on content the user is watching.
constexpr double kDeadDarkFrac = 0.998;
constexpr double kDeadSec      = 3.0;

// The picture summary is evidence only while it is fresh. If the engine stops feeding
// us — it disengaged, the capture stalled, the window went away — a stale "the rect was
// black" must not keep the verdict protected for the kCdmGoneDwellSec hold on its own.
// `unverified`: 2 s is roughly two missed 1 Hz scans, chosen so an engine that
// summarises once a frame cannot trip it by being briefly late.
constexpr double kPictureStaleSec = 2.0;

// A Chrome browser process MEASURED 29 children (2026-09-20). 256 bounds what a
// pathological tree — or a stale th32ParentProcessID, which is not reference counted —
// can cost us per scan. Hitting it is itself a reason to fail closed: a scan that did
// not look at every child cannot say the tree is clean.
constexpr int kMaxChildren = 256;

// Chrome's utility command lines run to a few KB. 32768 wchar_t is a sanity bound on a
// length we read out of another process's memory, not a real limit.
constexpr size_t kMaxCmdLineChars = 32768;

// ------------------------------------------------------------------ ntdll
//
// NtQueryInformationProcess is resolved by hand and ntdll is never linked against: it
// is not a supported link target, and a missing export must degrade to "we could not
// read this child" (kUnknown) rather than refusing to start the process.
// GetModuleHandleW, not LoadLibraryW — ntdll is mapped into every process before our
// first instruction runs, so the handle needs no reference and no free.

using NspNtStatus = LONG;
using PfnNtQueryInformationProcess = NspNtStatus(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

constexpr ULONG kProcessBasicInformation = 0;
constexpr ULONG kProcessCommandLineInformation = 60;  // Win8.1+
constexpr NspNtStatus kStatusSuccess = 0;
constexpr NspNtStatus kStatusInfoLengthMismatch = static_cast<NspNtStatus>(0xC0000004u);

struct NspProcessBasicInformation {
    NspNtStatus ExitStatus;
    PVOID       PebBaseAddress;
    ULONG_PTR   AffinityMask;
    LONG        BasePriority;
    ULONG_PTR   UniqueProcessId;
    ULONG_PTR   InheritedFromUniqueProcessId;
};

// UNICODE_STRING as it is laid out in the TARGET process. `Buffer` is an address over
// there and is never dereferenced here — only handed to ReadProcessMemory.
struct NspRemoteUnicodeString {
    USHORT Length;  // BYTES, not characters
    USHORT MaximumLength;
    PVOID  Buffer;
};
static_assert(sizeof(NspRemoteUnicodeString) == 16, "x64 UNICODE_STRING is 16 bytes");

// The published x64 offsets: PEB::ProcessParameters at 0x20 and
// RTL_USER_PROCESS_PARAMETERS::CommandLine at 0x70. They are not a documented contract
// and nothing in this repo asserts them at run time — which is exactly why every read
// below is checked and every failure lands on "unreadable", i.e. kUnknown, rather than
// on a string we then match against.
constexpr SIZE_T kPebProcessParametersOffset = 0x20;
constexpr SIZE_T kProcParamsCommandLineOffset = 0x70;

PfnNtQueryInformationProcess ResolveNtQueryInformationProcess() {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return nullptr;
    return reinterpret_cast<PfnNtQueryInformationProcess>(
        reinterpret_cast<void*>(GetProcAddress(ntdll, "NtQueryInformationProcess")));
}

std::string ErrNo(const char* what) {
    char b[96];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%s err %lu", what, GetLastError());
    return b;
}

std::string NtErr(const char* what, NspNtStatus st) {
    char b[96];
    _snprintf_s(b, sizeof(b), _TRUNCATE, "%s 0x%08lX", what, static_cast<unsigned long>(st));
    return b;
}

// ------------------------------------------------------- command line reads

// The route the design names: ProcessBasicInformation for the PEB, then two
// ReadProcessMemory hops to RTL_USER_PROCESS_PARAMETERS::CommandLine. Needs
// PROCESS_VM_READ on top of PROCESS_QUERY_LIMITED_INFORMATION, which a medium-IL
// process of the same user does get for Chrome's low-IL sandboxed children — the
// mandatory integrity policy is NO_WRITE_UP, so reading DOWN is allowed.
bool ReadCommandLineViaPeb(HANDLE h, PfnNtQueryInformationProcess ntq, std::wstring* out,
                           std::string* why) {
    NspProcessBasicInformation pbi{};
    ULONG got = 0;
    const NspNtStatus st = ntq(h, kProcessBasicInformation, &pbi, sizeof(pbi), &got);
    if (st != kStatusSuccess || got < sizeof(pbi) || !pbi.PebBaseAddress) {
        *why = NtErr("NtQuery(basic)", st);
        return false;
    }

    PVOID params = nullptr;
    SIZE_T read = 0;
    const auto peb = reinterpret_cast<const BYTE*>(pbi.PebBaseAddress);
    if (!ReadProcessMemory(h, peb + kPebProcessParametersOffset, &params, sizeof(params), &read) ||
        read != sizeof(params) || !params) {
        *why = ErrNo("read PEB");
        return false;
    }

    NspRemoteUnicodeString us{};
    const auto pp = reinterpret_cast<const BYTE*>(params);
    if (!ReadProcessMemory(h, pp + kProcParamsCommandLineOffset, &us, sizeof(us), &read) ||
        read != sizeof(us)) {
        *why = ErrNo("read params");
        return false;
    }
    if (!us.Buffer || us.Length == 0) {
        out->clear();  // a real, empty command line: readable, just says nothing
        return true;
    }
    const size_t chars = us.Length / sizeof(wchar_t);
    if (chars > kMaxCmdLineChars) {
        *why = "command line implausibly long";
        return false;
    }

    out->assign(chars, L'\0');
    if (!ReadProcessMemory(h, us.Buffer, &(*out)[0], chars * sizeof(wchar_t), &read) ||
        read != chars * sizeof(wchar_t)) {
        out->clear();
        *why = ErrNo("read cmdline");
        return false;
    }
    // Chrome writes no terminator inside Length, but a truncated read would leave one.
    const size_t nul = out->find(L'\0');
    if (nul != std::wstring::npos) out->resize(nul);
    return true;
}

// The probe's route (ProcessCommandLineInformation, Win8.1+), kept as a FALLBACK only.
// It needs no PROCESS_VM_READ and it is WOW64-correct, so it is what answers for a
// 32-bit child or one whose memory we may not read. It never widens what we accept: a
// child both routes fail on still counts as unreadable.
bool ReadCommandLineViaNtClass(HANDLE h, PfnNtQueryInformationProcess ntq, std::wstring* out,
                               std::string* why) {
    ULONG need = 0;
    NspNtStatus st = ntq(h, kProcessCommandLineInformation, nullptr, 0, &need);
    if ((st != kStatusInfoLengthMismatch && st != kStatusSuccess) ||
        need < sizeof(NspRemoteUnicodeString)) {
        *why = NtErr("NtQuery(cmdline size)", st);
        return false;
    }
    // uint64_t elements, not bytes: the returned UNICODE_STRING must be 8-aligned, and
    // std::vector<BYTE> makes no such promise.
    std::vector<uint64_t> buf((need + sizeof(uint64_t) - 1) / sizeof(uint64_t), 0);
    st = ntq(h, kProcessCommandLineInformation, buf.data(), need, &need);
    if (st != kStatusSuccess) {
        *why = NtErr("NtQuery(cmdline)", st);
        return false;
    }

    NspRemoteUnicodeString us{};
    memcpy(&us, buf.data(), sizeof(us));
    if (!us.Buffer || us.Length == 0) {
        out->clear();
        return true;
    }
    // The buffer points INSIDE our own block; anything else is a layout we do not know.
    const uintptr_t base = reinterpret_cast<uintptr_t>(buf.data());
    const uintptr_t at = reinterpret_cast<uintptr_t>(us.Buffer);
    const uintptr_t span = static_cast<uintptr_t>(need);
    if (at < base || at - base > span || (at - base) + static_cast<uintptr_t>(us.Length) > span) {
        *why = "cmdline buffer outside the returned block";
        return false;
    }
    const size_t chars = us.Length / sizeof(wchar_t);
    out->assign(reinterpret_cast<const wchar_t*>(at), chars);
    const size_t nul = out->find(L'\0');
    if (nul != std::wstring::npos) out->resize(nul);
    return true;
}

// A process that is on its way out cannot be read and does not need to be: it is
// not going to decrypt anything. MEASURED on a real YouTube run (2026-09-20): one
// of thirteen Chrome children was terminating at the moment of a scan, the PEB read
// returned ERROR_PARTIAL_COPY (299) and the command-line class returned
// STATUS_PROCESS_IS_TERMINATING (0xC000010A), the scan called it unreadable, the
// verdict went UNKNOWN and generation stopped for a second. Chrome starts and kills
// utility processes constantly, so counting a dying one as unknown would flicker the
// picture on and off during ordinary browsing - all of it fail-closed in the wrong
// place. The exit code is the authority: only a child that is STILL_ACTIVE and
// unreadable is evidence of anything.
bool ProcessIsGoingAway(HANDLE h) {
    DWORD code = 0;
    if (!GetExitCodeProcess(h, &code)) return false;  // cannot tell: treat as alive
    return code != STILL_ACTIVE;
}

struct ChildRead {
    bool        readable = false;
    bool        gone = false;      // the child was exiting: nothing to classify, not opaque
    std::wstring cmd;
    std::string why;  // why not, when !readable
};

ChildRead ReadChildCommandLine(DWORD pid, PfnNtQueryInformationProcess ntq) {
    ChildRead r;
    if (!ntq) {
        r.why = "NtQueryInformationProcess unavailable";
        return r;
    }
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    const bool vmRead = h != nullptr;
    if (!h) h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) {
        r.why = ErrNo("OpenProcess");
        return r;  // NOT clean: an unreadable child is kUnknown (contract 1)
    }

    // A 32-bit child's PEB is a PEB32 with different offsets; reading it through the
    // x64 ones would produce a plausible-looking wrong string, which is worse than no
    // string at all. Hand those to the fallback, which is WOW64-correct.
    BOOL wow = FALSE;
    const bool isWow = IsWow64Process(h, &wow) != FALSE && wow != FALSE;

    std::string whyPeb, whyNt;
    if (vmRead && !isWow && ReadCommandLineViaPeb(h, ntq, &r.cmd, &whyPeb)) {
        r.readable = true;
    } else if (ReadCommandLineViaNtClass(h, ntq, &r.cmd, &whyNt)) {
        r.readable = true;
    } else if (ProcessIsGoingAway(h)) {
        // Gone, not opaque. Both flags stay false and `why` stays empty, which the
        // caller reads as "there is nothing here to classify".
        r.gone = true;
    } else {
        r.cmd.clear();
        r.why = whyPeb.empty() ? whyNt : whyPeb + "; " + whyNt;
        if (r.why.empty()) r.why = "unreadable";
    }
    CloseHandle(h);
    return r;
}

// ------------------------------------------------------------ classification
//
// THE MATCHER, kept identical to drm_probe.py's `classify` so the probe measures the
// rule that ships and not an approximation of it. The probe's regexes, in order:
//   --utility-sub-type=([^\s"]+)     the subtype, matched against in preference to the
//                                    whole command line when there is one
//   media\.mojom\.MediaFoundationService   -> hardware DRM (L1 / SL3000)
//   media\.mojom\.(Cdm|Widevine)           -> the library CDM (Widevine L3)
//   media\.mojom\.                          -> something in the media namespace we do
//                                              not recognise
// plus the literal `widevinecdm`, which the engine also treats as a library CDM: the
// CDM utility process carries the path of widevinecdm.dll on its command line, and a
// Chrome that stopped naming a subtype would still carry that.
//
// Deliberately broad on the right-hand side: a rename INSIDE the media.mojom namespace
// must not be able to go silent. Which is also why kUnrecognisedMedia exists and lands
// on kUnknown rather than on kClear — see the risk note at the call site.

enum class CdmKind { kNone, kHardware, kLibrary, kUnrecognisedMedia };

std::wstring ToLower(const std::wstring& s) {
    std::wstring out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return out;
}

// `--utility-sub-type=([^\s"]+)` on an already lower-cased command line.
std::wstring ExtractSubType(const std::wstring& low) {
    static const wchar_t kSwitch[] = L"--utility-sub-type=";
    const size_t at = low.find(kSwitch);
    if (at == std::wstring::npos) return std::wstring();
    size_t i = at + (ARRAYSIZE(kSwitch) - 1);
    const size_t begin = i;
    while (i < low.size() && low[i] != L'"' && !iswspace(low[i])) ++i;
    return low.substr(begin, i - begin);
}

CdmKind ClassifyCommandLine(const std::wstring& cmd, std::wstring* subtypeOut) {
    subtypeOut->clear();
    if (cmd.empty()) return CdmKind::kNone;
    const std::wstring low = ToLower(cmd);
    const std::wstring sub = ExtractSubType(low);
    *subtypeOut = sub;
    // The probe's `hay = sub or cmd`: with a subtype present, only the subtype is
    // matched, so a URL or a profile path that happens to contain one of these words
    // cannot fabricate a verdict.
    const std::wstring& hay = sub.empty() ? low : sub;

    if (hay.find(L"media.mojom.mediafoundationservice") != std::wstring::npos)
        return CdmKind::kHardware;
    if (hay.find(L"media.mojom.cdm") != std::wstring::npos ||
        hay.find(L"media.mojom.widevine") != std::wstring::npos)
        return CdmKind::kLibrary;
    // This one is tested against the WHOLE command line on purpose: it is a file name
    // (widevinecdm.dll), not a subtype, and it appears alongside one.
    if (low.find(L"widevinecdm") != std::wstring::npos) return CdmKind::kLibrary;
    // Benign utilities that DO live under media.mojom. Exact subtypes only, each one seen on a
    // real machine: SpeechRecognitionService is Chrome's Live Caption / on-device speech, and
    // on the owner's Chrome it is resident - before this entry it held the gate at UNKNOWN and
    // generation never showed at all (2026-10-07).
    static const wchar_t* const kBenignMedia[] = {L"media.mojom.speechrecognitionservice"};
    for (const wchar_t* b : kBenignMedia)
        if (hay == b) return CdmKind::kNone;
    if (hay.find(L"media.mojom.") != std::wstring::npos) return CdmKind::kUnrecognisedMedia;
    return CdmKind::kNone;
}

// --------------------------------------------------------------- processes

struct ProcEntry {
    DWORD        pid = 0;
    DWORD        parent = 0;
    std::wstring name;  // szExeFile, e.g. L"chrome.exe", in whatever case Windows gave
};

bool TakeSnapshot(std::vector<ProcEntry>* out) {
    out->clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcEntry e;
            e.pid = pe.th32ProcessID;
            e.parent = pe.th32ParentProcessID;
            e.name = pe.szExeFile;
            out->push_back(std::move(e));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return !out->empty();
}

// The probe's PID-reuse guard: th32ParentProcessID is not reference counted, so an
// unrelated process that inherited the browser's old pid would appear to be its child.
// A child cannot predate its parent. 0 means we may not ask, and the caller then skips
// the guard rather than inventing an answer.
uint64_t ProcessCreatedAt(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return 0;
    FILETIME c{}, x{}, k{}, u{};
    uint64_t v = 0;
    if (GetProcessTimes(h, &c, &x, &k, &u))
        v = (static_cast<uint64_t>(c.dwHighDateTime) << 32) | c.dwLowDateTime;
    CloseHandle(h);
    return v;
}

// The snapshot an Impl starts life with. DrmStatus defaults to kClear — the struct has
// to, because `ok` is what says "not measuring" — so anything that publishes before a
// scan has completed publishes THIS instead. Contract 1: no verdict nobody earned.
DrmStatus UnknownStatus() {
    DrmStatus s;
    s.verdict = DrmVerdict::kUnknown;
    return s;
}

}  // namespace

const char* DrmSignalName(DrmSignal s) {
    switch (s) {
        case DrmSignal::kNone:        return "none";
        case DrmSignal::kHardwareCdm: return "hardware-cdm";
        case DrmSignal::kLibraryCdm:  return "library-cdm";
        case DrmSignal::kDeadPicture: return "dead-picture";
    }
    return "?";
}

// ------------------------------------------------------------------- Impl

struct DrmWatch::Impl {
    // ---- shared with the engine thread
    mutable std::mutex mu;
    DrmStatus status = UnknownStatus();
    std::atomic<bool> running{false};
    std::atomic<unsigned long> targetPid{0};  // DWORD

    // ---- the picture summary. Written only by the engine thread (OnPictureSummary and
    //      SetTargetPid, which the header requires to be the same thread), read by the
    //      scanner. Atomics rather than the mutex so the per-frame call cannot ever
    //      contend with a scan.
    std::atomic<double>  lastDarkFrac{0.0};
    std::atomic<int64_t> lastSummaryAt{0};  // QPC; 0 = nothing fed in yet
    std::atomic<int64_t> deadSince{0};      // QPC of the first consecutive dead summary

    // ---- owned by the thread that calls Start/Stop
    HANDLE thread = nullptr;
    HANDLE wake = nullptr;  // auto-reset: a target change wants an immediate scan
    HANDLE stop = nullptr;  // manual-reset

    // ---- scanner thread only, from here down
    PfnNtQueryInformationProcess ntq = nullptr;
    std::vector<ProcEntry> snap;
    DWORD      scannedPid = 0;            // the target the dwell state below belongs to
    int64_t    lastProtectedAt = 0;       // QPC of the last sighting of any signal
    DrmSignal  lastSignal = DrmSignal::kNone;
    DWORD      lastCdmPid = 0;
    bool       snapErrLogged = false;
    bool       targetGoneLogged = false;
    int        unreadableLogged = -1;
    std::wstring mediaWarned;
    uint64_t   scans = 0;

    static DWORD WINAPI ThreadMain(void* p);
    void Run();
    void Scan();
    bool PictureDead(int64_t now) const;
    void Publish(DrmVerdict v, DrmSignal s, DWORD cdmPid, int unreadable);
    void ResetPicture();
};

DWORD WINAPI DrmWatch::Impl::ThreadMain(void* p) {
    static_cast<Impl*>(p)->Run();
    return 0;
}

void DrmWatch::Impl::ResetPicture() {
    lastDarkFrac.store(0.0, std::memory_order_relaxed);
    lastSummaryAt.store(0, std::memory_order_relaxed);
    deadSince.store(0, std::memory_order_relaxed);
}

bool DrmWatch::Impl::PictureDead(int64_t now) const {
    const int64_t fed = lastSummaryAt.load(std::memory_order_relaxed);
    // Stale evidence is no evidence: see kPictureStaleSec.
    if (!fed || QpcToSec(now - fed) > kPictureStaleSec) return false;
    const int64_t since = deadSince.load(std::memory_order_relaxed);
    return since != 0 && QpcToSec(now - since) >= kDeadSec;
}

void DrmWatch::Impl::Publish(DrmVerdict v, DrmSignal s, DWORD cdmPid, int unreadable) {
    DrmStatus out;
    out.verdict = v;
    out.signal = s;
    out.cdmPid = cdmPid;
    out.unreadableChildren = unreadable;
    out.darkFrac = lastDarkFrac.load(std::memory_order_relaxed);
    out.scans = scans;
    out.ok = running.load(std::memory_order_relaxed);

    std::lock_guard<std::mutex> lock(mu);
    status = out;
}

void DrmWatch::Impl::Run() {
    for (;;) {
        // INFINITE while nothing is captured: contract 2's "idle otherwise". A target
        // change sets `wake`, so the first scan of a new target lands in milliseconds.
        const DWORD timeout =
            targetPid.load(std::memory_order_relaxed) != 0 ? kScanPeriodMs : INFINITE;
        HANDLE waits[2] = {stop, wake};  // stop is index 0, so it wins a simultaneous signal
        const DWORD w = WaitForMultipleObjects(2, waits, FALSE, timeout);
        if (w == WAIT_OBJECT_0) break;
        if (w == WAIT_FAILED) {
            LogErr("drm: WaitForMultipleObjects failed, GetLastError=%lu - the scanner is "
                   "stopping; every target is UNKNOWN from here, which the caller must treat "
                   "as protected",
                   GetLastError());
            // ok goes false FIRST, then the fail-closed verdict is published with it, so
            // a caller that reads Status() once more cannot see a stale clear one.
            running.store(false, std::memory_order_relaxed);
            Publish(DrmVerdict::kUnknown, DrmSignal::kNone, 0, 0);
            break;
        }
        Scan();
    }
}

void DrmWatch::Impl::Scan() {
    const int64_t now = QpcNow();
    const DWORD pid = static_cast<DWORD>(targetPid.load(std::memory_order_relaxed));

    // A target change resets the dwell: the previous browser's CDM says nothing about
    // this one, and holding its verdict over would be wrong in both directions.
    if (pid != scannedPid) {
        scannedPid = pid;
        lastProtectedAt = 0;
        lastSignal = DrmSignal::kNone;
        lastCdmPid = 0;
        snapErrLogged = false;
        targetGoneLogged = false;
        unreadableLogged = -1;
        mediaWarned.clear();
    }

    if (pid == 0) {
        // Nothing is being captured. This is NOT kClear: contract 1 says only a
        // completed scan of a real target may publish that, and an idle scanner has
        // scanned nothing. The caller is not generating here anyway.
        Publish(DrmVerdict::kUnknown, DrmSignal::kNone, 0, 0);
        return;
    }

    ++scans;

    if (!TakeSnapshot(&snap)) {
        if (!snapErrLogged) {
            snapErrLogged = true;
            LogErr("drm: CreateToolhelp32Snapshot failed, GetLastError=%lu - verdict UNKNOWN "
                   "(the caller must not generate)",
                   GetLastError());
        }
        Publish(DrmVerdict::kUnknown, DrmSignal::kNone, lastCdmPid, 0);
        return;
    }
    snapErrLogged = false;

    const ProcEntry* self = nullptr;
    for (const ProcEntry& e : snap) {
        if (e.pid == pid) {
            self = &e;
            break;
        }
    }
    if (!self) {
        // The target exited between the engine setting it and this scan, or it is one
        // the snapshot does not show us. Either way we cannot say anything about it.
        if (!targetGoneLogged) {
            targetGoneLogged = true;
            LogErr("drm: target pid %lu is not in the process snapshot - verdict UNKNOWN", pid);
        }
        Publish(DrmVerdict::kUnknown, DrmSignal::kNone, lastCdmPid, 0);
        return;
    }
    targetGoneLogged = false;
    const std::wstring targetName = self->name;
    const uint64_t targetCreated = ProcessCreatedAt(pid);

    // ---- signal (a): walk the direct children that are the browser's own processes.
    //
    // Same-image children only, which is the probe's rule (`chrome.exe` children of the
    // `chrome.exe` browser process) generalised to whatever browser we were given. It
    // also means a target that spawns no same-image children — PotPlayer, say — scans
    // clean in a few microseconds instead of being dragged through a tree it has not
    // got.
    int  unreadable = 0;
    int  children = 0;
    bool sawUnrecognisedMedia = false;
    CdmKind best = CdmKind::kNone;
    DWORD bestPid = 0;
    std::wstring bestSub;
    std::string firstWhy;

    for (const ProcEntry& e : snap) {
        if (e.parent != pid || e.pid == pid || e.pid == 0) continue;
        if (_wcsicmp(e.name.c_str(), targetName.c_str()) != 0) continue;
        if (++children > kMaxChildren) {
            // A scan that did not look at every child cannot call the tree clean.
            ++unreadable;
            if (firstWhy.empty()) firstWhy = "more children than kMaxChildren";
            break;
        }
        if (targetCreated) {
            const uint64_t created = ProcessCreatedAt(e.pid);
            if (created && created < targetCreated) {
                // PID reuse: this is not really our child. The probe counts it as an
                // error row, so it counts as unreadable here too — never as clean.
                ++unreadable;
                if (firstWhy.empty()) firstWhy = "pid reuse (older than the browser)";
                continue;
            }
        }

        const ChildRead cr = ReadChildCommandLine(e.pid, ntq);
        if (cr.gone) continue;  // exiting: it will not decrypt anything (measured)
        if (!cr.readable) {
            ++unreadable;
            if (firstWhy.empty()) firstWhy = cr.why;
            continue;
        }
        std::wstring sub;
        const CdmKind k = ClassifyCommandLine(cr.cmd, &sub);
        if (k == CdmKind::kUnrecognisedMedia) {
            sawUnrecognisedMedia = true;
            if (mediaWarned != sub) {
                mediaWarned = sub;
                // RISK, and a deliberate one. Chrome puts audio on audio.mojom.* and
                // capture on video_capture.mojom.*, so nothing benign is expected under
                // media.mojom.* — but that is `unverified` (drm_probe.py has not been
                // run), and if some benign utility does live there this line will print
                // once and the browser will never be generated over. That is contract 1
                // working as designed: it costs smoothness, not protection.
                LogErr("drm: unrecognised media.mojom utility subtype '%ls' on pid %lu - "
                       "verdict UNKNOWN until the matcher learns it",
                       sub.empty() ? L"?" : sub.c_str(), e.pid);
            }
            continue;
        }
        if (k == CdmKind::kHardware || (k == CdmKind::kLibrary && best != CdmKind::kHardware)) {
            best = k;
            bestPid = e.pid;
            bestSub = sub;
        }
    }

    if (unreadable > 0) {
        if (unreadableLogged != unreadable) {
            unreadableLogged = unreadable;
            LogErr("drm: %d of %d children of pid %lu could not be read (%s) - verdict UNKNOWN, "
                   "which the caller must treat as protected",
                   unreadable, children, pid,
                   firstWhy.empty() ? "no reason recorded" : firstWhy.c_str());
        }
    } else {
        unreadableLogged = -1;
    }

    // ---- compose. A positive signal beats "cannot tell", and hardware DRM beats the
    //      library CDM, because the two mean different things for the picture half.
    DrmSignal sigNow = DrmSignal::kNone;
    DWORD sigPid = 0;
    if (best == CdmKind::kHardware) {
        sigNow = DrmSignal::kHardwareCdm;
        sigPid = bestPid;
    } else if (best == CdmKind::kLibrary) {
        sigNow = DrmSignal::kLibraryCdm;
        sigPid = bestPid;
    } else if (PictureDead(now)) {
        sigNow = DrmSignal::kDeadPicture;
    }

    if (sigNow != DrmSignal::kNone) {
        if (lastSignal != sigNow || lastCdmPid != sigPid)
            Log("drm: PROTECTED - %s, subtype '%ls', pid %lu - generation stays off",
                DrmSignalName(sigNow), bestSub.empty() ? L"-" : bestSub.c_str(), sigPid);
        lastProtectedAt = now;
        lastSignal = sigNow;
        lastCdmPid = sigPid;
        Publish(DrmVerdict::kProtected, sigNow, sigPid, unreadable);
        return;
    }

    // Contract 3: hold the last protected verdict for the dwell before letting go.
    if (lastProtectedAt != 0 && QpcToSec(now - lastProtectedAt) < kCdmGoneDwellSec) {
        Publish(DrmVerdict::kProtected, lastSignal, lastCdmPid, unreadable);
        return;
    }

    if (unreadable > 0 || sawUnrecognisedMedia) {
        Publish(DrmVerdict::kUnknown, DrmSignal::kNone, 0, unreadable);
        return;
    }

    if (lastProtectedAt != 0) {
        Log("drm: clear - no CDM for %.1f s (%s was the last signal); generation may resume",
            QpcToSec(now - lastProtectedAt), DrmSignalName(lastSignal));
        lastProtectedAt = 0;
        lastSignal = DrmSignal::kNone;
        lastCdmPid = 0;
    }
    Publish(DrmVerdict::kClear, DrmSignal::kNone, 0, 0);
}

// --------------------------------------------------------------- DrmWatch

DrmWatch::DrmWatch() = default;

DrmWatch::~DrmWatch() { Stop(); }

bool DrmWatch::Start(std::string* err) {
    if (!impl_) impl_ = std::make_unique<Impl>();
    Impl& d = *impl_;
    if (d.thread) return d.running.load(std::memory_order_relaxed);

    d.ntq = ResolveNtQueryInformationProcess();
    if (!d.ntq) {
        // Without it no command line can be read, so every scan would be kUnknown and
        // the browser path would never generate. Say so loudly instead of limping.
        if (err)
            *err = "GetProcAddress(ntdll!NtQueryInformationProcess) failed, GetLastError=" +
                   std::to_string(GetLastError());
        return false;
    }

    d.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);   // manual-reset
    d.wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);  // auto-reset
    if (!d.stop || !d.wake) {
        if (err) *err = "CreateEvent failed, GetLastError=" + std::to_string(GetLastError());
        if (d.stop) { CloseHandle(d.stop); d.stop = nullptr; }
        if (d.wake) { CloseHandle(d.wake); d.wake = nullptr; }
        return false;
    }

    // ok before the thread exists, and an UNKNOWN verdict with it: a caller that reads
    // Status() between here and the first scan must not see the struct's default, which
    // is kClear (contract 1).
    d.running.store(true, std::memory_order_relaxed);
    d.Publish(DrmVerdict::kUnknown, DrmSignal::kNone, 0, 0);

    d.thread = CreateThread(nullptr, 0, &Impl::ThreadMain, &d, 0, nullptr);
    if (!d.thread) {
        if (err) *err = "CreateThread failed, GetLastError=" + std::to_string(GetLastError());
        d.running.store(false, std::memory_order_relaxed);
        d.Publish(DrmVerdict::kUnknown, DrmSignal::kNone, 0, 0);
        CloseHandle(d.stop);
        d.stop = nullptr;
        CloseHandle(d.wake);
        d.wake = nullptr;
        return false;
    }
    return true;
}

void DrmWatch::Stop() {
    if (!impl_) return;
    Impl& d = *impl_;
    if (d.thread) {
        SetEvent(d.stop);
        if (WaitForSingleObject(d.thread, 5000) != WAIT_OBJECT_0) {
            // The scanner still points into Impl, so Impl must outlive it: leak it
            // rather than free memory a running thread reads. Same call the game gate
            // and the tray make.
            LogErr("drm: the scanner thread did not stop within 5 s - leaving it behind");
            impl_.release();
            return;
        }
        CloseHandle(d.thread);
        d.thread = nullptr;
    }
    d.running.store(false, std::memory_order_relaxed);
    // ok == false is the fail-closed state, and it has to be visible to a caller that
    // reads Status() after Stop().
    d.Publish(DrmVerdict::kUnknown, DrmSignal::kNone, 0, 0);
    if (d.wake) { CloseHandle(d.wake); d.wake = nullptr; }
    if (d.stop) { CloseHandle(d.stop); d.stop = nullptr; }
}

void DrmWatch::SetTargetPid(DWORD pid) {
    if (!impl_) return;
    Impl& d = *impl_;
    const DWORD prev = static_cast<DWORD>(
        d.targetPid.exchange(static_cast<unsigned long>(pid), std::memory_order_relaxed));
    if (prev == pid) return;

    // The published snapshot still describes the PREVIOUS target, and the engine may
    // read it before the scanner wakes. Publish UNKNOWN for the new one now — kClear is
    // only ever earned by a completed scan of the target it names (contract 1) — drop
    // the old picture evidence, and wake the scanner so the real answer follows in
    // milliseconds rather than at the next 1 Hz tick.
    d.ResetPicture();
    {
        std::lock_guard<std::mutex> lock(d.mu);
        d.status.verdict = DrmVerdict::kUnknown;
        d.status.signal = DrmSignal::kNone;
        d.status.cdmPid = 0;
        d.status.unreadableChildren = 0;
        d.status.darkFrac = 0.0;
    }
    if (d.wake) SetEvent(d.wake);
}

void DrmWatch::OnPictureSummary(double darkFrac) {
    if (!impl_) return;
    Impl& d = *impl_;
    // A NaN would compare false against everything below and silently disarm the
    // signal; clamp it to "not dark", which is the honest reading of a summary we
    // cannot interpret. Note the test is >= : the engine may legitimately report a
    // perfectly dead 1.0.
    double v = darkFrac;
    if (!(v >= 0.0)) v = 0.0;
    if (v > 1.0) v = 1.0;

    const int64_t now = QpcNow();
    d.lastDarkFrac.store(v, std::memory_order_relaxed);
    d.lastSummaryAt.store(now, std::memory_order_relaxed);
    if (v >= kDeadDarkFrac) {
        // Read-modify-write without a lock: the header requires this and SetTargetPid
        // to come from the same (engine) thread, and the scanner only ever reads.
        if (d.deadSince.load(std::memory_order_relaxed) == 0)
            d.deadSince.store(now, std::memory_order_relaxed);
    } else {
        // One live frame ends the run. The dwell has to be CONTINUOUS, or a video that
        // is dark half the time would accumulate its way to kDeadSec.
        d.deadSince.store(0, std::memory_order_relaxed);
    }
}

DrmStatus DrmWatch::Status() const {
    if (!impl_) {
        // Never started, or Stop() leaked a hung scanner. ok == false already says
        // "fail closed", but the verdict is made explicit rather than left on the
        // struct's kClear default.
        DrmStatus s;
        s.verdict = DrmVerdict::kUnknown;
        s.ok = false;
        return s;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    return impl_->status;
}

}  // namespace nsp
