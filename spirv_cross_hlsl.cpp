// Minimal SPIR-V -> HLSL converter used by shpk_dump.
// Keeps the dependency surface to SPIRV-Cross' HLSL backend only.

#include <spirv_hlsl.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

static bool read_file(const std::string& path, std::vector<uint32_t>& data) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;

    in.seekg(0, std::ios::end);
    std::streamoff size = in.tellg();
    if (size < 0 || (size % 4) != 0) return false;
    in.seekg(0, std::ios::beg);

    data.resize(static_cast<size_t>(size) / 4);
    if (!data.empty()) {
        in.read(reinterpret_cast<char*>(data.data()), size);
        if (!in) return false;
    }
    return true;
}

static bool write_file(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

int main(int argc, char** argv) {
    std::string input;
    std::string output;
    unsigned shader_model = 50;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--output" || arg == "-o") {
            if (++i >= argc) {
                std::cerr << "spirv-cross: missing output path\n";
                return 2;
            }
            output = argv[i];
        } else if (arg == "--shader-model") {
            if (++i >= argc) {
                std::cerr << "spirv-cross: missing shader model\n";
                return 2;
            }
            shader_model = static_cast<unsigned>(std::stoul(argv[i]));
        } else if (arg == "--hlsl" || arg == "--flatten-ubo") {
            // Accepted for compatibility; the HLSL backend is always selected.
        } else if (!arg.empty() && arg[0] != '-') {
            input = arg;
        } else {
            std::cerr << "spirv-cross: unsupported option " << arg << "\n";
            return 2;
        }
    }

    if (input.empty()) {
        std::cerr << "spirv-cross: missing input\n";
        return 2;
    }

    std::vector<uint32_t> spirv;
    if (!read_file(input, spirv)) {
        std::cerr << "spirv-cross: failed to read " << input << "\n";
        return 1;
    }

    try {
        spirv_cross::CompilerHLSL compiler(std::move(spirv));
        spirv_cross::CompilerHLSL::Options options;
        options.shader_model = shader_model;
        compiler.set_hlsl_options(options);

        std::string hlsl = compiler.compile();
        if (!output.empty()) {
            if (!write_file(output, hlsl)) {
                std::cerr << "spirv-cross: failed to write " << output << "\n";
                return 1;
            }
        } else {
            std::cout << hlsl;
        }
    } catch (const std::exception& error) {
        std::cerr << "spirv-cross: " << error.what() << "\n";
        return 1;
    }

    return 0;
}
