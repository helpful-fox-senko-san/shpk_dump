#pragma once

#include <array>
#include <cstdint>

struct ShpkPassName {
    std::uint32_t crc;
    const char* name;
};

inline constexpr auto kShpkPassNames = std::to_array<ShpkPassName>({
    { 0xC5A5389Cu, "PASS_0" },
    { 0xE412A2D4u, "PASS_Z_OPAQUE" },
    { 0x03AC862Eu, "PASS_G_OPAQUE" },
    { 0xFBDE0A8Fu, "PASS_LIGHTING_OPAQUE" },
    { 0x6006067Fu, "PASS_G_SEMITRANSPARENCY" },
    { 0x1F197698u, "PASS_LIGHTING_SEMITRANSPARENCY" },
    { 0x955C0B73u, "PASS_COMPOSITE_OPAQUE" },
    { 0x5BC1AD3Fu, "PASS_7" },
    { 0x8EF40D56u, "PASS_WATER" },
    { 0x24CDF1EAu, "PASS_WATER_Z" },
    { 0x2D0C1A37u, "PASS_SEMITRANSPARENCY" },
    { 0xC885BBD3u, "PASS_COMPOSITE_SEMITRANSPARENCY" },
    { 0xC6BE7BBAu, "PASS_10" },
    { 0x28B01A96u, "PASS_12" },
    { 0xC1D3BFA3u, "PASS_14" },
});
