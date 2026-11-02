cmake_minimum_required(VERSION 3.20)
project(PoltergeistLauncher CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX UNICODE _UNICODE)
if(MSVC)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  add_compile_options(/utf-8 /O2)
endif()
if(MINGW)
  add_compile_options(-O2)
endif()

include(FetchContent)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)

FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui.git GIT_TAG v1.90.9 GIT_SHALLOW TRUE)
FetchContent_Declare(json  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_Declare(stb   GIT_REPOSITORY https://github.com/nothings/stb.git GIT_TAG master GIT_SHALLOW TRUE)
FetchContent_Declare(miniz GIT_REPOSITORY https://github.com/richgel999/miniz.git GIT_TAG 3.0.2 GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(imgui json stb miniz)

add_executable(PoltergeistLauncher WIN32
  main.cpp
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_dx11.cpp)

target_include_directories(PoltergeistLauncher PRIVATE
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${stb_SOURCE_DIR})

target_link_libraries(PoltergeistLauncher PRIVATE
  nlohmann_json::nlohmann_json miniz
  d3d11 dxgi d3dcompiler winhttp comdlg32 shell32 advapi32 ole32 dwmapi)

if(MINGW)
  target_link_options(PoltergeistLauncher PRIVATE -municode -static)
endif()
