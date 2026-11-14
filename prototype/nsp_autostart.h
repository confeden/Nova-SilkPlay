// nsp_autostart.h — current-user Startup shortcut; never duplicated in settings.json.
#pragma once
#include <string>
namespace nsp {
enum class AutostartState {
    kOff,
    kOn,
    kBlocked  // Task Manager disabled it; StartupApproved is never written by us
};
AutostartState AutostartQuery();
// Creates FOLDERID_Startup\Nova SilkPlay.lnk targeting this executable, or removes it.
// Removes our legacy HKCU Run value too, preventing duplicate launches.
bool AutostartSet(bool on, std::string* err);
// Migrates an enabled legacy Run value; repairs only missing shortcut targets.
// Never adds autostart without an existing registration or steals a working target.
bool AutostartRepair();
}  // namespace nsp
