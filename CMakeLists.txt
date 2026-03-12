cmake_minimum_required(VERSION 3.21)

if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)
endif()

if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/ToolscreenVersion.cmake")
set(TOOLSCREEN_VERSION "${TOOLSCREEN_VERSION_MAJOR}.${TOOLSCREEN_VERSION_MINOR}.${TOOLSCREEN_VERSION_PATCH}")

project(Toolscreen VERSION ${TOOLSCREEN_VERSION} LANGUAGES C CXX RC)

set(TOOLSCREEN_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(TOOLSCREEN_GENERATED_VERSION_HEADER "${TOOLSCREEN_GENERATED_DIR}/version.h")
file(MAKE_DIRECTORY "${TOOLSCREEN_GENERATED_DIR}")
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/version.h.in"
    "${TOOLSCREEN_GENERATED_VERSION_HEADER}"
    @ONLY
)

if(NOT WIN32)
    message(FATAL_ERROR "Toolscreen only supports Windows builds.")
endif()

option(TOOLSCREEN_ENABLE_JAR_PACKAGING "Enable the EasyInjectBundled jar packaging target." ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(TOOLSCREEN_ARTIFACT_ROOT "${CMAKE_BINARY_DIR}/bin")

function(toolscreen_configure_target_outputs target_name)
    foreach(config Debug Release RelWithDebInfo MinSizeRel)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(${target_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY_${config_upper} "${TOOLSCREEN_ARTIFACT_ROOT}/${config}"
            LIBRARY_OUTPUT_DIRECTORY_${config_upper} "${TOOLSCREEN_ARTIFACT_ROOT}/${config}"
            ARCHIVE_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/lib/${config}"
            PDB_OUTPUT_DIRECTORY_${config_upper} "${TOOLSCREEN_ARTIFACT_ROOT}/${config}"
            COMPILE_PDB_OUTPUT_DIRECTORY_${config_upper} "${CMAKE_BINARY_DIR}/obj/${config}"
        )
    endforeach()
endfunction()

function(toolscreen_enable_release_symbols target_name)
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            $<$<CONFIG:Release>:/Zi>
            $<$<CONFIG:MinSizeRel>:/Zi>
        )

        if(TARGET ${target_name} AND NOT "${target_name}" MATCHES "_minhook$|_glew$|_imgui$")
            target_link_options(${target_name} PRIVATE
                $<$<CONFIG:Release>:/DEBUG:FULL>
                $<$<CONFIG:MinSizeRel>:/DEBUG:FULL>
            )
        endif()
    endif()
endfunction()

function(toolscreen_set_brand_version branding_file version_string)
    file(READ "${branding_file}" toolscreen_branding_contents)
    string(REPLACE "\r\n" "\n" toolscreen_branding_contents "${toolscreen_branding_contents}")

    if(toolscreen_branding_contents MATCHES "(^|\n)brand\\.version=")
        string(REGEX REPLACE "(^|\n)brand\\.version=[^\n]*" "\\1brand.version=${version_string}"
            toolscreen_branding_contents "${toolscreen_branding_contents}")
    else()
        string(APPEND toolscreen_branding_contents "\nbrand.version=${version_string}\n")
    endif()

    file(WRITE "${branding_file}" "${toolscreen_branding_contents}")
endfunction()

if(MSVC)
    set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} /O2 /Ob2 /GL")
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /O2 /Ob2 /GL")
    set(CMAKE_SHARED_LINKER_FLAGS_RELEASE "${CMAKE_SHARED_LINKER_FLAGS_RELEASE} /LTCG")
endif()

include(FetchContent)

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/m417z/minhook-detours
    GIT_TAG v1.0.6
)

FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
    FetchContent_Populate(minhook)

    add_library(toolscreen_minhook STATIC
        ${minhook_SOURCE_DIR}/src/MinHook.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Disassembler.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/InlineHook.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Instruction.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Memory.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Thread.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Trampoline.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Transaction.c
    )

    target_include_directories(toolscreen_minhook PUBLIC
        ${minhook_SOURCE_DIR}/src
        ${minhook_SOURCE_DIR}/src/phnt
    )

    set_target_properties(toolscreen_minhook PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

    toolscreen_enable_release_symbols(toolscreen_minhook)

add_library(Toolscreen::minhook ALIAS toolscreen_minhook)

FetchContent_Declare(
    glew
    URL https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui
    GIT_TAG v1.92.6
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)

FetchContent_GetProperties(glew)
if(NOT glew_POPULATED)
    FetchContent_Populate(glew)

    add_library(toolscreen_glew STATIC
        ${glew_SOURCE_DIR}/src/glew.c
    )

    target_include_directories(toolscreen_glew PUBLIC ${glew_SOURCE_DIR}/include)
    target_compile_definitions(toolscreen_glew PUBLIC GLEW_STATIC)

    set_target_properties(toolscreen_glew PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        OUTPUT_NAME "libglew32"
    )
endif()

    toolscreen_enable_release_symbols(toolscreen_glew)

add_library(Toolscreen::glew ALIAS toolscreen_glew)

FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
endif()

FetchContent_GetProperties(nlohmann_json)
if(NOT nlohmann_json_POPULATED)
    FetchContent_Populate(nlohmann_json)
endif()

set(TOOLSCREEN_IMGUI_DIR "${imgui_SOURCE_DIR}")

add_library(toolscreen_imgui STATIC
    ${TOOLSCREEN_IMGUI_DIR}/imgui.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_draw.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_tables.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_widgets.cpp
    ${TOOLSCREEN_IMGUI_DIR}/imgui_demo.cpp
    ${TOOLSCREEN_IMGUI_DIR}/misc/cpp/imgui_stdlib.cpp
    ${TOOLSCREEN_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp
    ${TOOLSCREEN_IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(toolscreen_imgui PUBLIC
    ${TOOLSCREEN_IMGUI_DIR}
    ${TOOLSCREEN_IMGUI_DIR}/backends
    ${TOOLSCREEN_IMGUI_DIR}/misc/cpp
)

target_compile_definitions(toolscreen_imgui PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLEW)

target_link_libraries(toolscreen_imgui PUBLIC
    Toolscreen::glew
    dwmapi
    gdi32
    imm32
    opengl32
)

set_target_properties(toolscreen_imgui PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)

toolscreen_enable_release_symbols(toolscreen_imgui)

find_package(OpenGL REQUIRED)

set(TOOLSCREEN_MODULE_DIRS
    bootstrap
    common
    config
    features
    gui
    gui/tabs
    hooks
    platform
    render
    runtime
    third_party
)

set(TOOLSCREEN_SOURCES)
set(TOOLSCREEN_HEADERS)

foreach(module_dir IN LISTS TOOLSCREEN_MODULE_DIRS)
    file(GLOB module_sources CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/${module_dir}/*.cpp"
    )
    file(GLOB module_headers CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/${module_dir}/*.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/${module_dir}/*.inl"
    )

    list(APPEND TOOLSCREEN_SOURCES ${module_sources})
    list(APPEND TOOLSCREEN_HEADERS ${module_headers})
endforeach()

list(REMOVE_DUPLICATES TOOLSCREEN_SOURCES)
list(REMOVE_DUPLICATES TOOLSCREEN_HEADERS)

add_library(${PROJECT_NAME} SHARED
    ${TOOLSCREEN_SOURCES}
    ${TOOLSCREEN_HEADERS}
    ${TOOLSCREEN_GENERATED_VERSION_HEADER}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/resource.rc
)

source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}/src" PREFIX "Source Files" FILES
    ${TOOLSCREEN_SOURCES}
    ${TOOLSCREEN_HEADERS}
    ${CMAKE_CURRENT_SOURCE_DIR}/src/platform/resource.rc
)
source_group("Generated Files" FILES ${TOOLSCREEN_GENERATED_VERSION_HEADER})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${TOOLSCREEN_GENERATED_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${nlohmann_json_SOURCE_DIR}/single_include
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    TOOLSCREEN_EXPORTS
    CMAKE_BUILD
    GLEW_STATIC
    NOMINMAX
    UNICODE
    _UNICODE
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Toolscreen::minhook
    Toolscreen::glew
    toolscreen_imgui
    OpenGL::GL
    comdlg32
    dbghelp
    dwmapi
    gdi32
    imm32
    msimg32
    ntdll
    opengl32
    psapi
    shell32
    shlwapi
    user32
    version
    winhttp
    winmm
)

if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3
        /MP
        /EHa
        /bigobj
    )
endif()

toolscreen_configure_target_outputs(${PROJECT_NAME})
toolscreen_enable_release_symbols(${PROJECT_NAME})

enable_testing()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/cmake")
file(GENERATE
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_artifact_$<CONFIG>.cmake"
    CONTENT "if(NOT EXISTS \"$<TARGET_FILE:${PROJECT_NAME}>\")\n  message(FATAL_ERROR \"Expected built Toolscreen artifact at '$<TARGET_FILE:${PROJECT_NAME}>'\")\nendif()\nif(NOT EXISTS \"$<TARGET_PDB_FILE:${PROJECT_NAME}>\")\n  message(FATAL_ERROR \"Expected Toolscreen PDB at '$<TARGET_PDB_FILE:${PROJECT_NAME}>'\")\nendif()\n"
)

add_test(
    NAME toolscreen_artifact_exists
    COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_artifact_$<CONFIG>.cmake"
)

if(TOOLSCREEN_ENABLE_JAR_PACKAGING)
    find_package(Java COMPONENTS Development REQUIRED)

    find_program(TOOLSCREEN_MAVEN_EXECUTABLE NAMES mvn.cmd mvn)

    if(NOT TOOLSCREEN_MAVEN_EXECUTABLE)
        FetchContent_Declare(
            maven
            URL https://archive.apache.org/dist/maven/maven-3/3.9.9/binaries/apache-maven-3.9.9-bin.zip
            URL_HASH SHA512=8beac8d11ef208f1e2a8df0682b9448a9a363d2ad13ca74af43705549e72e74c9378823bf689287801cbbfc2f6ea9596201d19ccacfdfb682ee8a2ff4c4418ba
        )

        FetchContent_GetProperties(maven)
        if(NOT maven_POPULATED)
            FetchContent_Populate(maven)
        endif()

        set(TOOLSCREEN_MAVEN_EXECUTABLE "${maven_SOURCE_DIR}/bin/mvn.cmd")
    endif()

    FetchContent_Declare(
        easyinjectbundled
        GIT_REPOSITORY https://github.com/jojoe77777/EasyInjectBundled.git
        GIT_TAG main
        GIT_SHALLOW TRUE
    )

    FetchContent_GetProperties(easyinjectbundled)
    if(NOT easyinjectbundled_POPULATED)
        FetchContent_Populate(easyinjectbundled)
    endif()

    set(TOOLSCREEN_EASYINJECT_BRANDING_PROPERTIES "${easyinjectbundled_SOURCE_DIR}/branding.properties")
    toolscreen_set_brand_version("${TOOLSCREEN_EASYINJECT_BRANDING_PROPERTIES}" "${PROJECT_VERSION}")

    FetchContent_Declare(
        easyinjectbundled_exe
        GIT_REPOSITORY https://github.com/jojoe77777/EasyInjectBundled.git
        GIT_TAG exe
        GIT_SHALLOW TRUE
    )

    FetchContent_GetProperties(easyinjectbundled_exe)
    if(NOT easyinjectbundled_exe_POPULATED)
        FetchContent_Populate(easyinjectbundled_exe)
    endif()

    set(TOOLSCREEN_EASYINJECT_EXE_BRANDING_PROPERTIES "${easyinjectbundled_exe_SOURCE_DIR}/branding.properties")
    toolscreen_set_brand_version("${TOOLSCREEN_EASYINJECT_EXE_BRANDING_PROPERTIES}" "${PROJECT_VERSION}")

    set(JAR_NAME "Toolscreen-${PROJECT_VERSION}-double-click-me.jar")
    set(JAR_OUTPUT "${TOOLSCREEN_ARTIFACT_ROOT}/$<CONFIG>/${JAR_NAME}")
    set(EXE_NAME "Toolscreen-${PROJECT_VERSION}-double-click-me.exe")
    set(EXE_OUTPUT "${TOOLSCREEN_ARTIFACT_ROOT}/$<CONFIG>/${EXE_NAME}")

    add_custom_target(jar ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${easyinjectbundled_SOURCE_DIR}/custom-dlls"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TOOLSCREEN_ARTIFACT_ROOT}/$<CONFIG>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${easyinjectbundled_SOURCE_DIR}/custom-dlls/Toolscreen.dll"
        COMMAND "${TOOLSCREEN_MAVEN_EXECUTABLE}" -f "${easyinjectbundled_SOURCE_DIR}/pom.xml" clean package -q
                -Dbrand.name=Toolscreen
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${easyinjectbundled_SOURCE_DIR}/target/${JAR_NAME}" "${JAR_OUTPUT}"
        DEPENDS ${PROJECT_NAME}
        COMMENT "Building ${JAR_NAME}"
        VERBATIM
    )

    file(MAKE_DIRECTORY "${easyinjectbundled_exe_SOURCE_DIR}/custom-dlls")
    add_subdirectory("${easyinjectbundled_exe_SOURCE_DIR}/exe" "${CMAKE_CURRENT_BINARY_DIR}/easyinjectbundled-exe")
    toolscreen_configure_target_outputs(EasyInjectExe)

    add_custom_target(toolscreen_prepare_easyinject_exe_inputs
        COMMAND ${CMAKE_COMMAND} -E make_directory "${easyinjectbundled_exe_SOURCE_DIR}/custom-dlls"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${easyinjectbundled_exe_SOURCE_DIR}/custom-dlls/Toolscreen.dll"
        DEPENDS ${PROJECT_NAME}
        COMMENT "Preparing EasyInject exe packaging inputs"
        VERBATIM
    )

    add_dependencies(EasyInjectExe toolscreen_prepare_easyinject_exe_inputs)

    add_custom_target(installer_exe ALL
        DEPENDS EasyInjectExe
        COMMENT "Building ${EXE_NAME}"
    )

    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_jar_$<CONFIG>.cmake"
        CONTENT "if(NOT EXISTS \"${JAR_OUTPUT}\")\n  message(FATAL_ERROR \"Expected Toolscreen jar at '${JAR_OUTPUT}'\")\nendif()\n"
    )

    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_exe_$<CONFIG>.cmake"
        CONTENT "if(NOT EXISTS \"${EXE_OUTPUT}\")\n  message(FATAL_ERROR \"Expected Toolscreen exe at '${EXE_OUTPUT}'\")\nendif()\n"
    )

    file(GENERATE
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_exe_embeds_dll_$<CONFIG>.cmake"
        CONTENT "set(EMBED_DLL_INDEX_FILE \"${CMAKE_CURRENT_BINARY_DIR}/easyinjectbundled-exe/embedded_dll_index.txt\")\nif(NOT EXISTS \"${CMAKE_CURRENT_BINARY_DIR}/easyinjectbundled-exe/embedded_dll_index.txt\")\n  message(FATAL_ERROR \"Expected EasyInject embedded DLL index at '${CMAKE_CURRENT_BINARY_DIR}/easyinjectbundled-exe/embedded_dll_index.txt'\")\nendif()\nfile(READ \"${CMAKE_CURRENT_BINARY_DIR}/easyinjectbundled-exe/embedded_dll_index.txt\" EMBED_DLL_INDEX_CONTENT)\nif(NOT EMBED_DLL_INDEX_CONTENT MATCHES \"Toolscreen[.]dll\")\n  message(FATAL_ERROR \"Expected EasyInject EXE packaging metadata to embed Toolscreen.dll\")\nendif()\n"
    )

    add_test(
        NAME toolscreen_jar_exists
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_jar_$<CONFIG>.cmake"
    )

    add_test(
        NAME toolscreen_exe_exists
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_exe_$<CONFIG>.cmake"
    )

    add_test(
        NAME toolscreen_exe_embeds_dll
        COMMAND ${CMAKE_COMMAND} -P "${CMAKE_CURRENT_BINARY_DIR}/cmake/check_toolscreen_exe_embeds_dll_$<CONFIG>.cmake"
    )
endif()

