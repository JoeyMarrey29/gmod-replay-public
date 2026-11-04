cmake_minimum_required(VERSION 3.25)

project(gmod_replay_application LANGUAGES C CXX RC)

# private: JoeyMarrey29/gmod-replay (collaborators only). public: JoeyMarrey29/gmod-replay-public.
set(GMOD_REPLAY_EDITION private CACHE STRING "Which edition to build: private or public")
set_property(CACHE GMOD_REPLAY_EDITION PROPERTY STRINGS private public)
if(GMOD_REPLAY_EDITION STREQUAL "public")
    add_compile_definitions(GMOD_REPLAY_PUBLIC=1)
    set(REPLAY_NOTES_FOLDER releases-public)
elseif(GMOD_REPLAY_EDITION STREQUAL "private")
    set(REPLAY_NOTES_FOLDER releases)
else()
    message(FATAL_ERROR "GMOD_REPLAY_EDITION must be private or public")
endif()

add_executable(gmod-replay-menu-settings-tests EXCLUDE_FROM_ALL
    tests/MenuSettingsTests.cpp src/ui/EscMenu.cpp src/ui/Settings.cpp)
target_compile_features(gmod-replay-menu-settings-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-menu-settings-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-menu-settings-tests PRIVATE include)
target_link_libraries(gmod-replay-menu-settings-tests PRIVATE user32 gdi32)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" REPLAY_VERSION)
string(STRIP "${REPLAY_VERSION}" REPLAY_VERSION)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION")
file(GLOB REPLAY_RELEASE_NOTES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/../${REPLAY_NOTES_FOLDER}/*.md")
list(SORT REPLAY_RELEASE_NOTES COMPARE NATURAL ORDER DESCENDING)
set(REPLAY_NOTES_HEADER "#pragma once\n#include <array>\n#include <string_view>\nnamespace gmod::ui::release_data {\ninline constexpr std::string_view version = \"${REPLAY_VERSION}\";\ninline constexpr std::array markdown = {\n")
foreach(NOTES_FILE IN LISTS REPLAY_RELEASE_NOTES)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${NOTES_FILE}")
    file(READ "${NOTES_FILE}" NOTES_TEXT)
    string(APPEND REPLAY_NOTES_HEADER "std::string_view{R\"REPLAY_NOTES(${NOTES_TEXT})REPLAY_NOTES\"},\n")
endforeach()
string(APPEND REPLAY_NOTES_HEADER "};\n}\n")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/ReleaseNotesData.h" "${REPLAY_NOTES_HEADER}")
set(DEMO_CACHE_REVISION "${CMAKE_CXX_COMPILER_ID}:${CMAKE_CXX_COMPILER_VERSION}:${CMAKE_SIZEOF_VOID_P}")
foreach(CACHE_SOURCE IN ITEMS src/demo/DemoCache.cpp src/demo/DemoParser.cpp src/demo/SourceDemoReader.cpp
        include/demo/DemoData.h include/demo/DemoCache.h include/demo/SourceDemoReader.h include/demo/Perpheads.h)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${CACHE_SOURCE}")
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${CACHE_SOURCE}" CACHE_SOURCE_HASH)
    string(APPEND DEMO_CACHE_REVISION "${CACHE_SOURCE_HASH}")
endforeach()
string(SHA256 DEMO_CACHE_REVISION "${DEMO_CACHE_REVISION}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/DemoCacheVersion.h"
    "#pragma once\ninline constexpr char demo_cache_revision[] = \"${DEMO_CACHE_REVISION}\";\n")
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/assets/windows/GModReplay.ico" REPLAY_ICON_PATH)
configure_file(
    src/app/GModReplay.rc.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/GModReplay.rc"
    @ONLY
)

set(BUILD_XAUDIO_WIN10 OFF CACHE BOOL "" FORCE)
set(BUILD_WGI OFF CACHE BOOL "" FORCE)
set(BUILD_XINPUT OFF CACHE BOOL "" FORCE)
set(BUILD_DXIL_SHADERS OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/directxtk12)

add_executable(gmod-replay-application WIN32
    "${CMAKE_CURRENT_BINARY_DIR}/generated/GModReplay.rc"
    src/app/main.cpp
    src/app/ViewingPolicy.cpp
    src/audio/AudioSystem.cpp
    src/audio/SoundCatalog.cpp
    src/dx12/FrameTextOverlay.cpp
    src/demo/DemoParser.cpp
    src/demo/DemoCache.cpp
    src/demo/Outfit.cpp
    src/demo/SourceDemoReader.cpp
    src/ui/MainMenu.cpp
    src/ui/ReleaseNotes.cpp
    src/ui/Updates.cpp
    src/ui/NameTags.cpp
    src/ui/RadioHud.cpp
    src/ui/DialogHud.cpp
    src/ui/EscMenu.cpp
    src/ui/GameConsole.cpp
    src/ui/Hud.cpp
    src/ui/Settings.cpp
    src/ui/PlayerCard.cpp
    src/ui/PlayerListPanel.cpp
    src/ui/TimelineBar.cpp
    src/source/Vfs.cpp
    src/source/GameContent.cpp
    src/source/bsp/BspFile.cpp
    src/source/bsp/BspReader.cpp
    src/source/bsp/StaticPropsReader.cpp
    src/source/AssetResolver.cpp
    src/source/Ground.cpp
    src/source/WorldTrace.cpp
    src/source/model/VvdReader.cpp
    src/source/model/Animation.cpp
    src/source/model/PlayerAnimator.cpp
    src/source/model/StaticPropMeshImporter.cpp
    src/source/vmt/VmtReader.cpp
    src/source/vtf/VtfReader.cpp
    src/source/png/PngReader.cpp
    third_party/lzma/LzmaDec.c
)

add_custom_target(gmod-replay-menu-assets
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:gmod-replay-application>/assets/ui"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/ui/menu_background.png"
        "$<TARGET_FILE_DIR:gmod-replay-application>/assets/ui/menu_background.png"
    VERBATIM
)
add_dependencies(gmod-replay-application gmod-replay-menu-assets)

target_compile_features(gmod-replay-application PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-application PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)
target_include_directories(gmod-replay-application PRIVATE
    include
    src/app
    "${CMAKE_CURRENT_BINARY_DIR}/generated"
    third_party/directxtk12/Inc
    third_party/lzma
)
target_link_libraries(gmod-replay-application PRIVATE
    d3d12
    dxgi
    d3dcompiler
    shell32
    shlwapi
    user32
    ole32
    windowscodecs
    gdi32
    xaudio2
    bcrypt
    crypt32
    winhttp
    mfplat
    mfreadwrite
    mfuuid
    DirectXTK12
)

add_executable(gmod-replay-release-notes-tests EXCLUDE_FROM_ALL
    tests/ReleaseNotesTests.cpp
    src/ui/NameTags.cpp
    src/ui/EscMenu.cpp
    src/ui/ReleaseNotes.cpp
    src/ui/Updates.cpp
    src/ui/MainMenu.cpp
    src/ui/Settings.cpp
    src/demo/DemoCache.cpp
    src/source/GameContent.cpp
    src/source/Vfs.cpp
    src/source/png/PngReader.cpp
    third_party/lzma/LzmaDec.c
)
target_compile_features(gmod-replay-release-notes-tests PRIVATE cxx_std_20)
target_include_directories(gmod-replay-release-notes-tests PRIVATE
    include third_party/lzma "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_compile_definitions(gmod-replay-release-notes-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(gmod-replay-release-notes-tests PRIVATE REPLAY_SOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(gmod-replay-release-notes-tests PRIVATE shell32 ole32 user32 gdi32 windowscodecs bcrypt)

add_executable(gmod-replay-combat-tests EXCLUDE_FROM_ALL
    tests/CombatPlaybackTests.cpp
    src/demo/DemoParser.cpp
    src/demo/Outfit.cpp
    src/demo/SourceDemoReader.cpp
    src/source/WorldTrace.cpp
)
target_compile_features(gmod-replay-combat-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-combat-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-combat-tests PRIVATE include)

add_executable(gmod-replay-bsp-geometry-tests EXCLUDE_FROM_ALL tests/BspModelGeometryTests.cpp)
add_executable(gmod-replay-bank-baked-tests EXCLUDE_FROM_ALL
    tests/BankBakedLightingTests.cpp src/source/bsp/BspReader.cpp src/source/bsp/BspFile.cpp
    src/source/Vfs.cpp third_party/lzma/LzmaDec.c
    src/demo/DemoParser.cpp src/demo/Outfit.cpp src/demo/SourceDemoReader.cpp)
target_compile_features(gmod-replay-bank-baked-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-bank-baked-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-bank-baked-tests PRIVATE include third_party/lzma)
add_executable(gmod-replay-frame-text-tests EXCLUDE_FROM_ALL
    tests/FrameTextTests.cpp src/dx12/FrameTextOverlay.cpp src/ui/NameTags.cpp)
target_compile_features(gmod-replay-frame-text-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-frame-text-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-frame-text-tests PRIVATE include)
target_link_libraries(gmod-replay-frame-text-tests PRIVATE d3d12 dxgi d3dcompiler user32 gdi32)
add_executable(gmod-replay-npc-gui-tests EXCLUDE_FROM_ALL
    tests/NpcGuiTests.cpp src/demo/DemoParser.cpp src/demo/Outfit.cpp src/demo/SourceDemoReader.cpp)
target_compile_features(gmod-replay-npc-gui-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-npc-gui-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-npc-gui-tests PRIVATE include)
target_compile_features(gmod-replay-bsp-geometry-tests PRIVATE cxx_std_20)
target_include_directories(gmod-replay-bsp-geometry-tests PRIVATE include)

add_executable(gmod-replay-audio-tests EXCLUDE_FROM_ALL
    tests/AudioPlaybackTests.cpp
    src/audio/AudioSystem.cpp
    src/source/Vfs.cpp
    third_party/lzma/LzmaDec.c
)
target_compile_features(gmod-replay-audio-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-audio-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-audio-tests PRIVATE include third_party/lzma)
target_link_libraries(gmod-replay-audio-tests PRIVATE xaudio2 mfplat mfreadwrite mfuuid ole32 shlwapi)

add_executable(gmod-replay-gpu-cache-tests EXCLUDE_FROM_ALL
    tests/GpuAssetCacheTests.cpp
    src/ui/Settings.cpp
    src/demo/DemoCache.cpp
)
target_compile_features(gmod-replay-gpu-cache-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-gpu-cache-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-gpu-cache-tests PRIVATE include "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(gmod-replay-gpu-cache-tests PRIVATE d3d12 dxgi bcrypt)

add_executable(gmod-replay-animation-tests EXCLUDE_FROM_ALL
    tests/PlayerAnimationTests.cpp
    src/source/model/Animation.cpp
    src/source/model/PlayerAnimator.cpp
    src/source/Vfs.cpp
    third_party/lzma/LzmaDec.c
)

add_executable(gmod-replay-model-pose-tests EXCLUDE_FROM_ALL
    tests/ModelPoseTests.cpp
    src/source/model/StaticPropMeshImporter.cpp
    src/source/model/VvdReader.cpp
    src/source/model/Animation.cpp
    src/source/Vfs.cpp
    third_party/lzma/LzmaDec.c
)
target_compile_features(gmod-replay-model-pose-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-model-pose-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-model-pose-tests PRIVATE include third_party/lzma)
target_compile_features(gmod-replay-animation-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-animation-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-animation-tests PRIVATE include third_party/lzma)

add_executable(gmod-replay-viewing-policy-tests EXCLUDE_FROM_ALL
    tests/ViewingPolicyTests.cpp
    src/app/ViewingPolicy.cpp
)
target_compile_features(gmod-replay-viewing-policy-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-viewing-policy-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-viewing-policy-tests PRIVATE include)
target_link_libraries(gmod-replay-viewing-policy-tests PRIVATE crypt32 winhttp shell32 ole32)

add_executable(gmod-replay-source-demo-tests EXCLUDE_FROM_ALL
    tests/SourceDemoReaderTests.cpp
    src/demo/SourceDemoReader.cpp
)
target_compile_features(gmod-replay-source-demo-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-source-demo-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-source-demo-tests PRIVATE include)

add_executable(gmod-replay-content-tests EXCLUDE_FROM_ALL
    tests/GameContentTests.cpp
    src/source/GameContent.cpp
    src/source/Vfs.cpp
    third_party/lzma/LzmaDec.c
)
target_compile_features(gmod-replay-content-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-content-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-content-tests PRIVATE include third_party/lzma)

add_executable(gmod-replay-vtf-tests EXCLUDE_FROM_ALL
    tests/VtfReaderTests.cpp
    src/source/vmt/VmtReader.cpp
    src/source/vtf/VtfReader.cpp
    src/source/Vfs.cpp
    third_party/lzma/LzmaDec.c
)
target_compile_features(gmod-replay-vtf-tests PRIVATE cxx_std_20)
target_compile_definitions(gmod-replay-vtf-tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(gmod-replay-vtf-tests PRIVATE include third_party/lzma)
