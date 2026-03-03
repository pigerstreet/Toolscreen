cmake_minimum_required(VERSION 3.15)

# Set policies to avoid warnings
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)  # Use extraction timestamp for URL downloads
endif()
if(POLICY CMP0169)
    cmake_policy(SET CMP0169 OLD)  # Allow FetchContent_Populate for custom builds
endif()
if(POLICY CMP0177)
    cmake_policy(SET CMP0177 NEW)  # Normalize install paths
endif()

# Read version from version.h
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/src/version.h" VERSION_FILE)
string(REGEX REPLACE ".*TOOLSCREEN_VERSION_MAJOR[ \t]+([0-9]+).*" [[\1]] TOOLSCREEN_VERSION_MAJOR "${VERSION_FILE}")
string(REGEX REPLACE ".*TOOLSCREEN_VERSION_MINOR[ \t]+([0-9]+).*" [[\1]] TOOLSCREEN_VERSION_MINOR "${VERSION_FILE}")
string(REGEX REPLACE ".*TOOLSCREEN_VERSION_PATCH[ \t]+([0-9]+).*" [[\1]] TOOLSCREEN_VERSION_PATCH "${VERSION_FILE}")
set(TOOLSCREEN_VERSION ${TOOLSCREEN_VERSION_MAJOR}.${TOOLSCREEN_VERSION_MINOR}.${TOOLSCREEN_VERSION_PATCH})

project(Toolscreen VERSION ${TOOLSCREEN_VERSION})

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)

FetchContent_Declare(
    minhook
    GIT_REPOSITORY https://github.com/m417z/minhook-detours
    GIT_TAG v1.0.6
)
FetchContent_Declare(
    glew
    URL https://github.com/nigels-com/glew/releases/download/glew-2.2.0/glew-2.2.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.9
)
FetchContent_Declare(
    tomlplusplus
    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
    GIT_TAG v3.4.0
)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)

FetchContent_MakeAvailable(tomlplusplus nlohmann_json)
FetchContent_GetProperties(minhook)
if(NOT minhook_POPULATED)
    FetchContent_Populate(minhook)
    add_library(minhook STATIC
        ${minhook_SOURCE_DIR}/src/MinHook.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Disassembler.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/InlineHook.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Instruction.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Memory.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Thread.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Trampoline.c
        ${minhook_SOURCE_DIR}/src/SlimDetours/Transaction.c
    )
    target_include_directories(minhook PUBLIC 
        ${minhook_SOURCE_DIR}/src
        ${minhook_SOURCE_DIR}/src/phnt
    )
    set_target_properties(minhook PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()
FetchContent_GetProperties(glew)
if(NOT glew_POPULATED)
    FetchContent_Populate(glew)
    add_library(glew_static STATIC
        ${glew_SOURCE_DIR}/src/glew.c
    )
    target_include_directories(glew_static PUBLIC ${glew_SOURCE_DIR}/include)
    target_compile_definitions(glew_static PUBLIC GLEW_STATIC)
    set_target_properties(glew_static PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    add_library(GLEW::GLEW ALIAS glew_static)
endif()
FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
    FetchContent_Populate(imgui)
    add_library(imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/imgui_demo.cpp
        ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    )
    target_include_directories(imgui PUBLIC 
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
        ${imgui_SOURCE_DIR}/misc/cpp
    )
    target_compile_definitions(imgui PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLEW)
    set_target_properties(imgui PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

find_package(OpenGL REQUIRED)
find_package(Java COMPONENTS Runtime)

file(GLOB_RECURSE SOURCES "src/*.cpp")
file(GLOB_RECURSE HEADERS "src/*.h" "src/*.inl")

add_library(${PROJECT_NAME} SHARED
    ${SOURCES}
    ${HEADERS}
    src/Toolscreen.rc
)

set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
)

target_include_directories(${PROJECT_NAME} PRIVATE src)

target_link_libraries(${PROJECT_NAME} PRIVATE
    minhook
    GLEW::GLEW
    imgui
    tomlplusplus::tomlplusplus
    nlohmann_json::nlohmann_json
    OpenGL::GL
)

target_compile_definitions(${PROJECT_NAME} PRIVATE
    TOOLSCREEN_EXPORTS
    GLEW_STATIC
    UNICODE
)

if(MSVC)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        comdlg32 msimg32 dbghelp psapi shlwapi ntdll
    )
    target_compile_options(${PROJECT_NAME} PRIVATE
        /W3
        /MP
        /EHa
    )
endif()

if(Java_JAVA_EXECUTABLE)
    FetchContent_Declare(
        maven
        URL https://archive.apache.org/dist/maven/maven-3/3.9.9/binaries/apache-maven-3.9.9-bin.zip
        URL_HASH SHA512=8beac8d11ef208f1e2a8df0682b9448a9a363d2ad13ca74af43705549e72e74c9378823bf689287801cbbfc2f6ea9596201d19ccacfdfb682ee8a2ff4c4418ba
    )
    FetchContent_GetProperties(maven)
    if(NOT maven_POPULATED)
        FetchContent_Populate(maven)
    endif()

    FetchContent_Declare(
        easyinjectbundled
        GIT_REPOSITORY https://github.com/jojoe77777/EasyInjectBundled.git
        GIT_TAG 2d53697315ff0e122c176d19c23d847cc4307c4f
    )
    FetchContent_GetProperties(easyinjectbundled)
    if(NOT easyinjectbundled_POPULATED)
        FetchContent_Populate(easyinjectbundled)
    endif()

    set(JAR_NAME "Toolscreen-${PROJECT_VERSION}-double-click-me.jar")
    set(JAR_OUTPUT "${CMAKE_BINARY_DIR}/${JAR_NAME}")

    add_custom_command(
        OUTPUT "${JAR_OUTPUT}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${easyinjectbundled_SOURCE_DIR}/custom-dlls"
        COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:${PROJECT_NAME}>" "${easyinjectbundled_SOURCE_DIR}/custom-dlls/Toolscreen.dll"
        COMMAND "${maven_SOURCE_DIR}/bin/mvn.cmd" -f "${easyinjectbundled_SOURCE_DIR}/pom.xml" clean package -q -Dbrand.name=Toolscreen -Dbrand.version=${PROJECT_VERSION}
        COMMAND ${CMAKE_COMMAND} -E copy "${easyinjectbundled_SOURCE_DIR}/target/${JAR_NAME}" "${JAR_OUTPUT}"
        DEPENDS ${PROJECT_NAME}
        COMMENT "Building ${JAR_NAME}"
        VERBATIM
    )

    add_custom_target(jar ALL DEPENDS "${JAR_OUTPUT}")
    install(FILES "${JAR_OUTPUT}" DESTINATION "${CMAKE_CURRENT_SOURCE_DIR}/..")
    message(STATUS "JAR packaging enabled: ${JAR_NAME}")
else()
    message(WARNING "Java not found - JAR packaging disabled.")
endif()