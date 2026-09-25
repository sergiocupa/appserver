# ============================================================================
#  codecs/CMakeLists.txt
#
#  Build multiplataforma dos codecs (Opus + VP9/libvpx + AV1/SVT + H265/x265).
#  Alvos: Windows|Linux  x  x64|ARM64.
#
#  A ideia central: NAO re-listar fontes por arquitetura. Cada codec ja sabe
#  escolher SSE2/AVX2 (x86) ou NEON (arm) para o alvo:
#    - Opus / SVT-AV1 / x265  -> tem CMake proprio -> add_subdirectory
#    - libvpx                 -> nao tem CMake     -> ExternalProject roda o
#                                ./configure dele com --target=<os-arch>
#
#  Este CMake e para o caminho Linux (x64 e ARM64, ARM64 testavel via QEMU).
#  No Windows-x64 o build de producao continua no codecs.vcxproj (MSVC), que
#  ja funciona; aqui o Windows fica como configuravel/experimental (ver README).
#
#  Uso rapido (Linux x64):
#     cmake -S codecs -B build/linux-x64 -DCMAKE_BUILD_TYPE=Release
#     cmake --build build/linux-x64 -j
#
#  Uso ARM64 (cross + teste no QEMU):
#     cmake -S codecs -B build/linux-arm64 -DCMAKE_BUILD_TYPE=Release \
#           -DCMAKE_TOOLCHAIN_FILE=codecs/cmake/toolchain-aarch64-linux.cmake
#     cmake --build build/linux-arm64 -j
#     qemu-aarch64-static build/linux-arm64/codecs_smoke
# ============================================================================
cmake_minimum_required(VERSION 3.16)

# option() em subdiretorio deve honrar variaveis normais que setamos aqui.
if(POLICY CMP0077)
  cmake_policy(SET CMP0077 NEW)
endif()

project(codecs C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- Detecta arquitetura do ALVO -------------------------------------------
set(_arch "${CMAKE_SYSTEM_PROCESSOR}")
string(TOLOWER "${_arch}" _arch)
if(_arch MATCHES "aarch64|arm64")
  set(CODECS_ARCH "arm64")
elseif(_arch MATCHES "x86_64|amd64")
  set(CODECS_ARCH "x64")
else()
  message(FATAL_ERROR "Arquitetura nao suportada: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
message(STATUS "codecs: OS=${CMAKE_SYSTEM_NAME} ARCH=${CODECS_ARCH}")

# As deps (opus, svtav1, x265, vpx) moraram em MediaFragmenter/deps/src; hoje ficam aqui
# mesmo, em codecs/. O caminho antigo nao existe mais e o configure falhava.
set(DEPS "${CMAKE_CURRENT_SOURCE_DIR}")

# ============================================================================
#  Otimizacao ISOLADA nos codecs
# ============================================================================
#  Aqui o Debug compila otimizado. Vale so para ESTE diretorio e os que ele inclui
#  (opus, svtav1, x265): set() em CMake nao volta para o escopo do pai, entao o resto
#  do projeto continua compilando como sempre.
#
#  Por que: nos codecs a falta de otimizacao muda o RESULTADO de uma medicao, nao so o
#  tempo de espera. E o libvpx nem escolha tinha -- ele nao tem CMake, entra por
#  ExternalProject rodando o ./configure dele, que ignora o CMAKE_BUILD_TYPE e sai
#  sempre otimizado (CONFIG_DEBUG 0, com AVX2). O resultado era um build onde um codec
#  estava otimizado e os outros nao, e a comparacao entre eles nao valia nada.
#
#  O -O3 vem depois de CMAKE_C_FLAGS_DEBUG na linha de comando, entao ganha do -O0 que
#  o gcc assume quando so ha -g. O -g continua, entao o simbolo de depuracao fica.
#
#  O preco: depurar DENTRO de um codec fica pior (inline, variaveis otimizadas).
if(NOT MSVC)
  string(APPEND CMAKE_C_FLAGS_DEBUG   " -O3")
  string(APPEND CMAKE_CXX_FLAGS_DEBUG " -O3")
  if(NOT CMAKE_BUILD_TYPE)
    # Sem build type nenhum o gcc tambem cai em -O0, e nem as flags _DEBUG sao usadas.
    string(APPEND CMAKE_C_FLAGS   " -O3")
    string(APPEND CMAKE_CXX_FLAGS " -O3")
  endif()
endif()

# ============================================================================
#  1) Opus  (C portavel; escolhe intrinsecos pelo alvo)
# ============================================================================
set(OPUS_BUILD_SHARED_LIBRARY        OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS              OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING               OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE   OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/opus" "${CMAKE_BINARY_DIR}/_opus" EXCLUDE_FROM_ALL)

# ============================================================================
#  2) SVT-AV1  (encoder; CMake proprio tem ASM_NEON p/ arm64)
# ============================================================================
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_APPS        OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING     OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/svtav1" "${CMAKE_BINARY_DIR}/_svtav1" EXCLUDE_FROM_ALL)

# ============================================================================
#  3) x265  (HEVC; CMake proprio tem ENABLE_NEON p/ aarch64)
#     ENABLE_ASSEMBLY OFF no 1o momento: evita depender de nasm(x86)/gas(arm).
#     Para maxima performance, ligar depois com o assembler instalado.
# ============================================================================
set(ENABLE_SHARED   OFF CACHE BOOL "" FORCE)
set(ENABLE_CLI      OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS    OFF CACHE BOOL "" FORCE)
set(ENABLE_ASSEMBLY OFF CACHE BOOL "" FORCE)
add_subdirectory("${DEPS}/x265/source" "${CMAKE_BINARY_DIR}/_x265" EXCLUDE_FROM_ALL)

# ============================================================================
#  4) libvpx (VP9)  -- sem CMake -> ExternalProject roda o ./configure dele.
#     O configure escolhe as fontes NEON (arm64) ou SSE2/AVX2 (x64) sozinho.
#     x64 precisa de nasm; arm64 usa intrinsecos NEON (sem assembler externo).
# ============================================================================
include(ExternalProject)

if(CODECS_ARCH STREQUAL "arm64")
  set(_vpx_target "arm64-linux-gcc")
  set(_vpx_extra  "--enable-neon")
else()
  set(_vpx_target "x86_64-linux-gcc")
  set(_vpx_extra  "--as=nasm")
endif()

# Prefixo de cross-compile (ex.: aarch64-linux-gnu-), derivado do compilador.
set(_vpx_cross "")
if(CMAKE_C_COMPILER_TARGET OR CMAKE_CROSSCOMPILING)
  get_filename_component(_cc "${CMAKE_C_COMPILER}" NAME)
  string(REGEX REPLACE "gcc$" "" _vpx_cross "${_cc}")   # aarch64-linux-gnu-gcc -> aarch64-linux-gnu-
endif()

set(_vpx_src     "${DEPS}/vpx")
set(_vpx_prefix  "${CMAKE_BINARY_DIR}/_vpx")
set(_vpx_work    "${_vpx_prefix}/vpxsrc")
set(_vpx_lib     "${_vpx_prefix}/lib/libvpx.a")

if(UNIX)
  # O source vendorizado tem vpx_config.h/asm, *_rtcd.h e config.mk gerados
  # para o build WINDOWS (usados pelo codecs.vcxproj) — nao podem ser apagados.
  # libvpx recusa configure out-of-tree se achar vpx_config.h no source.
  # Solucao: copiar o source p/ uma pasta de trabalho e remover SO ali os
  # arquivos gerados; configurar/buildar na copia (in-tree, modo normal).
  ExternalProject_Add(vpx_ext
    SOURCE_DIR        "${_vpx_work}"
    PREFIX            "${_vpx_prefix}"
    INSTALL_DIR       "${_vpx_prefix}"
    DOWNLOAD_COMMAND  bash -c "mkdir -p '${_vpx_work}' && cp -a '${_vpx_src}/.' '${_vpx_work}/' && rm -f '${_vpx_work}/vpx_config.h' '${_vpx_work}/vpx_config.asm' '${_vpx_work}/config.mk' '${_vpx_work}/vpx_version.h' '${_vpx_work}'/*_rtcd.h"
    CONFIGURE_COMMAND bash -c "cd '${_vpx_work}' && CROSS='${_vpx_cross}' ./configure --target=${_vpx_target} --disable-vp8 --enable-vp9 --disable-examples --disable-tools --disable-docs --disable-unit-tests --enable-static --disable-shared --enable-pic ${_vpx_extra} --prefix='${_vpx_prefix}'"
    BUILD_COMMAND     bash -c "cd '${_vpx_work}' && make -j"
    INSTALL_COMMAND   bash -c "cd '${_vpx_work}' && make install"
    BUILD_BYPRODUCTS  "${_vpx_lib}"
    BUILD_IN_SOURCE   0
  )
  add_library(vpx STATIC IMPORTED GLOBAL)
  set_target_properties(vpx PROPERTIES IMPORTED_LOCATION "${_vpx_lib}")
  file(MAKE_DIRECTORY "${_vpx_prefix}/include")
  set_target_properties(vpx PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_vpx_prefix}/include")
  add_dependencies(vpx vpx_ext)
else()
  message(WARNING "libvpx via configure so no caminho UNIX. "
                  "No Windows use o codecs.vcxproj (MSVC) para VP9.")
endif()

# ============================================================================
#  Alvo agregado 'codecs' -- o app linka isto.
# ============================================================================
add_library(codecs INTERFACE)
target_link_libraries(codecs INTERFACE opus SvtAv1Enc x265-static)
target_include_directories(codecs INTERFACE
  "${DEPS}/opus/include"
  "${DEPS}/svtav1/Source/API"
  "${DEPS}/x265/source"
  "${CMAKE_BINARY_DIR}/_x265")   # x265_config.h e gerado aqui pelo CMake do x265
if(UNIX)
  target_link_libraries(codecs INTERFACE vpx)
  target_include_directories(codecs INTERFACE "${DEPS}/vpx")
endif()

# ============================================================================
#  Smoke test -- prova de linkagem/execucao (roda no QEMU p/ arm64).
# ============================================================================
option(CODECS_BUILD_SMOKE "Compila o smoke test dos codecs" ON)
if(CODECS_BUILD_SMOKE)
  add_executable(codecs_smoke "${CMAKE_CURRENT_SOURCE_DIR}/cmake/smoke_test.c")
  target_link_libraries(codecs_smoke PRIVATE codecs)
endif()
