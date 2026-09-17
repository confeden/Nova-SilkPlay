// nsp_version.h — the app's version, shared by the resource script and the code.
//
// The app and the engine are versioned apart (I5). The app's first release is 0.9, the beta
// (D30); until that build is cut every build is a development build of it, which is what the
// "-dev" suffix says. Plain #defines, because rc.exe reads this file too.
#pragma once

#define NSP_VERSION_MAJOR 0
#define NSP_VERSION_MINOR 9
#define NSP_VERSION_PATCH 0
#define NSP_VERSION_BUILD 0

#define NSP_VERSION_TEXT "0.9.0-dev"
#define NSP_VERSION_WTEXT L"0.9.0-dev"
