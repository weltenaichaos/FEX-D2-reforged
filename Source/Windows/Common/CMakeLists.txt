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
  Switches.cpp
  ThreadPlacer.cpp
  PriorityGuard.cpp
  LinuxSys.cpp
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

# CI's build number and description go into the stats log's file name and header. Only StatsLog.cpp sees them, so a new
# number recompiles that one file.
if (DEFINED ENV{FEX_BUILD_NUMBER})
  set_property(SOURCE StatsLog.cpp APPEND PROPERTY COMPILE_DEFINITIONS "FEX_BUILD_NUMBER=\"$ENV{FEX_BUILD_NUMBER}\"")
endif()
if (DEFINED ENV{FEX_BUILD_INFO})
  set_property(SOURCE StatsLog.cpp APPEND PROPERTY COMPILE_DEFINITIONS "FEX_BUILD_INFO=\"$ENV{FEX_BUILD_INFO}\"")
endif()
