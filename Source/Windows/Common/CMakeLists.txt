add_library(CommonWindowsRuntime STATIC LoadConfig.S)
add_subdirectory(CRT)
add_subdirectory(WinAPI)

target_link_libraries(CommonWindowsRuntime FEXCore_Base JemallocLibs)
target_compile_options(CommonWindowsRuntime PRIVATE -Wno-inconsistent-dllimport)
target_include_directories(CommonWindowsRuntime PRIVATE "${CMAKE_SOURCE_DIR}/Source/Windows/include/")

add_library(CommonWindows STATIC
  Allocator.cpp
  CPUFeatures.cpp
  EnvironmentVariablesHandling.cpp
  FEXUnixLib.cpp
  SHMStats.cpp
  StatsLog.cpp
  InvalidationTracker.cpp
  ImageTracker.cpp
  Logging.cpp
  Threads.cpp)

target_link_libraries(CommonWindows FEXCore_Base)

# The opt-in FEX_STATSLOG logger. With it off, FEX_STATSLOG* are ignored and nothing of the logger runs.
option(ENABLE_STATSLOG "Build the FEX_STATSLOG per-second stats log" TRUE)
if (NOT ENABLE_STATSLOG)
  target_compile_definitions(CommonWindows PRIVATE FEX_DISABLE_STATSLOG)
endif()
target_include_directories(CommonWindows PRIVATE "${CMAKE_SOURCE_DIR}/Source/Windows/include/")
