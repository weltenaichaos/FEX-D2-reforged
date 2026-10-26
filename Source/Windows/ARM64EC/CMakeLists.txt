add_library(arm64ecfex SHARED
  CodeBuffer.cpp
  FrameLatency.cpp
  GameFiles.cpp
  LoaderWatch.cpp
  Module.cpp
  Module.S
  NativeMD5.c
  NativeRedirect.cpp
  NativeUnpackCache.cpp
  NativeZlib.c
  Rumble.cpp
  Vkd3dReport.cpp
  TurnipReport.cpp
  libarm64ecfex.def
  $<TARGET_OBJECTS:FEXCore_object>)

patch_library_wine(arm64ecfex)

target_include_directories(arm64ecfex PRIVATE
  "${CMAKE_SOURCE_DIR}/Source/Windows/include/"
  "${CMAKE_SOURCE_DIR}/Source/"
  "${CMAKE_SOURCE_DIR}/Source/Windows/")

target_link_libraries(arm64ecfex PRIVATE
  FEXCore_Base
  xxHash::xxhash
  Common
  CommonTools
  CommonWindows
  CommonWindowsRuntime
  ntdll_ex)

target_link_options(arm64ecfex PRIVATE -static -nostdlib -nostartfiles -nodefaultlibs -lc++ -lc++abi -lunwind)
target_link_libraries(arm64ecfex PRIVATE ${LIBGCC_PATH})
install(TARGETS arm64ecfex RUNTIME
  DESTINATION ${CMAKE_INSTALL_LIBDIR}
  COMPONENT Runtime)
