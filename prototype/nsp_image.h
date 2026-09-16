// nsp_image.h — PNG in and out, and a hash over raw pixels.
//
// The quality harness feeds the engine frames from disk and scores what comes
// back, so the byte path in and out has to be exact and boring: 8-bit BGRA, no
// colour management, no gamma chunk, no surprises. WIC is in-box, so this adds
// no dependency.
//
// The hash exists because determinism is a GATE, not a hope: two runs of the
// same build on the same frames must produce identical output pixels, and the
// cheapest way to assert that is to compare digests rather than images.
#pragma once

#include "nsp_common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace nsp {

// Decodes any WIC-readable image to straight 32bpp BGRA, top-down, tightly
// packed (stride = width * 4). Alpha is whatever the file carried; the engine
// ignores it (its overlay is opaque, DXGI_ALPHA_MODE_IGNORE).
bool LoadImageBgra(const std::string& path, std::vector<uint8_t>* pixels, UINT* w, UINT* h,
                   std::string* err);

// Writes 32bpp BGRA as PNG. Deliberately writes NO gAMA, sRGB or ICC chunk: a
// decoder that re-transforms the values would silently bias every metric
// computed from the file, and the harness treats the bytes as final.
bool SaveImageBgraPng(const std::string& path, const uint8_t* pixels, UINT w, UINT h, UINT stride,
                      std::string* err);

// Lowercase hex SHA-256, via CNG (bcrypt). Empty string on failure.
std::string Sha256Hex(const void* data, size_t bytes);

}  // namespace nsp
