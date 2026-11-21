cmake_minimum_required(VERSION 3.20)

project(shpk_dump LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(_config IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
    string(TOUPPER "${_config}" _config_upper)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_config_upper} ${CMAKE_BINARY_DIR}/bin)
endforeach()

find_package(Threads REQUIRED)

option(SHPK_BUILD_HLSL_TOOLS
    "Build dxil-spirv and spirv-cross from source for HLSL decompilation" ON)
option(SHPK_BUNDLE_HLSL_TOOLS
    "Copy the HLSL decompiler tools next to shpk_dump" ON)

set(SHPK_HLSL_WINDOWS_DIR
    ""
    CACHE PATH "Optional local directory containing dxbc2dxil.exe and dxilconv.dll")
set(SHPK_TOOLS_BUNDLE_DIR
    "${CMAKE_BINARY_DIR}/bin/tools/hlsl-decompiler"
    CACHE PATH "Directory where the decompiler tools are bundled")

if (SHPK_BUNDLE_HLSL_TOOLS)
    set(SHPK_WINDOWS_BINARIES "")

    if (SHPK_BUILD_HLSL_TOOLS)
        set(DXIL_SPIRV_CLI ON CACHE BOOL "" FORCE)
        set(DXIL_SPIRV_NATIVE_LLVM OFF CACHE BOOL "" FORCE)
        set(DXIL_SPIRV_MISC_CLI OFF CACHE BOOL "" FORCE)
        set(DXIL_SPIRV_FUZZ_CLI OFF CACHE BOOL "" FORCE)

        # Only build the SPIRV-Cross HLSL path.  The full CLI would also pull
        # in MSL, C++ and reflection backends we never use.
        set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
        set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
        set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
        set(SPIRV_CROSS_ENABLE_HLSL ON CACHE BOOL "" FORCE)
        set(SPIRV_CROSS_ENABLE_MSL OFF CACHE BOOL "" FORCE)
        set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
        set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
        set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
        set(SPIRV_CROSS_ENABLE_C_API ON CACHE BOOL "" FORCE)

        set(SHPK_DXIL_SPIRV_SOURCE
            "${CMAKE_CURRENT_SOURCE_DIR}/external/dxil-spirv")
        if (EXISTS "${SHPK_DXIL_SPIRV_SOURCE}/CMakeLists.txt")
            add_subdirectory("${SHPK_DXIL_SPIRV_SOURCE}"
                "${CMAKE_BINARY_DIR}/external/dxil-spirv" EXCLUDE_FROM_ALL)
        else()
            include(FetchContent)
            FetchContent_Declare(dxil_spirv
                GIT_REPOSITORY https://github.com/HansKristian-Work/dxil-spirv.git
                GIT_TAG 34f0ff845eb7f1ca76ca8df5d1593d6913f19e9c
                GIT_SUBMODULES_RECURSE TRUE
            )
            FetchContent_MakeAvailable(dxil_spirv)
        endif()

        add_executable(spirv-cross-minimal spirv_cross_hlsl.cpp)
        target_link_libraries(spirv-cross-minimal PRIVATE spirv-cross-hlsl)
        set_target_properties(spirv-cross-minimal PROPERTIES
            OUTPUT_NAME spirv-cross
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
            RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
        )

        set_target_properties(dxil-spirv PROPERTIES
            BUILD_RPATH "$ORIGIN"
            INSTALL_RPATH "$ORIGIN"
        )

        # The DXBC -> DXIL step is Windows-only upstream.  Reuse the local
        # prebuilt files when present, otherwise fetch the HLSL-Decompiler
        # release that contains them.
        if (SHPK_HLSL_WINDOWS_DIR AND
            EXISTS "${SHPK_HLSL_WINDOWS_DIR}/dxbc2dxil.exe")
            list(APPEND SHPK_WINDOWS_BINARIES
                "${SHPK_HLSL_WINDOWS_DIR}/dxbc2dxil.exe"
                "${SHPK_HLSL_WINDOWS_DIR}/dxilconv.dll")
        else()
            set(SHPK_HLSL_RELEASE_URL
                "https://github.com/YYadorigi/HLSL-Decompiler/releases/download/v0.6/HLSLDecompiler.zip"
                CACHE STRING "HLSL-Decompiler release archive")

            set(_archive "${CMAKE_BINARY_DIR}/_deps/HLSLDecompiler.zip")
            set(_extract "${CMAKE_BINARY_DIR}/_deps/HLSLDecompiler")
            if (EXISTS "${_extract}/dxbc2dxil.exe" AND
                EXISTS "${_extract}/dxilconv.dll")
                set(_have_shim TRUE)
            else()
                set(_have_shim FALSE)
            endif()

            if (NOT _have_shim)
                if (NOT EXISTS "${_archive}")
                    file(DOWNLOAD "${SHPK_HLSL_RELEASE_URL}" "${_archive}"
                         STATUS _download_status SHOW_PROGRESS)
                    list(GET _download_status 0 _download_code)
                else()
                    set(_download_code 0)
                endif()

                if (_download_code EQUAL 0)
                    file(MAKE_DIRECTORY "${_extract}")
                    file(ARCHIVE_EXTRACT INPUT "${_archive}" DESTINATION "${_extract}")
                endif()
            endif()

            if (EXISTS "${_extract}/dxbc2dxil.exe")
                list(APPEND SHPK_WINDOWS_BINARIES
                    "${_extract}/dxbc2dxil.exe"
                    "${_extract}/dxilconv.dll")
            else()
                message(WARNING
                    "Could not download the Windows dxbc2dxil component. "
                    "Set SHPK_HLSL_WINDOWS_DIR to a directory containing "
                    "dxbc2dxil.exe and dxilconv.dll.")
            endif()
        endif()

        set(_windows_copy_commands "")
        foreach (_file IN LISTS SHPK_WINDOWS_BINARIES)
            list(APPEND _windows_copy_commands
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_file}" "${SHPK_TOOLS_BUNDLE_DIR}/")
        endforeach()

        set(_symlink_commands "")
        if (UNIX AND NOT APPLE)
            list(APPEND _symlink_commands
                COMMAND ${CMAKE_COMMAND} -E rm -f
                    "${SHPK_TOOLS_BUNDLE_DIR}/$<TARGET_SONAME_FILE_NAME:dxil-spirv-c-shared>"
                COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "$<TARGET_FILE_NAME:dxil-spirv-c-shared>"
                    "${SHPK_TOOLS_BUNDLE_DIR}/$<TARGET_SONAME_FILE_NAME:dxil-spirv-c-shared>"
                COMMAND ${CMAKE_COMMAND} -E rm -f
                    "${SHPK_TOOLS_BUNDLE_DIR}/$<TARGET_LINKER_FILE_NAME:dxil-spirv-c-shared>"
                COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "$<TARGET_FILE_NAME:dxil-spirv-c-shared>"
                    "${SHPK_TOOLS_BUNDLE_DIR}/$<TARGET_LINKER_FILE_NAME:dxil-spirv-c-shared>"
            )
        endif()

        add_custom_target(hlsl_tools ALL
            COMMAND ${CMAKE_COMMAND} -E make_directory "${SHPK_TOOLS_BUNDLE_DIR}"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:dxil-spirv>
                "${SHPK_TOOLS_BUNDLE_DIR}/$<TARGET_FILE_NAME:dxil-spirv>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:spirv-cross-minimal>
                "${SHPK_TOOLS_BUNDLE_DIR}/$<TARGET_FILE_NAME:spirv-cross-minimal>"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:dxil-spirv-c-shared> "${SHPK_TOOLS_BUNDLE_DIR}/"
            ${_symlink_commands}
            ${_windows_copy_commands}
            DEPENDS dxil-spirv spirv-cross-minimal dxil-spirv-c-shared
            VERBATIM
        )
    else()
        if (SHPK_HLSL_WINDOWS_DIR AND
            EXISTS "${SHPK_HLSL_WINDOWS_DIR}/dxbc2dxil.exe")
            add_custom_target(hlsl_tools ALL
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SHPK_TOOLS_BUNDLE_DIR}"
                COMMAND ${CMAKE_COMMAND} -E copy_directory
                    "${SHPK_HLSL_WINDOWS_DIR}" "${SHPK_TOOLS_BUNDLE_DIR}"
                VERBATIM
            )
        else()
            message(WARNING
                "SHPK_BUILD_HLSL_TOOLS is OFF and no prebuilt Windows tools "
                "were found in SHPK_HLSL_WINDOWS_DIR.")
        endif()
    endif()
endif()

add_executable(shpk_dump
    shpk_dump.cpp
    shpk_crc_keys.hpp
    shpk_material_names.hpp
    shpk_pass_names.hpp
)

set_target_properties(shpk_dump PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin"
)

target_include_directories(shpk_dump PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(shpk_dump PRIVATE
    Threads::Threads
)

if (TARGET hlsl_tools)
    add_dependencies(shpk_dump hlsl_tools)
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(shpk_dump PRIVATE
        -Wall
        -Wextra
    )
elseif (MSVC)
    target_compile_options(shpk_dump PRIVATE
        /W4
    )
endif()

include(GNUInstallDirs)
install(TARGETS shpk_dump
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

if (SHPK_BUNDLE_HLSL_TOOLS AND TARGET hlsl_tools)
    install(DIRECTORY "${SHPK_TOOLS_BUNDLE_DIR}/"
        DESTINATION ${CMAKE_INSTALL_BINDIR}/tools/hlsl-decompiler
    )
endif()
