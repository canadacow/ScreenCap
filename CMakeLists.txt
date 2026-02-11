cmake_minimum_required(VERSION 3.24)

project(ScreenCap LANGUAGES C CXX)

if(NOT WIN32)
  message(FATAL_ERROR "ScreenCap is Windows-only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── nativefiledialog (third-party, static) ───────────────────────────

add_library(nfd STATIC
  third-party/nativefiledialog/src/nfd_common.c
  third-party/nativefiledialog/src/nfd_win.cpp
)

target_include_directories(nfd PUBLIC
  third-party/nativefiledialog/src/include
)

target_link_libraries(nfd PRIVATE
  ole32
  uuid
  shell32
)

if(MSVC)
  # nfd is C / old-style C++; suppress warnings we don't own.
  target_compile_options(nfd PRIVATE /W0)
endif()

# ── ScreenCap ────────────────────────────────────────────────────────

add_executable(ScreenCap
  WIN32
  src/app.rc
  src/winmain.cpp
  src/win/ComInit.h
  src/win/ComInit.cpp
  src/win/TrayIcon.h
  src/win/TrayIcon.cpp
  src/win/TrayWindow.h
  src/win/TrayWindow.cpp
  src/capture/FrameData.h
  src/capture/FrameData.cpp
  src/capture/PixelFormats.h
  src/capture/ConvertShader.h
  src/capture/DesktopDuplicator.h
  src/capture/DesktopDuplicator.cpp
  src/capture/SaveImage.h
  src/capture/SaveImage.cpp
  src/capture/WindowCapture.h
  src/capture/WindowCapture.cpp
  src/preview/Shaders.h
  src/preview/PreviewWindow.h
  src/preview/PreviewWindow.cpp
)

target_include_directories(ScreenCap PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(ScreenCap PRIVATE
  UNICODE
  _UNICODE
  WIN32_LEAN_AND_MEAN
  NOMINMAX
)

if(MSVC)
  target_compile_options(ScreenCap PRIVATE
    /W4
    /permissive-
    /EHsc
    /utf-8
  )
endif()

target_link_libraries(ScreenCap PRIVATE
  nfd
  d2d1
  d3d11
  d3dcompiler
  dwmapi
  dwrite
  dxgi
  dxguid
  ole32
  shell32
  user32
  windowsapp
  windowscodecs
)
