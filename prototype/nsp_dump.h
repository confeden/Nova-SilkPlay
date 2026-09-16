// nsp_dump.h — write a GPU texture to a PPM file.
//
// Purely a development aid, and the only way to judge synthesis quality without
// standing in front of the screen: dump the two source frames and the frame the
// synthesizer puts between them, then look at whether the moving edge is one
// sharp edge (motion compensation worked) or two faint ones (it did not).
//
// PPM because it needs no library: P6, 8-bit RGB, one header line.
// tools: `python ppm2png.py out_*.ppm` turns them into something viewable.
#pragma once

#include "nsp_common.h"

#include <d3d11_1.h>

namespace nsp {

// Copies `tex` to a staging texture, maps it (blocking — this is a one-shot
// debug path, not the hot loop) and writes RGB. Returns false and logs on error.
bool DumpTextureToPpm(ID3D11Device* device, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                      const char* path);

}  // namespace nsp
