# CMake platform module for IRIX 6.5 native builds: uname -s says IRIX64 on
# 64-bit kernels, and CMake 4 ships no IRIX platform module at all.  Without
# one CMake falls back to generic defaults and never sets UNIX.  Settings
# follow the IRIX module our LLVM tree uses for cross builds.  Load it with
# -DCMAKE_MODULE_PATH=<this directory's parent> (configure-native.sh does).
set(CMAKE_DL_LIBS "dl")
set(CMAKE_SHARED_LIBRARY_PREFIX "lib")
set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG "-Wl,-rpath,")
set(CMAKE_SHARED_LIBRARY_RUNTIME_C_FLAG_SEP ":")
set(CMAKE_SHARED_LIBRARY_RPATH_LINK_C_FLAG "-Wl,-rpath-link,")
set(CMAKE_SHARED_LIBRARY_SONAME_C_FLAG "-Wl,-soname,")
set(CMAKE_EXE_EXPORTS_C_FLAG "-Wl,--export-dynamic")
set(CMAKE_PLATFORM_USES_PATH_WHEN_NO_SONAME 1)
foreach(type SHARED_LIBRARY SHARED_MODULE EXE)
  set(CMAKE_${type}_LINK_STATIC_C_FLAGS "-Wl,-Bstatic")
  set(CMAKE_${type}_LINK_DYNAMIC_C_FLAGS "-Wl,-Bdynamic")
endforeach()
include(Platform/UnixPaths)
