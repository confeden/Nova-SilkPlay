// uia_video.cpp - can UI Automation tell us where a <video> sits in a WINDOWED Chrome,
// how fast, and does the rect follow scrolling? (P11, the windowed video rect)
//
//   uia_video.exe --title YouTube [--dump] [--poll N] [--every-ms M]
//
// --dump   one line per element under the window whose bounding box is at least
//          64x64: depth, control type, localized type, class, aria role, automation id,
//          name, rect. Used once to learn which property marks a <video>.
// --poll   N reads of the video rect, M ms apart, timing each, in physical screen px.
//
// Prints rects in PHYSICAL screen pixels (the process is per-monitor DPI aware v2,
// which is what UIA's BoundingRectangle follows for an aware client).
#include <windows.h>
#include <uiautomation.h>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "user32.lib")

static double Ms(LARGE_INTEGER a, LARGE_INTEGER b) {
    static LARGE_INTEGER f{};
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    return 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart);
}

static std::string Narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

static std::string BstrProp(IUIAutomationElement* e, PROPERTYID id) {
    VARIANT v; VariantInit(&v);
    std::string s;
    if (SUCCEEDED(e->GetCurrentPropertyValue(id, &v)) && v.vt == VT_BSTR) s = Narrow(v.bstrVal);
    VariantClear(&v);
    if (s.size() > 60) s = s.substr(0, 60) + "...";
    return s;
}

struct FindCtx { std::wstring title; HWND hwnd = nullptr; };
static BOOL CALLBACK EnumTop(HWND h, LPARAM lp) {
    auto* c = reinterpret_cast<FindCtx*>(lp);
    if (!IsWindowVisible(h)) return TRUE;
    wchar_t cls[64]{}, t[512]{};
    GetClassNameW(h, cls, 64);
    if (wcscmp(cls, L"Chrome_WidgetWin_1") != 0) return TRUE;
    GetWindowTextW(h, t, 512);
    if (wcsstr(t, c->title.c_str())) { c->hwnd = h; return FALSE; }
    return TRUE;
}

static void Dump(IUIAutomation* uia, IUIAutomationElement* e, int depth, int* count) {
    if (depth > 40 || *count > 4000) return;
    ++*count;
    RECT r{};
    e->get_CurrentBoundingRectangle(&r);
    if (r.right - r.left >= 64 && r.bottom - r.top >= 64) {
        CONTROLTYPEID ct = 0;
        e->get_CurrentControlType(&ct);
        printf("%2d ct=%d loc='%s' cls='%s' role='%s' id='%s' name='%s' [%ld,%ld %ldx%ld]\n", depth, ct,
               BstrProp(e, UIA_LocalizedControlTypePropertyId).c_str(),
               BstrProp(e, UIA_ClassNamePropertyId).c_str(),
               BstrProp(e, UIA_AriaRolePropertyId).c_str(),
               BstrProp(e, UIA_AutomationIdPropertyId).c_str(),
               BstrProp(e, UIA_NamePropertyId).c_str(), r.left, r.top, r.right - r.left, r.bottom - r.top);
    }
    IUIAutomationTreeWalker* w = nullptr;
    uia->get_RawViewWalker(&w);
    IUIAutomationElement* c = nullptr;
    w->GetFirstChildElement(e, &c);
    while (c) {
        Dump(uia, c, depth + 1, count);
        IUIAutomationElement* n = nullptr;
        w->GetNextSiblingElement(c, &n);
        c->Release();
        c = n;
    }
    w->Release();
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    FindCtx fc; fc.title = L"YouTube";
    bool dump = false;
    int poll = 0, everyMs = 250;
    std::wstring roleWanted = L"";  // empty: localized type "video"
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--title" && i + 1 < argc) fc.title = argv[++i];
        else if (a == L"--dump") dump = true;
        else if (a == L"--poll" && i + 1 < argc) poll = _wtoi(argv[++i]);
        else if (a == L"--every-ms" && i + 1 < argc) everyMs = _wtoi(argv[++i]);
    }
    EnumWindows(EnumTop, reinterpret_cast<LPARAM>(&fc));
    if (!fc.hwnd) { printf("no Chrome window with that title\n"); return 2; }
    RECT wr{}; GetWindowRect(fc.hwnd, &wr);
    printf("hwnd 0x%p window [%ld,%ld %ldx%ld]\n", static_cast<void*>(fc.hwnd), wr.left, wr.top,
           wr.right - wr.left, wr.bottom - wr.top);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&uia)))) {
        printf("CUIAutomation8 unavailable\n");
        return 3;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    IUIAutomationElement* root = nullptr;
    HRESULT hr = uia->ElementFromHandle(fc.hwnd, &root);
    QueryPerformanceCounter(&t1);
    printf("ElementFromHandle: hr=0x%08lx %.1f ms\n", static_cast<unsigned long>(hr), Ms(t0, t1));
    if (!root) return 4;

    if (dump) {
        int count = 0;
        QueryPerformanceCounter(&t0);
        Dump(uia, root, 0, &count);
        QueryPerformanceCounter(&t1);
        printf("dump: %d elements visited, %.1f ms\n", count, Ms(t0, t1));
    }

    VARIANT v; VariantInit(&v);
    v.vt = VT_BSTR; v.bstrVal = SysAllocString(L"video");
    IUIAutomationCondition* cond = nullptr;
    uia->CreatePropertyCondition(UIA_LocalizedControlTypePropertyId, v, &cond);
    VariantClear(&v);

    IUIAutomationCacheRequest* cr = nullptr;
    uia->CreateCacheRequest(&cr);
    cr->AddProperty(UIA_BoundingRectanglePropertyId);

    IUIAutomationElement* video = nullptr;
    for (int i = 0; i < (poll > 0 ? poll : 1); ++i) {
        QueryPerformanceCounter(&t0);
        if (!video) {
            root->FindFirstBuildCache(TreeScope_Descendants, cond, cr, &video);
            QueryPerformanceCounter(&t1);
            if (video) {
                RECT r{}; video->get_CachedBoundingRectangle(&r);
                printf("%4d find  %.2f ms [%ld,%ld %ldx%ld]\n", i, Ms(t0, t1), r.left, r.top,
                       r.right - r.left, r.bottom - r.top);
            } else {
                printf("%4d find  %.2f ms: no video element\n", i, Ms(t0, t1));
            }
        } else {
            IUIAutomationElement* upd = nullptr;
            hr = video->BuildUpdatedCache(cr, &upd);
            QueryPerformanceCounter(&t1);
            if (SUCCEEDED(hr) && upd) {
                RECT r{}; upd->get_CachedBoundingRectangle(&r);
                printf("%4d read  %.2f ms [%ld,%ld %ldx%ld]\n", i, Ms(t0, t1), r.left, r.top,
                       r.right - r.left, r.bottom - r.top);
                upd->Release();
            } else {
                printf("%4d read  %.2f ms: hr=0x%08lx (element gone?)\n", i, Ms(t0, t1),
                       static_cast<unsigned long>(hr));
                video->Release();
                video = nullptr;
            }
        }
        fflush(stdout);
        if (poll > 1) Sleep(everyMs);
    }
    return 0;
}
