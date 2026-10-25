# CMake toolchain for the camera: static mipsel (Ingenic T20) via zig.
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mips)
set(CMAKE_C_COMPILER zig cc -target mipsel-linux-musleabihf -mcpu=mips32r2)
set(CMAKE_CXX_COMPILER zig c++ -target mipsel-linux-musleabihf -mcpu=mips32r2)
set(CMAKE_AR ${CMAKE_CURRENT_LIST_DIR}/zig-ar)
set(CMAKE_RANLIB ${CMAKE_CURRENT_LIST_DIR}/zig-ranlib)
