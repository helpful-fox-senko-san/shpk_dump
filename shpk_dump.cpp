// shpk_dump
//
// Usage:
//   ./shpk_dump skin.shpk
//     -> skin.txt        (readable description of all metadata)
//     -> skin/           (one .dxbc file per shader blob)
//
//   ./shpk_dump -o /tmp/skin_dump skin.shpk
//   ./shpk_dump *.shpk
//   ./shpk_dump                 (dumps every .shpk in the current directory)
//
// The format is a little-endian container.  The actual shader blobs are
// DirectX bytecode: DXBC for DX11 packages, CSO for DX9 packages.  For vertex
// shaders the container stores a small header in front of the DXBC data; that
// header is shown in the text dump and is not included in the .dxbc output.

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <print>
#include <regex>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "shpk_crc_keys.hpp"
#include "shpk_material_names.hpp"
#include "shpk_pass_names.hpp"

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Small bounds-checked little-endian reader
// ---------------------------------------------------------------------------

struct Reader {
    std::span<const uint8_t> data;
    size_t pos = 0;
    bool ok = true;

    bool need(size_t n) {
        if (!ok) return false;
        if (n > data.size() || pos > data.size() - n) {
            ok = false;
            return false;
        }
        return true;
    }

    uint8_t u8() {
        if (!need(1)) return 0;
        return data[pos++];
    }

    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = static_cast<uint16_t>(data[pos]) |
                     static_cast<uint16_t>(data[pos + 1] << 8);
        pos += 2;
        return v;
    }

    int16_t i16() { return static_cast<int16_t>(u16()); }

    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = static_cast<uint32_t>(data[pos]) |
                     (static_cast<uint32_t>(data[pos + 1]) << 8) |
                     (static_cast<uint32_t>(data[pos + 2]) << 16) |
                     (static_cast<uint32_t>(data[pos + 3]) << 24);
        pos += 4;
        return v;
    }

    int32_t i32() { return static_cast<int32_t>(u32()); }

    std::span<const uint8_t> bytes(size_t n) {
        if (!need(n)) return {};
        auto result = data.subspan(pos, n);
        pos += n;
        return result;
    }

    void skip(size_t n) {
        if (need(n)) pos += n;
    }
};

// ---------------------------------------------------------------------------
// Data model
// ---------------------------------------------------------------------------

enum class Stage { Vertex, Pixel, Hull, Domain, Geometry };

static const char* stage_name(Stage s) {
    switch (s) {
        case Stage::Vertex:   return "Vertex";
        case Stage::Pixel:    return "Pixel";
        case Stage::Hull:     return "Hull";
        case Stage::Domain:   return "Domain";
        case Stage::Geometry: return "Geometry";
    }
    return "?";
}

static const char* stage_prefix(Stage s) {
    switch (s) {
        case Stage::Vertex:   return "vs";
        case Stage::Pixel:    return "ps";
        case Stage::Hull:     return "hs";
        case Stage::Domain:   return "ds";
        case Stage::Geometry: return "gs";
    }
    return "xx";
}

struct Resource {
    uint32_t id = 0;
    uint32_t str_offset = 0;
    uint16_t str_size = 0;
    uint16_t is_texture = 0;
    uint16_t slot = 0;
    uint16_t size = 0;
    std::string name;
};

struct Shader {
    Stage stage = Stage::Vertex;
    uint32_t blob_offset = 0;
    uint32_t blob_size = 0;
    uint32_t unk = 0;
    std::vector<Resource> constants;
    std::vector<Resource> samplers;
    std::vector<Resource> uavs;
    std::vector<Resource> textures;
};

struct MaterialParam {
    uint32_t id = 0;
    uint16_t offset = 0;
    uint16_t size = 0;
};

struct Key {
    uint32_t id = 0;
    uint32_t default_value = 0;
};

struct Pass {
    uint32_t id = 0;
    uint32_t vs = 0;
    uint32_t ps = 0;
    uint32_t hs = 0xFFFFFFFFu;
    uint32_t ds = 0xFFFFFFFFu;
    uint32_t gs = 0xFFFFFFFFu;
};

struct Node {
    uint32_t selector = 0;
    uint32_t pass_count = 0;
    std::array<uint8_t, 16> pass_indexes{};
    std::array<uint32_t, 2> unk131{};
    std::vector<uint32_t> system_keys;
    std::vector<uint32_t> scene_keys;
    std::vector<uint32_t> material_keys;
    std::vector<uint32_t> subview_keys;
    std::vector<Pass> passes;
};

struct Alias {
    uint32_t selector = 0;
    uint32_t node = 0;
};

struct SubCluster {
    uint16_t own_index = 0;
    uint16_t alias_count = 0;
    std::array<uint32_t, 97> data{};   // aliases (pairs) followed by extra data
};

struct Cluster {
    uint32_t subview2 = 0;
    uint32_t subview1 = 0;
    uint32_t unk = 0;
    std::vector<SubCluster> subclusters;
};

struct Shpk {
    std::span<const uint8_t> file_data;
    uint32_t version = 0;
    uint32_t dx_magic = 0;
    uint32_t file_length = 0;
    uint32_t blobs_offset = 0;
    uint32_t strings_offset = 0;
    uint32_t material_params_size = 0;
    uint16_t num_material_params = 0;
    bool has_defaults = false;
    uint32_t num_constants = 0;
    uint16_t num_samplers = 0;
    uint16_t num_textures = 0;
    uint32_t num_uavs = 0;
    uint32_t num_system_keys = 0;
    uint32_t num_scene_keys = 0;
    uint32_t num_material_keys = 0;
    uint32_t num_nodes = 0;
    uint32_t num_aliases = 0;
    uint32_t num_hull = 0;
    uint32_t num_domain = 0;
    uint32_t num_geometry = 0;
    uint32_t num_clusters = 0;

    std::vector<Shader> vertex_shaders;
    std::vector<Shader> pixel_shaders;
    std::vector<Shader> hull_shaders;
    std::vector<Shader> domain_shaders;
    std::vector<Shader> geometry_shaders;

    std::vector<MaterialParam> material_params;
    std::vector<uint8_t> material_defaults;
    std::vector<Resource> constants;
    std::vector<Resource> samplers;
    std::vector<Resource> textures;
    std::vector<Resource> uavs;

    std::vector<Key> system_keys;
    std::vector<Key> scene_keys;
    std::vector<Key> material_keys;
    std::array<uint32_t, 2> subview_keys{};
    std::vector<Node> nodes;
    std::vector<Alias> aliases;
    std::vector<Cluster> clusters;
    std::vector<uint8_t> additional_data;
};

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

static constexpr uint32_t ShPkMagic = 0x6B506853u;
static constexpr uint32_t Dx9Magic  = 0x00395844u;
static constexpr uint32_t Dx11Magic = 0x31315844u;
static constexpr uint32_t MaterialParamsConstantId = 0x64D12851u;

static bool is_dx11(const Shpk& s) { return s.dx_magic == Dx11Magic; }

static std::span<const uint8_t> safe_slice(std::span<const uint8_t> data,
                                           uint64_t offset, uint64_t size) {
    if (offset > data.size() || size > data.size() - offset) return {};
    return data.subspan(static_cast<size_t>(offset), static_cast<size_t>(size));
}

static std::string read_name(const Shpk& s, uint32_t str_offset, uint16_t str_size) {
    auto raw = safe_slice(s.file_data, static_cast<uint64_t>(s.strings_offset) + str_offset,
                          str_size);
    if (raw.empty()) return str_size == 0 ? std::string{} : "<invalid string offset>";
    std::string name(reinterpret_cast<const char*>(raw.data()), raw.size());
    while (!name.empty() && name.back() == '\0') name.pop_back();
    return name;
}

static Resource read_resource(Reader& r, const Shpk& s) {
    Resource res;
    res.id          = r.u32();
    res.str_offset  = r.u32();
    res.str_size    = r.u16();
    res.is_texture  = r.u16();
    res.slot        = r.u16();
    res.size        = r.u16();
    res.name        = read_name(s, res.str_offset, res.str_size);
    return res;
}

static std::vector<Resource> read_resources(Reader& r, const Shpk& s, uint32_t count) {
    std::vector<Resource> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count && r.ok; ++i) result.push_back(read_resource(r, s));
    return result;
}

static std::vector<Shader> read_shader_array(Reader& r, const Shpk& s, uint32_t count,
                                             Stage stage) {
    std::vector<Shader> result;
    result.reserve(count);

    for (uint32_t i = 0; i < count && r.ok; ++i) {
        Shader sh;
        sh.stage       = stage;
        sh.blob_offset = r.u32();
        sh.blob_size   = r.u32();

        uint16_t num_constants = r.u16();
        uint16_t num_samplers  = r.u16();
        uint16_t num_uavs      = r.u16();
        uint16_t num_textures  = r.u16();

        if (s.version >= 0x0D01) {
            sh.unk = r.u32();
        } else {
            // Old files did not store this field; these are the observed defaults.
            sh.unk = stage == Stage::Vertex   ? 1u
                   : stage == Stage::Pixel    ? 4u
                   : stage == Stage::Geometry ? 8u
                                              : 0u;
        }

        sh.constants = read_resources(r, s, num_constants);
        sh.samplers  = read_resources(r, s, num_samplers);
        sh.uavs      = read_resources(r, s, num_uavs);
        sh.textures  = read_resources(r, s, num_textures);

        result.push_back(std::move(sh));
    }

    return result;
}

static std::vector<Key> read_keys(Reader& r, uint32_t count) {
    std::vector<Key> result;
    result.reserve(count);
    for (uint32_t i = 0; i < count && r.ok; ++i) {
        Key k;
        k.id            = r.u32();
        k.default_value = r.u32();
        result.push_back(k);
    }
    return result;
}

static bool parse_shpk(std::span<const uint8_t> data, Shpk& s, std::string& error) {
    Reader r{data};
    s.file_data = data;

    uint32_t magic = r.u32();
    s.version      = r.u32();
    s.dx_magic     = r.u32();
    s.file_length  = r.u32();
    s.blobs_offset = r.u32();
    s.strings_offset = r.u32();

    uint32_t num_vertex = r.u32();
    uint32_t num_pixel  = r.u32();

    s.material_params_size = r.u32();
    s.num_material_params  = r.u16();
    s.has_defaults         = r.u16() != 0;
    s.num_constants        = r.u32();
    s.num_samplers         = r.u16();
    s.num_textures         = r.u16();
    s.num_uavs             = r.u32();

    s.num_system_keys   = r.u32();
    s.num_scene_keys    = r.u32();
    s.num_material_keys = r.u32();
    s.num_nodes         = r.u32();
    s.num_aliases       = r.u32();

    if (s.version >= 0x0D01) {
        s.num_hull     = r.u32();
        s.num_domain   = r.u32();
        s.num_geometry = r.u32();
    }
    if (s.version >= 0x0E01) s.num_clusters = r.u32();

    if (!r.ok) {
        error = "file is too short for the ShPk header";
        return false;
    }
    if (magic != ShPkMagic) {
        error = std::format("bad magic 0x{:08X} (expected 0x{:08X})", magic, ShPkMagic);
        return false;
    }
    if (s.dx_magic != Dx9Magic && s.dx_magic != Dx11Magic) {
        error = std::format("unknown DirectX magic 0x{:08X}", s.dx_magic);
        return false;
    }
    if (s.file_length != data.size()) {
        error = std::format("header says file length is {} but file is {} bytes",
                            s.file_length, data.size());
        return false;
    }
    if (s.blobs_offset > data.size() || s.strings_offset > data.size()
        || s.blobs_offset > s.strings_offset) {
        error = "invalid blob/string section offsets";
        return false;
    }

    s.vertex_shaders   = read_shader_array(r, s, num_vertex, Stage::Vertex);
    s.pixel_shaders    = read_shader_array(r, s, num_pixel,  Stage::Pixel);
    s.hull_shaders     = read_shader_array(r, s, s.num_hull, Stage::Hull);
    s.domain_shaders   = read_shader_array(r, s, s.num_domain, Stage::Domain);
    s.geometry_shaders = read_shader_array(r, s, s.num_geometry, Stage::Geometry);

    s.material_params.reserve(s.num_material_params);
    for (uint16_t i = 0; i < s.num_material_params && r.ok; ++i) {
        MaterialParam p;
        p.id     = r.u32();
        p.offset = r.u16();
        p.size   = r.u16();
        s.material_params.push_back(p);
    }

    if (s.has_defaults) {
        auto defaults = r.bytes(s.material_params_size);
        s.material_defaults.assign(defaults.begin(), defaults.end());
    }

    s.constants = read_resources(r, s, s.num_constants);
    s.samplers  = read_resources(r, s, s.num_samplers);
    s.textures  = read_resources(r, s, s.num_textures);
    s.uavs      = read_resources(r, s, s.num_uavs);

    s.system_keys   = read_keys(r, s.num_system_keys);
    s.scene_keys    = read_keys(r, s.num_scene_keys);
    s.material_keys = read_keys(r, s.num_material_keys);
    s.subview_keys[0] = r.u32();
    s.subview_keys[1] = r.u32();

    s.nodes.reserve(s.num_nodes);
    for (uint32_t i = 0; i < s.num_nodes && r.ok; ++i) {
        Node n;
        n.selector   = r.u32();
        n.pass_count = r.u32();

        for (auto& idx : n.pass_indexes) idx = r.u8();

        if (s.version >= 0x0D01) {
            n.unk131[0] = r.u32();
            n.unk131[1] = r.u32();
        }

        n.system_keys.reserve(s.num_system_keys);
        for (uint32_t j = 0; j < s.num_system_keys; ++j) n.system_keys.push_back(r.u32());
        n.scene_keys.reserve(s.num_scene_keys);
        for (uint32_t j = 0; j < s.num_scene_keys; ++j) n.scene_keys.push_back(r.u32());
        n.material_keys.reserve(s.num_material_keys);
        for (uint32_t j = 0; j < s.num_material_keys; ++j) n.material_keys.push_back(r.u32());
        n.subview_keys.reserve(2);
        for (uint32_t j = 0; j < 2; ++j) n.subview_keys.push_back(r.u32());

        n.passes.reserve(n.pass_count);
        for (uint32_t j = 0; j < n.pass_count && r.ok; ++j) {
            Pass p;
            p.id = r.u32();
            p.vs = r.u32();
            p.ps = r.u32();
            if (s.version >= 0x0D01) {
                p.hs = r.u32();
                p.ds = r.u32();
                p.gs = r.u32();
            }
            n.passes.push_back(p);
        }

        s.nodes.push_back(std::move(n));
    }

    s.aliases.reserve(s.num_aliases);
    for (uint32_t i = 0; i < s.num_aliases && r.ok; ++i) {
        Alias a;
        a.selector = r.u32();
        a.node     = r.u32();
        s.aliases.push_back(a);
    }

    s.clusters.reserve(s.num_clusters);
    for (uint32_t i = 0; i < s.num_clusters && r.ok; ++i) {
        Cluster c;
        c.subview2 = r.u32();
        c.subview1 = r.u32();
        uint32_t subcluster_count = r.u32();
        c.unk = r.u32();

        c.subclusters.reserve(subcluster_count);
        for (uint32_t j = 0; j < subcluster_count && r.ok; ++j) {
            SubCluster sc;
            sc.own_index   = r.u16();
            sc.alias_count = r.u16();
            for (auto& v : sc.data) v = r.u32();
            c.subclusters.push_back(sc);
        }
        s.clusters.push_back(std::move(c));
    }

    if (!r.ok) {
        error = "unexpected end of file while parsing metadata";
        return false;
    }
    if (r.pos > s.blobs_offset) {
        error = std::format("metadata ends at 0x{:X}, past the blob section at 0x{:X}",
                            r.pos, s.blobs_offset);
        return false;
    }

    auto extra = safe_slice(data, r.pos, s.blobs_offset - r.pos);
    s.additional_data.assign(extra.begin(), extra.end());
    return true;
}

// ---------------------------------------------------------------------------
// Dump helpers
// ---------------------------------------------------------------------------

static std::string hex_bytes(std::span<const uint8_t> bytes, char sep = ' ') {
    std::string out;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (i) out.push_back(sep);
        out += std::format("{:02X}", bytes[i]);
    }
    return out;
}

static std::string escape_string(std::string_view s) {
    std::string out;
    for (unsigned char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c >= 0x20 && c < 0x7F) {
                    out.push_back(static_cast<char>(c));
                } else {
                    out += std::format("\\x{:02X}", c);
                }
                break;
        }
    }
    return out;
}

static const char* key_name(uint32_t id) {
    auto it = std::lower_bound(kShpkKeyNames.begin(), kShpkKeyNames.end(), id,
                               [](const ShpkCrcName& entry, uint32_t value) {
                                   return entry.crc < value;
                               });
    if (it != kShpkKeyNames.end() && it->crc == id) return it->name;
    return nullptr;
}

static std::string named_crc(uint32_t value) {
    if (const char* name = key_name(value)) {
        return std::format("0x{:08X} \"{}\"", value, escape_string(name));
    }
    return std::format("0x{:08X}", value);
}

static const char* material_parameter_name(uint32_t id) {
    auto it = std::lower_bound(kShpkMaterialNames.begin(), kShpkMaterialNames.end(), id,
                               [](const ShpkMaterialName& entry, uint32_t value) {
                                   return entry.crc < value;
                               });
    if (it != kShpkMaterialNames.end() && it->crc == id) return it->name;
    return nullptr;
}

static std::string named_material_parameter(uint32_t id) {
    if (const char* name = material_parameter_name(id)) {
        return std::format("0x{:08X} \"{}\"", id, escape_string(name));
    }
    return std::format("0x{:08X}", id);
}

static const char* pass_name(uint32_t id) {
    for (const ShpkPassName& entry : kShpkPassNames) {
        if (entry.crc == id) return entry.name;
    }
    return nullptr;
}

static void print_pass_id(FILE* f, uint32_t id) {
    if (const char* name = pass_name(id)) {
        std::print(f, "0x{:08X} ({})", id, name);
    } else {
        std::print(f, "0x{:08X}", id);
    }
}

static std::string material_expr(const MaterialParam& p) {
    int start_slot = p.offset / 4;
    int end_slot   = (p.offset + p.size) / 4;
    static constexpr char swizzle[] = "xyzw";

    std::string out = std::format("g_MaterialParameter[{}].", start_slot / 4);
    for (int slot = start_slot; slot < end_slot; ++slot) out.push_back(swizzle[slot % 4]);
    return out;
}

static void dump_resources(FILE* f, const char* title, const std::vector<Resource>& resources,
                           int indent) {
    if (resources.empty()) return;
    std::print(f, "{}{} ({})\n", std::string(indent, ' '), title, resources.size());
    for (size_t i = 0; i < resources.size(); ++i) {
        const Resource& r = resources[i];
        std::print(f,
                   "{}  [{}] id=0x{:08X} name=\"{}\" is_texture={} slot={} size={}\n",
                   std::string(indent, ' '), i, r.id, escape_string(r.name),
                   r.is_texture, static_cast<int16_t>(r.slot),
                   static_cast<int16_t>(r.size));
    }
}

static bool write_blob(const fs::path& dir, const std::string& name,
                       std::span<const uint8_t> data) {
    std::ofstream out(dir / name, std::ios::binary);
    if (!out) return false;
    if (!data.empty()) {
        out.write(reinterpret_cast<const char*>(data.data()),
                  static_cast<std::streamsize>(data.size()));
    }
    return static_cast<bool>(out);
}

// ---------------------------------------------------------------------------
// Optional DXBC -> HLSL decompilation
//
// The upstream HLSL-Decompiler project uses a Windows-only DXC component
// (dxbc2dxil.exe + dxilconv.dll).  We run that under Wine, then use the native
// dxil-spirv and spirv-cross builds from the same project.
// ---------------------------------------------------------------------------

struct HlslTools {
    fs::path dxbc2dxil;
    fs::path dxil_spirv;
    fs::path spirv_cross;
#ifndef _WIN32
    std::string wine = "wine64";
#endif
};

struct MaterialRewrite {
    uint32_t id = 0;
    uint16_t offset = 0;
    uint16_t size = 0;
    std::string name;
};

struct HlslMapping {
    std::unordered_map<int, std::string> cbuffer_names;
    std::unordered_map<int, std::string> sampler_names;
    std::unordered_map<int, std::string> texture_names;
    int material_slot = -1;
    std::vector<MaterialRewrite> material_params;
};

struct HlslJob {
    fs::path dxbc_path;
    fs::path hlsl_path;
    std::shared_ptr<const HlslMapping> mapping;
};

static std::optional<HlslTools> g_hlsl_tools;
static unsigned g_hlsl_threads = 16;

#ifdef _WIN32
static std::wstring windows_quote(const fs::path& path) {
    const std::wstring value = path.wstring();
    std::wstring out = L"\"";
    for (wchar_t c : value) {
        if (c == L'"') out += L"\\\"";
        else out.push_back(c);
    }
    out.push_back(L'"');
    return out;
}

static bool run_process(const fs::path& exe, const std::wstring& args) {
    std::wstring command_line = windows_quote(exe) + L" " + args;
    std::vector<wchar_t> buffer(command_line.begin(), command_line.end());
    buffer.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(exe.wstring().c_str(), buffer.data(), nullptr, nullptr,
                        FALSE, 0, nullptr, nullptr, &startup, &process)) {
        return false;
    }

    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}
#else
static std::string shell_quote(std::string_view value) {
    std::string out = "'";
    for (char c : value) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}
#endif

#ifndef _WIN32
static bool command_exists(std::string_view command) {
    if (command.empty()) return false;
    if (command.find('/') != std::string_view::npos) {
        return fs::exists(fs::path(command));
    }

    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;

    std::string_view paths(path_env);
    size_t begin = 0;
    while (begin <= paths.size()) {
        size_t end = paths.find(':', begin);
        std::string_view dir = paths.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        fs::path candidate = (dir.empty() ? fs::path(".") : fs::path(dir))
                           / fs::path(command);
        std::error_code ec;
        if (fs::exists(candidate, ec) && !fs::is_directory(candidate, ec)) return true;
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return false;
}
#endif

static std::optional<HlslTools> find_hlsl_tools(const fs::path& exe_dir,
                                                std::string& error) {
    (void)error;
    std::vector<fs::path> dirs;
    dirs.push_back(exe_dir);
    dirs.push_back(exe_dir / "tools");
    dirs.push_back(exe_dir / "tools" / "hlsl-decompiler");
    dirs.push_back(fs::current_path());
    dirs.push_back(fs::current_path() / "tools");
    dirs.push_back(fs::current_path() / "tools" / "hlsl-decompiler");

    auto find_file = [&](std::initializer_list<const char*> names) -> fs::path {
        for (const fs::path& dir : dirs) {
            for (const char* name : names) {
                fs::path candidate = dir / name;
                if (fs::exists(candidate)) return candidate;
            }
        }
        return {};
    };

    HlslTools tools;
    tools.dxbc2dxil = find_file({ "dxbc2dxil.exe" });
#ifdef _WIN32
    tools.dxil_spirv = find_file({ "dxil-spirv.exe" });
    tools.spirv_cross = find_file({ "spirv-cross.exe" });
#else
    tools.dxil_spirv = find_file({ "dxil-spirv" });
    tools.spirv_cross = find_file({ "spirv-cross" });
#endif

    if (tools.dxbc2dxil.empty() || tools.dxil_spirv.empty()
        || tools.spirv_cross.empty()) {
        return std::nullopt;
    }

#ifndef _WIN32
    const char* wine_env = std::getenv("WINE");
    std::string wine = (wine_env && *wine_env) ? wine_env : "wine64";
    if (!command_exists(wine)) {
        error = std::format("WINE executable not found: {}", wine);
        return std::nullopt;
    }
    tools.wine = std::move(wine);
#endif
    return tools;
}

#ifndef _WIN32
static bool run_command(const std::string& command) {
    int result = std::system(command.c_str());
    return result == 0;
}
#endif

static bool decompile_dxbc(const HlslTools& tools, const fs::path& dxbc_path,
                           const fs::path& hlsl_path) {
    fs::path dxil_path = dxbc_path;
    dxil_path.replace_extension(".dxil");
    fs::path spv_path = dxbc_path;
    spv_path.replace_extension(".spv");

#ifdef _WIN32
    std::wstring args1 = windows_quote(dxbc_path) + L" /o "
                       + windows_quote(dxil_path) + L" /emit-bc";
    bool ok = run_process(tools.dxbc2dxil, args1)
           && fs::exists(dxil_path) && fs::file_size(dxil_path) > 0;
    if (ok) {
        std::wstring args2 = windows_quote(dxil_path) + L" --output "
                           + windows_quote(spv_path) + L" --raw-llvm";
        ok = run_process(tools.dxil_spirv, args2)
          && fs::exists(spv_path) && fs::file_size(spv_path) > 0;
    }
    if (ok) {
        std::wstring args3 = windows_quote(spv_path) + L" --output "
                           + windows_quote(hlsl_path)
                           + L" --hlsl --shader-model 50";
        ok = run_process(tools.spirv_cross, args3)
          && fs::exists(hlsl_path) && fs::file_size(hlsl_path) > 0;
    }
#else
    // dxbc2dxil is a Windows executable and treats leading '/' in absolute Unix
    // paths as an option.  Run it from the shader directory with relative names.
    const fs::path workdir = dxbc_path.parent_path();
    std::string cmd1 = std::format(
        "cd {} && WINEDEBUG=-all {} {} {} /o {} /emit-bc 2>/dev/null",
        shell_quote(workdir.string()),
        shell_quote(tools.wine),
        shell_quote(tools.dxbc2dxil.string()),
        shell_quote(dxbc_path.filename().string()),
        shell_quote(dxil_path.filename().string()));

    bool ok = run_command(cmd1) && fs::exists(dxil_path) && fs::file_size(dxil_path) > 0;
    if (ok) {
        ok = run_command(std::format("{} {} --output {} --raw-llvm",
                                     shell_quote(tools.dxil_spirv.string()),
                                     shell_quote(dxil_path.string()),
                                     shell_quote(spv_path.string())))
             && fs::exists(spv_path) && fs::file_size(spv_path) > 0;
    }
    if (ok) {
        ok = run_command(std::format("{} {} --output {} --hlsl --shader-model 50",
                                     shell_quote(tools.spirv_cross.string()),
                                     shell_quote(spv_path.string()),
                                     shell_quote(hlsl_path.string())))
             && fs::exists(hlsl_path) && fs::file_size(hlsl_path) > 0;
    }
#endif

    std::error_code ec;
    fs::remove(dxil_path, ec);
    fs::remove(spv_path, ec);
    return ok;
}

static std::string sanitize_identifier(std::string_view value) {
    std::string out;
    out.reserve(value.size() + 1);
    for (unsigned char c : value) {
        if (std::isalnum(c) || c == '_') out.push_back(static_cast<char>(c));
        else out.push_back('_');
    }
    if (out.empty()) out = "unnamed";
    if (std::isdigit(static_cast<unsigned char>(out.front()))) out.insert(out.begin(), '_');
    return out;
}

static std::string resource_base_name(const Resource& resource) {
    if (!resource.name.empty()) return sanitize_identifier(resource.name);
    return std::format("unk_key_{:08X}", resource.id);
}

static std::string unique_identifier(std::string base, const std::string& suffix,
                                     std::unordered_set<std::string>& used) {
    std::string candidate = base;
    if (used.contains(candidate)) candidate = base + suffix;
    if (used.contains(candidate)) {
        for (unsigned i = 2;; ++i) {
            candidate = std::format("{}_{}", base, i);
            if (!used.contains(candidate)) break;
        }
    }
    used.insert(candidate);
    return candidate;
}

static std::shared_ptr<HlslMapping> build_hlsl_mapping(const Shpk& s, const Shader& sh) {
    auto mapping = std::make_shared<HlslMapping>();
    std::unordered_set<std::string> used;

    for (const Resource& constant : sh.constants) {
        if (constant.slot == 0xFFFFu) continue;
        std::string name = unique_identifier(resource_base_name(constant), "_CB", used);
        mapping->cbuffer_names[constant.slot] = name;
        if (constant.id == MaterialParamsConstantId || constant.name == "g_MaterialParameter") {
            mapping->material_slot = constant.slot;
        }
    }

    std::unordered_map<std::string, int> resource_name_counts;
    for (const Resource& sampler : sh.samplers) {
        if (sampler.slot != 0xFFFFu) ++resource_name_counts[resource_base_name(sampler)];
    }
    for (const Resource& texture : sh.textures) {
        if (texture.slot != 0xFFFFu) ++resource_name_counts[resource_base_name(texture)];
    }

    for (const Resource& sampler : sh.samplers) {
        if (sampler.slot == 0xFFFFu) continue;
        std::string base = resource_base_name(sampler);
        std::string suffix = resource_name_counts[base] > 1 ? "_S" : "_S";
        mapping->sampler_names[sampler.slot] =
            unique_identifier(base, suffix, used);
    }
    for (const Resource& texture : sh.textures) {
        if (texture.slot == 0xFFFFu) continue;
        std::string base = resource_base_name(texture);
        std::string suffix = resource_name_counts[base] > 1 ? "_T" : "_T";
        mapping->texture_names[texture.slot] =
            unique_identifier(base, suffix, used);
    }

    for (const MaterialParam& param : s.material_params) {
        MaterialRewrite rewrite;
        rewrite.id = param.id;
        rewrite.offset = param.offset;
        rewrite.size = param.size;
        if (const char* name = material_parameter_name(param.id)) {
            rewrite.name = sanitize_identifier(name);
        } else {
            rewrite.name = std::format("unk_key_{:08X}", param.id);
        }
        rewrite.name = unique_identifier(rewrite.name, "_mat", used);
        mapping->material_params.push_back(std::move(rewrite));
    }

    return mapping;
}

static std::string regex_escape(std::string_view value) {
    std::string out;
    out.reserve(value.size() * 2);
    for (char c : value) {
        if (std::string_view(".^$|()[]{}*+?\\").find(c) != std::string_view::npos) {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    return out;
}

static std::string replace_token(const std::string& text, const std::string& token,
                                 const std::string& replacement) {
    if (token.empty()) return text;
    std::regex re("\\b" + regex_escape(token) + "\\b");
    std::string out;
    size_t last = 0;
    for (std::sregex_iterator it(text.begin(), text.end(), re), end; it != end; ++it) {
        out.append(text, last, static_cast<size_t>(it->position()) - last);
        out += replacement;
        last = static_cast<size_t>(it->position() + it->length());
    }
    out.append(text, last, std::string::npos);
    return out;
}

static bool rewrite_hlsl(const HlslMapping& mapping, const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();

    auto cbuffer_names = mapping.cbuffer_names;
    auto sampler_names = mapping.sampler_names;
    auto texture_names = mapping.texture_names;

    // Fill in any binding that the ShPk metadata did not describe.
    std::regex cbuffer_decl("cbuffer\\s+\\w+\\s*:\\s*register\\(b(\\d+)\\)");
    for (std::sregex_iterator it(text.begin(), text.end(), cbuffer_decl), end;
         it != end; ++it) {
        int slot = std::stoi((*it)[1].str());
        if (!cbuffer_names.contains(slot)) {
            cbuffer_names[slot] = std::format("not_mapped_{:x}",
                                              static_cast<unsigned>(slot) * 16u);
        }
    }

    std::regex sampler_decl("Sampler\\w*\\s+\\w+\\s*:\\s*register\\(s(\\d+)\\)");
    for (std::sregex_iterator it(text.begin(), text.end(), sampler_decl), end;
         it != end; ++it) {
        int slot = std::stoi((*it)[1].str());
        if (!sampler_names.contains(slot)) {
            sampler_names[slot] = std::format("not_mapped_{}_S", slot);
        }
    }

    std::regex texture_decl("(?:Texture|Buffer)\\S*<[^>]*>\\s+\\w+\\s*:"
                            "\\s*register\\(t(\\d+)\\)");
    for (std::sregex_iterator it(text.begin(), text.end(), texture_decl), end;
         it != end; ++it) {
        int slot = std::stoi((*it)[1].str());
        if (!texture_names.contains(slot)) {
            texture_names[slot] = std::format("not_mapped_{}_T", slot);
        }
    }

    for (const auto& [slot, name] : cbuffer_names) {
        text = replace_token(text, std::format("CB{}UBO", slot), name);
        text = replace_token(text, std::format("CB{}_m0", slot), name + "_m0");
    }
    for (const auto& [slot, name] : sampler_names) {
        text = replace_token(text, std::format("S{}", slot), name);
    }
    for (const auto& [slot, name] : texture_names) {
        text = replace_token(text, std::format("T{}", slot), name);
    }

    std::map<std::string, std::string> aliases;

    if (mapping.material_slot >= 0) {
        auto it = cbuffer_names.find(mapping.material_slot);
        if (it != cbuffer_names.end()) {
            const std::string member = it->second + "_m0";
            std::regex use_re("\\b" + regex_escape(member)
                              + "\\[(\\d+)u?\\](?!\\s*:)(?:\\.([xyzw]{1,4}))?");
            std::string out;
            size_t last = 0;

            for (std::sregex_iterator use(text.begin(), text.end(), use_re), end;
                 use != end; ++use) {
                const std::smatch& match = *use;
                size_t reg = static_cast<size_t>(std::stoul(match[1].str()));
                std::string swizzle = match[2].matched ? match[2].str() : "";

                std::vector<int> components;
                if (swizzle.empty()) {
                    components = { 0, 1, 2, 3 };
                } else {
                    for (char c : swizzle) {
                        switch (c) {
                            case 'x': components.push_back(0); break;
                            case 'y': components.push_back(1); break;
                            case 'z': components.push_back(2); break;
                            case 'w': components.push_back(3); break;
                        }
                    }
                }

                const MaterialRewrite* found = nullptr;
                int param_start = 0;
                int param_end = 0;
                for (const MaterialRewrite& param : mapping.material_params) {
                    if (param.offset / 16 != reg) continue;
                    int start = (param.offset % 16) / 4;
                    int end = (param.offset % 16 + param.size) / 4;
                    bool contained = true;
                    for (int component : components) {
                        if (component < start || component >= end) {
                            contained = false;
                            break;
                        }
                    }
                    if (contained) {
                        found = &param;
                        param_start = start;
                        param_end = end;
                        break;
                    }
                }

                std::string replacement;
                if (found != nullptr) {
                    std::vector<int> full_components;
                    for (int c = param_start; c < param_end; ++c) full_components.push_back(c);

                    std::string full_swizzle;
                    for (int c : full_components) full_swizzle.push_back("xyzw"[c]);

                    std::string expression = std::format("{}[{}]", member, reg);
                    if (!(param_start == 0 && param_end == 4)) {
                        expression += "." + full_swizzle;
                    }
                    aliases[found->name] = expression;

                    if (components == full_components) {
                        replacement = found->name;
                    } else {
                        std::string relative;
                        for (int component : components) {
                            relative.push_back("xyzw"[component - param_start]);
                        }
                        replacement = found->name + "." + relative;
                    }
                } else {
                    bool any_param_in_register = false;
                    for (const MaterialRewrite& param : mapping.material_params) {
                        if (param.offset / 16 == reg) {
                            any_param_in_register = true;
                            break;
                        }
                    }

                    if (swizzle.empty() && any_param_in_register) {
                        replacement = std::format("{}[{}]", member, reg);
                    } else {
                        int start_offset = static_cast<int>(reg * 16 + components.front() * 4);
                        std::string alias = std::format("not_mapped_{:x}", start_offset);
                        if (!swizzle.empty() && components.size() > 1) {
                            alias += "_" + swizzle;
                        }
                        std::string expression = std::format("{}[{}]", member, reg);
                        if (!swizzle.empty()) expression += "." + swizzle;
                        aliases[alias] = expression;
                        replacement = alias;
                    }
                }

                out.append(text, last, static_cast<size_t>(match.position()) - last);
                out += replacement;
                last = static_cast<size_t>(match.position() + match.length());
            }
            out.append(text, last, std::string::npos);
            text = std::move(out);
        }
    }

    if (!aliases.empty()) {
        std::string block = "// shpk_dump: known-name aliases\n";
        for (const auto& [name, expression] : aliases) {
            block += std::format("#define {} {}\n", name, expression);
        }
        block += "\n";
        text = block + text;
    }

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    return static_cast<bool>(out);
}

static void decompile_jobs(const std::vector<HlslJob>& jobs) {
    if (jobs.empty() || !g_hlsl_tools) return;

    unsigned thread_count = std::min<unsigned>(
        g_hlsl_threads == 0 ? 1 : g_hlsl_threads,
        static_cast<unsigned>(jobs.size()));
    std::atomic<size_t> next{0};
    std::atomic<size_t> done{0};
    std::atomic<size_t> failed{0};
    std::mutex print_mutex;

    auto worker = [&]() {
        for (;;) {
            size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= jobs.size()) break;

            const HlslJob& job = jobs[i];
            if (decompile_dxbc(*g_hlsl_tools, job.dxbc_path, job.hlsl_path)
                && (!job.mapping || rewrite_hlsl(*job.mapping, job.hlsl_path))) {
                done.fetch_add(1, std::memory_order_relaxed);
                std::error_code ec;
                fs::remove(job.dxbc_path, ec);
            } else {
                failed.fetch_add(1, std::memory_order_relaxed);
                std::lock_guard<std::mutex> lock(print_mutex);
                std::print(stderr, "warning: HLSL decompilation/rewrite failed for {}\n",
                           job.dxbc_path.filename().string());
            }
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (unsigned i = 0; i < thread_count; ++i) threads.emplace_back(worker);
    for (auto& thread : threads) thread.join();

    if (failed.load() != 0) {
        std::print(stderr, "HLSL: {} succeeded, {} failed\n",
                   done.load(), failed.load());
    }
}

static void dump_shader_list(FILE* f, const Shpk& s, const std::vector<Shader>& shaders,
                             Stage stage, const fs::path& outdir,
                             std::vector<HlslJob>& jobs) {
    std::print(f, "\n{} Shaders ({})\n", stage_name(stage), shaders.size());
    if (shaders.empty()) return;

    const bool dx11 = is_dx11(s);
    const char* ext = dx11 ? "dxbc" : "cso";

    for (size_t i = 0; i < shaders.size(); ++i) {
        const Shader& sh = shaders[i];

        size_t extra_header_size = 0;
        if (stage == Stage::Vertex) extra_header_size = dx11 ? 8 : 4;

        auto blob = safe_slice(s.file_data,
                               static_cast<uint64_t>(s.blobs_offset) + sh.blob_offset,
                               sh.blob_size);

        std::string out_name = std::format("{}_{:04}.{}", stage_prefix(stage), i, ext);
        bool valid = !blob.empty() || sh.blob_size == 0;
        bool written = false;
        std::span<const uint8_t> bytecode;

        std::print(f, "  [{}] blob_offset=0x{:08X} blob_size={} unk={} ",
                   i, sh.blob_offset, sh.blob_size, sh.unk);

        if (!valid) {
            std::print(f, "<blob outside file>\n");
            continue;
        }

        size_t actual_extra = std::min(extra_header_size, blob.size());
        auto extra_header = blob.first(actual_extra);
        bytecode = blob.subspan(actual_extra);

        bool is_dxbc = bytecode.size() >= 4
                    && std::memcmp(bytecode.data(), "DXBC", 4) == 0;
        bool is_dx9 = bytecode.size() >= 4
                   && (std::memcmp(bytecode.data(), "DX9\0", 4) == 0
                       || std::memcmp(bytecode.data(), "DX9", 3) == 0);

        std::string hlsl_name;
        if (is_dxbc && g_hlsl_tools) {
            hlsl_name = out_name;
            hlsl_name.replace(hlsl_name.size() - 4, 4, "hlsl");
        }

        std::print(f, "extra_header=[{}] bytecode=\"{}\" output={}",
                   hex_bytes(extra_header),
                   is_dxbc ? "DXBC" : (is_dx9 ? "DX9/CSO" : "unknown"),
                   out_name);
        if (!hlsl_name.empty()) std::print(f, " hlsl={}", hlsl_name);
        std::print(f, "\n");

        written = write_blob(outdir, out_name, bytecode);
        if (!written) {
            std::print(f, "      warning: could not write {}\n", out_name);
        } else if (!hlsl_name.empty()) {
            jobs.push_back(HlslJob{
                outdir / out_name,
                outdir / hlsl_name,
                build_hlsl_mapping(s, sh)
            });
        }

        dump_resources(f, "Constants", sh.constants, 6);
        dump_resources(f, "Samplers",  sh.samplers,  6);
        dump_resources(f, "UAVs",      sh.uavs,      6);
        dump_resources(f, "Textures",  sh.textures,  6);
    }
}

static void dump_shpk(FILE* f, const Shpk& s, const fs::path& input,
                      const fs::path& outdir, std::vector<HlslJob>& jobs) {
    const bool dx11 = is_dx11(s);

    std::print(f, "ShPk dump of {}\n", input.string());
    std::print(f, "{}\n\n", std::string(78, '='));

    std::print(f, "Header\n");
    std::print(f, "  Magic:                 0x{:08X} (\"ShPk\")\n", ShPkMagic);
    std::print(f, "  Version:               0x{:04X}\n", s.version);
    std::print(f, "  DirectX:               {} ({})\n", dx11 ? 11 : 9,
               dx11 ? "DX11" : "DX9");
    std::print(f, "  File length:           {} (0x{:X})\n", s.file_length, s.file_length);
    std::print(f, "  Shader blobs offset:   0x{:08X}\n", s.blobs_offset);
    std::print(f, "  Strings offset:        0x{:08X}\n", s.strings_offset);
    std::print(f, "  Vertex shaders:        {}\n", s.vertex_shaders.size());
    std::print(f, "  Pixel shaders:         {}\n", s.pixel_shaders.size());
    std::print(f, "  Hull shaders:          {}\n", s.hull_shaders.size());
    std::print(f, "  Domain shaders:        {}\n", s.domain_shaders.size());
    std::print(f, "  Geometry shaders:      {}\n", s.geometry_shaders.size());
    std::print(f, "  Material params:       {} (raw size {}, defaults: {})\n",
               s.material_params.size(), s.material_params_size,
               s.has_defaults ? "yes" : "no");
    std::print(f, "  Constants:             {}\n", s.constants.size());
    std::print(f, "  Samplers:              {}\n", s.samplers.size());
    std::print(f, "  Textures:              {}\n", s.textures.size());
    std::print(f, "  UAVs:                  {}\n", s.uavs.size());
    std::print(f, "  System keys:           {}\n", s.system_keys.size());
    std::print(f, "  Scene keys:            {}\n", s.scene_keys.size());
    std::print(f, "  Material keys:         {}\n", s.material_keys.size());
    std::print(f, "  Nodes:                 {}\n", s.nodes.size());
    std::print(f, "  Node aliases:          {}\n", s.aliases.size());
    std::print(f, "  Node alias clusters:   {}\n", s.clusters.size());
    std::print(f, "  Additional metadata:   {} bytes\n", s.additional_data.size());
    std::print(f, "  Shader output dir:     {}\n", outdir.string());

    dump_shader_list(f, s, s.vertex_shaders,   Stage::Vertex,   outdir, jobs);
    dump_shader_list(f, s, s.pixel_shaders,    Stage::Pixel,    outdir, jobs);
    dump_shader_list(f, s, s.hull_shaders,     Stage::Hull,     outdir, jobs);
    dump_shader_list(f, s, s.domain_shaders,   Stage::Domain,   outdir, jobs);
    dump_shader_list(f, s, s.geometry_shaders, Stage::Geometry, outdir, jobs);

    std::print(f, "\nMaterial Parameters ({})\n", s.material_params.size());
    std::print(f, "  (names resolved with keys.json + approved.txt; unresolved IDs are shown as hex)\n");
    for (size_t i = 0; i < s.material_params.size(); ++i) {
        const MaterialParam& p = s.material_params[i];
        std::print(f, "  [{}] id={} offset={} size={} expr={}",
                   i, named_material_parameter(p.id), p.offset, p.size, material_expr(p));

        if (s.has_defaults && static_cast<uint64_t>(p.offset) + p.size
                                <= s.material_defaults.size()) {
            auto bytes = std::span<const uint8_t>(s.material_defaults)
                             .subspan(p.offset, p.size);
            std::print(f, " default=[{}]", hex_bytes(bytes));

            if (p.size >= 4 && p.size % 4 == 0) {
                std::print(f, " floats=(");
                for (size_t k = 0; k < p.size / 4; ++k) {
                    float v;
                    std::memcpy(&v, bytes.data() + k * 4, sizeof(v));
                    if (k) std::print(f, ", ");
                    std::print(f, "{:g}", v);
                }
                std::print(f, ")");
            }
        } else if (s.has_defaults) {
            std::print(f, " default=<outside defaults block>");
        }
        std::print(f, "\n");
    }

    std::print(f, "\nGlobal Resources\n");
    dump_resources(f, "Constants", s.constants, 2);
    dump_resources(f, "Samplers",  s.samplers,  2);
    dump_resources(f, "Textures",  s.textures,  2);
    dump_resources(f, "UAVs",      s.uavs,      2);

    std::print(f, "\nKeys\n");
    std::print(f, "  (names resolved with CrcMaps.Keys; unresolved CRCs are shown as hex)\n");
    auto dump_key_list = [&](const char* title, const std::vector<Key>& keys) {
        std::print(f, "  {} ({})\n", title, keys.size());
        for (size_t i = 0; i < keys.size(); ++i) {
            std::print(f, "    [{}] id={} default={}\n",
                       i, named_crc(keys[i].id), named_crc(keys[i].default_value));
        }
    };
    dump_key_list("System Keys", s.system_keys);
    dump_key_list("Scene Keys", s.scene_keys);
    dump_key_list("Material Keys", s.material_keys);

    std::print(f, "  Sub-View Keys (2)\n");
    std::print(f, "    [0] id=0x00000001 default={}\n", named_crc(s.subview_keys[0]));
    std::print(f, "    [1] id=0x00000002 default={}\n", named_crc(s.subview_keys[1]));

    std::vector<uint32_t> system_key_ids;
    std::vector<uint32_t> scene_key_ids;
    std::vector<uint32_t> material_key_ids;
    for (const Key& key : s.system_keys) system_key_ids.push_back(key.id);
    for (const Key& key : s.scene_keys) scene_key_ids.push_back(key.id);
    for (const Key& key : s.material_keys) material_key_ids.push_back(key.id);
    const std::vector<uint32_t> subview_key_ids{ 1, 2 };

    std::print(f, "\nNodes ({})\n", s.nodes.size());
    for (size_t i = 0; i < s.nodes.size(); ++i) {
        const Node& n = s.nodes[i];
        std::print(f, "  [{}] selector=0x{:08X} pass_count={}\n",
                   i, n.selector, n.pass_count);

        std::print(f, "      pass_indexes:");
        for (uint8_t idx : n.pass_indexes) {
            std::print(f, " {}", static_cast<int>(static_cast<int8_t>(idx)));
        }
        std::print(f, "\n");
        std::print(f, "      unk131: {} {}\n",
                   named_crc(n.unk131[0]), named_crc(n.unk131[1]));

        auto dump_node_keys = [&](const char* title, const std::vector<uint32_t>& values,
                                  const std::vector<uint32_t>& key_ids) {
            std::print(f, "      {} ({}):", title, values.size());
            if (values.empty()) {
                std::print(f, " (none)\n");
                return;
            }
            std::print(f, "\n");
            for (size_t k = 0; k < values.size(); ++k) {
                uint32_t key_id = k < key_ids.size() ? key_ids[k] : 0;
                std::print(f, "        [{}] key={} value={}\n",
                           k, named_crc(key_id), named_crc(values[k]));
            }
        };
        dump_node_keys("system_keys", n.system_keys, system_key_ids);
        dump_node_keys("scene_keys", n.scene_keys, scene_key_ids);
        dump_node_keys("material_keys", n.material_keys, material_key_ids);
        dump_node_keys("subview_keys", n.subview_keys, subview_key_ids);

        std::print(f, "      passes ({}):\n", n.passes.size());
        for (size_t j = 0; j < n.passes.size(); ++j) {
            const Pass& p = n.passes[j];
            std::print(f, "        [{}] id=", j);
            print_pass_id(f, p.id);
            std::print(f, " vs={} ps={} hs={} ds={} gs={}\n",
                       p.vs, p.ps,
                       p.hs == 0xFFFFFFFFu ? -1 : static_cast<int64_t>(p.hs),
                       p.ds == 0xFFFFFFFFu ? -1 : static_cast<int64_t>(p.ds),
                       p.gs == 0xFFFFFFFFu ? -1 : static_cast<int64_t>(p.gs));
        }
    }

    std::print(f, "\nNode Aliases ({})\n", s.aliases.size());
    for (size_t i = 0; i < s.aliases.size(); ++i) {
        std::print(f, "  [{}] selector=0x{:08X} node={}\n",
                   i, s.aliases[i].selector, s.aliases[i].node);
    }

    std::print(f, "\nNode Alias Clusters ({})\n", s.clusters.size());
    for (size_t i = 0; i < s.clusters.size(); ++i) {
        const Cluster& c = s.clusters[i];
        std::print(f, "  [{}] subview2={} subview1={} unk=0x{:08X} subclusters={}\n",
                   i, named_crc(c.subview2), named_crc(c.subview1),
                   c.unk, c.subclusters.size());

        for (size_t j = 0; j < c.subclusters.size(); ++j) {
            const SubCluster& sc = c.subclusters[j];
            std::print(f, "    [{}] index={} alias_count={}\n",
                       j, sc.own_index, sc.alias_count);

            size_t alias_pairs = std::min<size_t>(sc.alias_count, sc.data.size() / 2);
            std::print(f, "        aliases ({}):", alias_pairs);
            if (alias_pairs == 0) {
                std::print(f, " (none)\n");
            } else {
                std::print(f, "\n");
                for (size_t k = 0; k < alias_pairs; ++k) {
                    std::print(f, "          selector=0x{:08X} node={}\n",
                               sc.data[k * 2], sc.data[k * 2 + 1]);
                }
            }

            auto extra = std::span<const uint32_t>(sc.data).subspan(alias_pairs * 2);
            if (!extra.empty()) {
                std::print(f, "        extra_data ({} words):", extra.size());
                for (uint32_t v : extra) std::print(f, " 0x{:08X}", v);
                std::print(f, "\n");
            }
        }
    }

    if (!s.additional_data.empty()) {
        std::print(f, "\nAdditional Metadata ({} bytes)\n", s.additional_data.size());
        std::print(f, "  {}\n", hex_bytes(s.additional_data));
    }
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

static std::vector<uint8_t> read_file(const fs::path& path, std::string& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "could not open input file";
        return {};
    }
    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0) {
        error = "could not determine input file size";
        return {};
    }
    in.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    in.read(reinterpret_cast<char*>(data.data()), size);
    if (!in && size != 0) {
        error = "could not read input file";
        return {};
    }
    return data;
}

static bool process_file(const fs::path& input, const std::optional<fs::path>& output_base) {
    std::string error;
    auto data = read_file(input, error);
    if (data.empty() && !error.empty()) {
        std::print(stderr, "error: {}: {}\n", input.string(), error);
        return false;
    }

    Shpk shpk;
    if (!parse_shpk(data, shpk, error)) {
        std::print(stderr, "error: {}: {}\n", input.string(), error);
        return false;
    }

    fs::path base = output_base ? *output_base
                                : input.parent_path() / input.stem();
    fs::path txt_path = base;
    txt_path += ".txt";

    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) {
        std::print(stderr, "error: could not create output directory {}: {}\n",
                   base.string(), ec.message());
        return false;
    }

    FILE* f = std::fopen(txt_path.string().c_str(), "wb");
    if (!f) {
        std::print(stderr, "error: could not open {} for writing\n", txt_path.string());
        return false;
    }

    std::vector<HlslJob> jobs;
    dump_shpk(f, shpk, input, base, jobs);
    std::fclose(f);

    decompile_jobs(jobs);

    std::print("{} -> {} ({} shader blobs in {})\n", input.string(),
               txt_path.string(),
               shpk.vertex_shaders.size() + shpk.pixel_shaders.size()
                   + shpk.hull_shaders.size() + shpk.domain_shaders.size()
                   + shpk.geometry_shaders.size(),
               base.string());
    return true;
}

static void usage(const char* argv0) {
    std::print(stderr,
               "Usage: {} [-o OUTPUT_BASE] [--no-hlsl] [--hlsl-threads N] [FILE.shpk ...]\n"
               "  With no files, every .shpk in the current directory is dumped.\n"
               "  Each input FILE.shpk produces FILE.txt and a FILE/ directory\n"
               "  containing the raw shader bytecode (.dxbc for DX11, .cso for DX9)\n"
               "  and, when the decompiler tools are available, .hlsl files.\n"
               "  HLSL decompilation uses 16 threads by default.\n",
               argv0);
}

int main(int argc, char** argv) {
    std::vector<fs::path> inputs;
    std::optional<fs::path> output_base;
    bool decompile_hlsl = true;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (arg == "-o" || arg == "--out") {
            if (i + 1 >= argc) {
                std::print(stderr, "error: {} requires an argument\n", arg);
                return 2;
            }
            output_base = fs::path(argv[++i]);
            continue;
        }
        if (arg == "--no-hlsl") {
            decompile_hlsl = false;
            continue;
        }
        if (arg == "--hlsl-threads") {
            if (i + 1 >= argc) {
                std::print(stderr, "error: {} requires a number\n", arg);
                return 2;
            }
            g_hlsl_threads = static_cast<unsigned>(std::stoul(argv[++i]));
            continue;
        }
        inputs.emplace_back(arg);
    }

    if (decompile_hlsl) {
        fs::path exe_dir = fs::absolute(argv[0]).parent_path();
        std::string tools_error;
        g_hlsl_tools = find_hlsl_tools(exe_dir, tools_error);
        if (!g_hlsl_tools) {
            if (!tools_error.empty()) {
                std::print(stderr, "error: {}\n", tools_error);
            } else {
                std::print(stderr,
                           "note: HLSL decompiler tools not found; only .dxbc/.cso will be written.\n"
                           "      Expected tools/hlsl-decompiler/ with dxbc2dxil.exe, "
                           "dxil-spirv and spirv-cross.\n");
            }
        }
    }

    if (output_base && inputs.size() > 1) {
        std::print(stderr, "error: -o/--out can only be used with one input file\n");
        return 2;
    }

    if (inputs.empty()) {
        std::error_code ec;
        for (const auto& entry : fs::directory_iterator(fs::current_path(), ec)) {
            if (entry.is_regular_file() && entry.path().extension() == ".shpk") {
                inputs.push_back(entry.path());
            }
        }
        std::sort(inputs.begin(), inputs.end());
        if (inputs.empty()) {
            usage(argv[0]);
            return 1;
        }
    }

    bool any_failed = false;
    for (const auto& input : inputs) {
        if (!process_file(input, output_base)) any_failed = true;
    }
    return any_failed ? 1 : 0;
}
