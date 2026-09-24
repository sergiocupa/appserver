# Build multiplataforma dos codecs (CMake)

`CMakeLists.txt` gera Opus + VP9 + AV1 + H265 para **Windows|Linux × x64|ARM64**.
Cada codec escolhe SSE2/AVX2 (x86) ou NEON (arm64) **pela arquitetura do alvo** —
não há lista de fontes chumbada por arch.

- Opus / SVT-AV1 / x265 → `add_subdirectory` (CMake próprio deles)
- libvpx → `ExternalProject` roda o `./configure --target=<os-arch>` dele

> Windows-x64 de **produção** continua no `codecs.vcxproj` (MSVC), que já funciona.
> Este CMake é o caminho **Linux** (x64 nativo; ARM64 via cross + QEMU).

## Pré-requisitos (WSL / Ubuntu)

```bash
sudo apt update
sudo apt install -y cmake ninja-build build-essential nasm \
     gcc-aarch64-linux-gnu g++-aarch64-linux-gnu qemu-user-static
```

## Linux x64 (nativo)

```bash
cmake -S codecs -B build/linux-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/linux-x64 -j
./build/linux-x64/codecs_smoke        # deve imprimir "RESULTADO: OK"
```

## Linux ARM64 (cross-compile + teste no emulador QEMU)

```bash
cmake -S codecs -B build/linux-arm64 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/codecs/cmake/toolchain-aarch64-linux.cmake
cmake --build build/linux-arm64 -j
# -L aponta o sysroot do cross (glibc aarch64 + loader dinamico):
qemu-aarch64-static -L /usr/aarch64-linux-gnu ./build/linux-arm64/codecs_smoke
```

O `arch=arm64` impresso pelo smoke prova que é binário aarch64 rodando emulado.

### Validado (WSL Ubuntu 24.04, host x86)

```
x64  nativo : arch=x64   opus/vp9/av1/x265 OK   RESULTADO: OK
arm64 QEMU  : arch=arm64  opus/vp9/av1/x265 OK   RESULTADO: OK
```

Build ARM64 = ELF `ARM aarch64` (libvpx compilado com NEON).

## Notas

- **ENABLE_ASSEMBLY** do x265 e o asm do libvpx x64 exigem `nasm`. No 1º momento
  o x265 vem com assembly **OFF** (C++ puro) para simplificar; ligue depois.
- **QEMU × Visual Studio**: o QEMU-user é fluxo de CLI/WSL; o VS **não** dirige a
  emulação aarch64. Para debug de IDE do binário ARM emulado, use VS Code +
  `gdb-multiarch` anexando em `qemu-aarch64 -g <porta>`.
- VS/VS Code abrem esta pasta como projeto CMake e configuram normalmente; o que
  não passa pelo VS é a **execução** do binário ARM emulado.
