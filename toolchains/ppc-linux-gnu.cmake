set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR powerpc)

set(CROSS_COMPILE powerpc-linux-gnu)

# Prefer a versioned gcc-15 if present (upstream default), else whatever the distro provides.
find_program(PPC_GCC NAMES ${CROSS_COMPILE}-gcc-15 ${CROSS_COMPILE}-gcc-14 ${CROSS_COMPILE}-gcc-13 ${CROSS_COMPILE}-gcc)
find_program(PPC_GXX NAMES ${CROSS_COMPILE}-g++-15 ${CROSS_COMPILE}-g++-14 ${CROSS_COMPILE}-g++-13 ${CROSS_COMPILE}-g++)
set(CMAKE_C_COMPILER ${PPC_GCC})
set(CMAKE_CXX_COMPILER ${PPC_GXX})
set(CMAKE_AR ${CROSS_COMPILE}-ar)
set(CMAKE_RANLIB ${CROSS_COMPILE}-ranlib)
set(CMAKE_OBJCOPY ${CROSS_COMPILE}-objcopy)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
