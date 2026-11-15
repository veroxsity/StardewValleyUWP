cmake_minimum_required(VERSION 3.27)

project(StardewValleyUWP VERSION 0.1.20 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(STARDEW_UWP_PACKAGE_STAGE "" CACHE PATH "Package half made by packaging/stage-package.ps1")
set(STARDEW_UWP_NATIVE_BUILD "" CACHE PATH "Build folder given to packaging/build-native.ps1")
set(STARDEW_UWP_D3DCOMPILER "C:/Program Files (x86)/Windows Kits/10/Redist/D3D/x64/d3dcompiler_47.dll" CACHE FILEPATH "D3D compiler required by FNA3D")

add_executable(stardew_uwp_host_desktop
  src/native/desktop_main.cpp
  src/native/runtime_host.cpp
  src/native/runtime_host.h
)

target_include_directories(stardew_uwp_host_desktop PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(stardew_uwp_host_desktop PRIVATE kernel32)

file(GLOB miniz_sources "${CMAKE_SOURCE_DIR}/third_party/miniz/*.c")
file(GLOB qrcodegen_sources "${CMAKE_SOURCE_DIR}/third_party/qrcodegen/*.c")

add_library(stardew_uwp_miniz STATIC ${miniz_sources})
target_include_directories(stardew_uwp_miniz PUBLIC "${CMAKE_SOURCE_DIR}")

# WinHTTP and CreateFileW sit in the desktop api partition, so the network and client code builds with that family
add_library(stardew_uwp_net STATIC
  src/net/http.cpp
  src/net/http.h
  src/text.h
)
target_compile_definitions(stardew_uwp_net PRIVATE WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP _WIN32_WINNT=0x0A00)
target_include_directories(stardew_uwp_net PUBLIC "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(stardew_uwp_net PUBLIC winhttp)

add_library(stardew_uwp_cf STATIC
  src/cf/curseforge.cpp
  src/cf/curseforge.h
  src/cf/image.cpp
  src/cf/image.h
  src/cf/json.cpp
  src/cf/json.h
  src/cf/mods.cpp
  src/cf/mods.h
  src/cf/setup_server.cpp
  src/cf/setup_server.h
  src/text.h
)
target_compile_definitions(stardew_uwp_cf PRIVATE WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP _WIN32_WINNT=0x0A00)
target_include_directories(stardew_uwp_cf PUBLIC "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/cf")
target_link_libraries(stardew_uwp_cf PUBLIC stardew_uwp_net stardew_uwp_miniz)

if(NOT WINDOWS_STORE)
  # desktop harness for the mods tab code
  add_executable(stardew_cf_probe tools/cf-probe/cf_probe.cpp)
  target_link_libraries(stardew_cf_probe PRIVATE stardew_uwp_cf)

  # renders every launcher screen to png, the same drawing code the console runs
  add_executable(stardew_launcher_preview
    tools/ui-preview/launcher_preview.cpp
    src/native/launcher_ui.cpp
    src/native/launcher_ui.h
    src/native/launcher_screens.cpp
    src/native/launcher_screens.h
    ${qrcodegen_sources}
  )
  target_compile_definitions(stardew_launcher_preview PRIVATE UNICODE _UNICODE _WIN32_WINNT=0x0A00)
  target_include_directories(stardew_launcher_preview PRIVATE "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src")
  target_link_libraries(stardew_launcher_preview PRIVATE d2d1 dwrite d3d11 dxgi dxguid windowscodecs ole32)
endif()

# speaks the Steam CM protocol directly because the steamworks sdk cannot run in an app container
file(GLOB steam_sources "${CMAKE_SOURCE_DIR}/src/steam/*.cpp")
file(GLOB zstd_sources "${CMAKE_SOURCE_DIR}/third_party/zstd/*.c")
file(GLOB lzma_sources "${CMAKE_SOURCE_DIR}/third_party/lzma/*.c")
add_library(stardew_uwp_steam STATIC
  ${steam_sources}
  ${zstd_sources}
  ${lzma_sources}
)
target_compile_definitions(stardew_uwp_steam PRIVATE WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP _WIN32_WINNT=0x0A00)
target_include_directories(stardew_uwp_steam PUBLIC "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/steam")
target_link_libraries(stardew_uwp_steam PUBLIC stardew_uwp_net stardew_uwp_miniz winhttp crypt32 bcrypt)


if(WINDOWS_STORE)
  set(STARDEW_UWP_SDL_SOURCE "${STARDEW_UWP_NATIVE_BUILD}/source/SDL2")
  set(STARDEW_UWP_FNA3D_BUILD "${STARDEW_UWP_NATIVE_BUILD}/FNA3D")
  set(STARDEW_UWP_FAUDIO_BUILD "${STARDEW_UWP_NATIVE_BUILD}/FAudio")
  if(NOT EXISTS "${STARDEW_UWP_SDL_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "No SDL2 source at ${STARDEW_UWP_SDL_SOURCE}, set STARDEW_UWP_NATIVE_BUILD to the folder build-native.ps1 wrote")
  endif()

  set(SDL_LIBC ON CACHE BOOL "" FORCE)
  set(SDL_SHARED ON CACHE BOOL "" FORCE)
  set(SDL_STATIC OFF CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  add_subdirectory("${STARDEW_UWP_SDL_SOURCE}" "${CMAKE_BINARY_DIR}/sdl2")

  set(sdl_winrt_main "${STARDEW_UWP_SDL_SOURCE}/src/main/winrt/SDL_winrt_main_NonXAML.cpp")
  set(sdl_winrt_resource "${STARDEW_UWP_SDL_SOURCE}/src/main/winrt/SDL2-WinRTResources.rc")
  set(sdl_winrt_cursor "${STARDEW_UWP_SDL_SOURCE}/src/main/winrt/SDL2-WinRTResource_BlankCursor.cur")
  set(appx_manifest "${CMAKE_SOURCE_DIR}/packaging/Package.appxmanifest")
  set(appx_assets
    "${CMAKE_SOURCE_DIR}/packaging/Assets/SplashScreen.png"
    "${CMAKE_SOURCE_DIR}/packaging/Assets/Square150x150Logo.png"
    "${CMAKE_SOURCE_DIR}/packaging/Assets/Square44x44Logo.png"
    "${CMAKE_SOURCE_DIR}/packaging/Assets/StoreLogo.png"
    "${CMAKE_SOURCE_DIR}/packaging/Assets/Wide310x150Logo.png"
    "${CMAKE_SOURCE_DIR}/packaging/Assets/Backdrop.jpg"
  )

  set(missing_appx_assets "")
  foreach(appx_asset IN LISTS appx_assets)
    if(NOT EXISTS "${appx_asset}")
      string(APPEND missing_appx_assets "\n  ${appx_asset}")
    endif()
  endforeach()
  if(NOT missing_appx_assets STREQUAL "")
    message(FATAL_ERROR "Package.appxmanifest names tile images that do not exist:${missing_appx_assets}")
  endif()

  add_executable(stardew_uwp_host WIN32
    src/native/uwp_main.cpp
    src/native/runtime_host.cpp
    src/native/runtime_host.h
    src/native/host_environment.cpp
    src/native/host_environment.h
    src/native/launcher.cpp
    src/native/launcher.h
    src/native/launcher_ui.cpp
    src/native/launcher_ui.h
    src/native/launcher_screens.cpp
    src/native/launcher_screens.h
    src/native/gpu_present.cpp
    src/native/gpu_present.h
    ${qrcodegen_sources}
    ${sdl_winrt_main}
    ${sdl_winrt_resource}
    ${sdl_winrt_cursor}
    ${appx_manifest}
    ${appx_assets}
  )

  set_source_files_properties(src/native/host_environment.cpp PROPERTIES
    COMPILE_DEFINITIONS "WINAPI_FAMILY=WINAPI_FAMILY_DESKTOP_APP;_WIN32_WINNT=0x0A00")

  set(native_runtime_files
    "${STARDEW_UWP_FNA3D_BUILD}/Release/FNA3D.dll"
    "${STARDEW_UWP_FAUDIO_BUILD}/Release/FAudio.dll"
    "${STARDEW_UWP_D3DCOMPILER}"
  )
  foreach(native_file IN LISTS native_runtime_files)
    if(NOT EXISTS "${native_file}")
      message(FATAL_ERROR "Missing native dependency: ${native_file}")
    endif()
  endforeach()

  target_sources(stardew_uwp_host PRIVATE ${native_runtime_files})
  set_source_files_properties(${native_runtime_files} PROPERTIES VS_DEPLOYMENT_CONTENT 1 VS_DEPLOYMENT_LOCATION ".")

  file(GLOB_RECURSE package_stage_files LIST_DIRECTORIES false "${STARDEW_UWP_PACKAGE_STAGE}/*")
  if(NOT package_stage_files)
    message(FATAL_ERROR "Nothing in STARDEW_UWP_PACKAGE_STAGE, run packaging/stage-package.ps1 and point it at the output")
  endif()

  # the game is downloaded on the console, a package carrying it would be redistributing it
  foreach(game_file IN ITEMS "Stardew Valley.dll" "StardewValley.GameData.dll" "Stardew Valley.deps.json" "StardewModdingAPI.deps.json" "Content")
    if(EXISTS "${STARDEW_UWP_PACKAGE_STAGE}/${game_file}" OR EXISTS "${STARDEW_UWP_PACKAGE_STAGE}/overlay/${game_file}")
      message(FATAL_ERROR "${STARDEW_UWP_PACKAGE_STAGE} carries ${game_file}, a game file")
    endif()
  endforeach()

  target_sources(stardew_uwp_host PRIVATE ${package_stage_files})
  foreach(game_file IN LISTS package_stage_files)
    file(RELATIVE_PATH game_relative "${STARDEW_UWP_PACKAGE_STAGE}" "${game_file}")
    get_filename_component(game_subdirectory "${game_relative}" DIRECTORY)
    if(game_subdirectory STREQUAL "")
      set(game_subdirectory ".")
    endif()
    set_source_files_properties("${game_file}" PROPERTIES VS_DEPLOYMENT_CONTENT 1 VS_DEPLOYMENT_LOCATION "${game_subdirectory}")
  endforeach()

  set_source_files_properties(${sdl_winrt_main} PROPERTIES COMPILE_OPTIONS "/ZW")
  set_source_files_properties(${appx_manifest} PROPERTIES VS_DEPLOYMENT_CONTENT 1)
  set_source_files_properties(${appx_assets} PROPERTIES VS_DEPLOYMENT_CONTENT 1 VS_DEPLOYMENT_LOCATION "Assets")
  target_link_libraries(stardew_uwp_host PRIVATE SDL2::SDL2 kernel32 stardew_uwp_cf stardew_uwp_steam d2d1 dwrite d3d11 dxgi dxguid windowscodecs
    -nodefaultlib:vccorlib$<$<CONFIG:Debug>:d>
    -nodefaultlib:msvcrt$<$<CONFIG:Debug>:d>
    vccorlib$<$<CONFIG:Debug>:d>.lib
    msvcrt$<$<CONFIG:Debug>:d>.lib
  )
  target_compile_definitions(stardew_uwp_host PRIVATE STARDEW_UWP_WINDOWS_STORE)
  set_target_properties(stardew_uwp_host PROPERTIES
    OUTPUT_NAME "StardewValleyUWP"
    VS_GLOBAL_AppxPackageSigningEnabled false
    VS_GLOBAL_AppxGeneratePrisForPortableLibrariesEnabled false
    VS_GLOBAL_RootNamespace "StardewValleyUWP"
    VS_WINDOWS_TARGET_PLATFORM_MIN_VERSION "10.0.17763.0"
  )
endif()
