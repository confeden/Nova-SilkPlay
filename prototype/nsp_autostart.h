// nsp_autostart.h — "start with Windows", as an HKCU Run value.
//
// NOT part of Settings and NOT in settings.json, on purpose. A copy in the JSON
// would be a second source of truth that disagrees with reality the moment the
// user disables the entry in Task Manager, reinstalls the program somewhere
// else, or copies settings.json to another machine. There is one place to read
// and one place to write, and the Settings checkbox re-reads the registry every
// time it is shown and again after every write.
#pragma once

#include <string>

namespace nsp {

enum class AutostartState {
    kOff,      // no Run value
    kOn,       // Run value present and not disabled
    kBlocked   // Run value present, but Task Manager > Startup apps has it disabled
};

// HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value "Nova SilkPlay".
AutostartState AutostartQuery();

// Writes the value as the quoted full path of this executable, or deletes it.
// Never touches StartupApproved: that key belongs to the user and to Task
// Manager, and writing it is what a program fighting its user would do.
bool AutostartSet(bool on, std::string* err);

// If the value exists but names a different path than this executable — after a
// reinstall into a new folder, or a move — rewrite it. NEVER creates a value
// that is not already there: a program that adds itself to autostart because it
// was run once is malware behaviour.
bool AutostartRepair();

}  // namespace nsp
