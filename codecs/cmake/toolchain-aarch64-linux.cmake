# Cross-compile p/ Linux aarch64 (ARM64). Testavel via qemu-aarch64-static.
# Requer no host (WSL/Ubuntu):
#   sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu qemu-user-static
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER   aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# So procura libs/headers no sysroot do alvo; programas (nasm etc.) no host.
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Roda o binario aarch64 sob QEMU. O -L aponta o sysroot do cross (glibc +
# ld-linux-aarch64.so.1), senao o loader dinamico nao e encontrado.
set(CMAKE_CROSSCOMPILING_EMULATOR qemu-aarch64-static -L /usr/aarch64-linux-gnu)
