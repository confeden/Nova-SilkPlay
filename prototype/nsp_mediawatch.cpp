#include "nsp_mediawatch.h"

#include "nsp_common.h"

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <algorithm>
#include <cwctype>
#include <thread>

namespace nsp {

namespace wmc = winrt::Windows::Media::Control;

namespace {
bool HasNoCase(std::wstring hay, std::wstring needle) {
    const auto lower = [](std::wstring* s) {
        std::transform(s->begin(), s->end(), s->begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    };
    lower(&hay);
    lower(&needle);
    return hay.find(needle) != std::wstring::npos;
}
}  // namespace

const char* MediaStateName(MediaState s) {
    switch (s) {
        case MediaState::kIdle: return "idle";
        case MediaState::kPlaying: return "playing";
        default: return "unknown";
    }
}

struct MediaWatch::Impl {
    std::wstring appIdSub;
    std::atomic<int> state{static_cast<int>(MediaState::kUnknown)};
    HANDLE stop = nullptr;
    std::thread thread;

    void Run() {
        // Its own thread and its own (MTA) apartment: the session manager's calls cross into
        // another process, and the engine thread paces 165 frames a second.
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        wmc::GlobalSystemMediaTransportControlsSessionManager mgr{nullptr};
        std::string lastIds;
        bool loggedFail = false;
        do {
            MediaState now = MediaState::kUnknown;
            try {
                if (!mgr) mgr = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
                now = MediaState::kIdle;
                std::string ids;
                for (auto const& session : mgr.GetSessions()) {
                    const std::wstring id(session.SourceAppUserModelId().c_str());
                    ids += Narrow(id) + " ";
                    if (!HasNoCase(id, appIdSub)) continue;
                    const auto st = session.GetPlaybackInfo().PlaybackStatus();
                    if (st == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing)
                        now = MediaState::kPlaying;
                }
                if (ids != lastIds) {
                    lastIds = ids;
                    Log("media sessions: %s", ids.empty() ? "(none)" : ids.c_str());
                }
                loggedFail = false;
            } catch (winrt::hresult_error const& e) {
                if (!loggedFail)
                    LogErr("media session manager unavailable: %s - the windowed path scouts "
                           "without it", HrString(e.code()).c_str());
                loggedFail = true;
                mgr = nullptr;
                now = MediaState::kUnknown;
            }
            state.store(static_cast<int>(now), std::memory_order_release);
        } while (WaitForSingleObject(stop, 500) == WAIT_TIMEOUT);
        mgr = nullptr;
        winrt::uninit_apartment();
    }
};

MediaWatch::MediaWatch() : impl_(std::make_unique<Impl>()) {}
MediaWatch::~MediaWatch() { Stop(); }

void MediaWatch::Start(const std::wstring& appIdSub) {
    Stop();
    impl_->appIdSub = appIdSub;
    impl_->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->stop) return;
    impl_->thread = std::thread([this] { impl_->Run(); });
}

void MediaWatch::Stop() {
    if (impl_->thread.joinable()) {
        SetEvent(impl_->stop);
        impl_->thread.join();
    }
    if (impl_->stop) {
        CloseHandle(impl_->stop);
        impl_->stop = nullptr;
    }
    impl_->state.store(static_cast<int>(MediaState::kUnknown), std::memory_order_release);
}

MediaState MediaWatch::State() const {
    return static_cast<MediaState>(impl_->state.load(std::memory_order_acquire));
}

}  // namespace nsp
