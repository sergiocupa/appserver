# ============================================================================
#  appserver -- build LINUX (x64 e ARM64). O build Windows continua no appserver.sln.
#
#  Uso (x64, Linux/WSL):
#     cmake -S . -B build/linux-x64 -G Ninja -DCMAKE_BUILD_TYPE=Release
#     cmake --build build/linux-x64
#     cd test/appservertester && ../../build/linux-x64/appservertester
#
#  Dependencias do sistema (Ubuntu 24.04):
#     apt install build-essential cmake ninja-build nasm pkg-config libopenh264-dev libde265-dev
#  Opus, SVT-AV1, x265 e libvpx sao compilados a partir de codecs/ (codecs/CMakeLists.txt).
#  openh264 e libde265 vem do sistema: no Windows eles entram como binarios prontos, sem fonte.
#
#  xplatbase e yason vem dos submodules. XPLATBASE_DIR / YASON_DIR apontam para outra copia
#  (ex.: a origem em desenvolvimento) sem mexer no submodule.
#
#  As listas de fontes espelham os .vcxproj, para os dois sistemas compilarem o mesmo codigo.
# ============================================================================
cmake_minimum_required(VERSION 3.20)
project(appserver C CXX)

if(WIN32)
  message(FATAL_ERROR "No Windows use o appserver.sln. Este CMake e o build Linux.")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_EXTENSIONS ON)                 # gnu11: o codigo usa POSIX (strdup, fseeko, ...)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(XPLATBASE_DIR "${ROOT}/appserver/submodules/xplatbase/Xplatbase/Xplatbase" CACHE PATH "xplatbase (pasta com src/ e include/)")
set(YASON_DIR     "${ROOT}/appserver/submodules/yason/yason"                    CACHE PATH "yason (pasta com src/ e include/)")

string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _arch)
if(_arch MATCHES "aarch64|arm64")
  set(APP_ARCH arm64)
elseif(_arch MATCHES "x86_64|amd64")
  set(APP_ARCH x64)
else()
  message(FATAL_ERROR "Arquitetura nao suportada: ${CMAKE_SYSTEM_PROCESSOR}")
endif()
message(STATUS "appserver: Linux ${APP_ARCH}")
message(STATUS "  xplatbase: ${XPLATBASE_DIR}")
message(STATUS "  yason:     ${YASON_DIR}")

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPENH264 REQUIRED IMPORTED_TARGET openh264)
pkg_check_modules(DE265    REQUIRED IMPORTED_TARGET libde265)

# O codigo inclui "codec_api.h" e "de265.h" sem subpasta (no Windows os includes apontam direto
# para as pastas deles); aqui o equivalente e /usr/include/wels e /usr/include/libde265.
find_path(OPENH264_WELS_DIR codec_api.h HINTS ${OPENH264_INCLUDEDIR} /usr/include PATH_SUFFIXES wels REQUIRED)
find_path(DE265_HDR_DIR     de265.h     HINTS ${DE265_INCLUDEDIR}    /usr/include PATH_SUFFIXES libde265 REQUIRED)

# ---- compatibilidade com o MSVC, so nos alvos do proprio projeto ---------------------------
#  -include msvc_compat.h: sprintf_s/fopen_s/_stricmp & cia (ver o header).
#  -fcommon: o MSVC aceita a mesma variavel global definida em mais de um .c (via header); o
#            gcc >= 10 recusa com "multiple definition". Mantem o comportamento do Windows.
set(MSVC_COMPAT "${ROOT}/cmake/linux/msvc_compat.h")
function(app_compat tgt)
  target_compile_options(${tgt} PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${MSVC_COMPAT}>"
    "$<$<COMPILE_LANGUAGE:C>:-fcommon>")
endfunction()

# ============================================================================
#  Codecs de terceiros: Opus, SVT-AV1, x265, libvpx (alvo INTERFACE 'codecs')
# ============================================================================
set(CODECS_BUILD_SMOKE OFF CACHE BOOL "" FORCE)
add_subdirectory(codecs)

# ============================================================================
#  xplatbase / yason
# ============================================================================
# Instancia unica: com XPLATBASE_SHARED=ON a lib vira COMPARTILHADA e todo modulo do processo
# (executavel e plugins) passa a usar o MESMO pool de memoria, registro de threads e pool de
# tarefas. Estatico continua sendo o padrao -- ver prep-xplatbase/PLANO.md.
option(XPLATBASE_SHARED "xplatbase como biblioteca compartilhada (instancia unica no processo)" OFF)
set(XPLATBASE_KIND STATIC)
if(XPLATBASE_SHARED)
  set(XPLATBASE_KIND SHARED)
endif()

# O detector de instancia duplicada e novo na origem; o submodule so o tera depois do push.
set(XPLATBASE_INSTANCE_SRC "")
if(EXISTS ${XPLATBASE_DIR}/src/xplat_instance.c)
  set(XPLATBASE_INSTANCE_SRC ${XPLATBASE_DIR}/src/xplat_instance.c)
endif()

set(XPLATBASE_SOURCES
  ${XPLATBASE_INSTANCE_SRC}
  ${XPLATBASE_DIR}/src/atomics.c
  ${XPLATBASE_DIR}/src/event_handler.c
  ${XPLATBASE_DIR}/src/list_hander.c
  ${XPLATBASE_DIR}/src/mem_leak_watch.c
  ${XPLATBASE_DIR}/src/memory_pool.c
  ${XPLATBASE_DIR}/src/numeric.c
  ${XPLATBASE_DIR}/src/ring_queue.c
  ${XPLATBASE_DIR}/src/string_handler.c
  ${XPLATBASE_DIR}/src/thread_activity_linux.c     # o _win.c e a metade Windows
  ${XPLATBASE_DIR}/src/thread_handler.c
  ${XPLATBASE_DIR}/src/thread_pool.c
  ${XPLATBASE_DIR}/src/thread_wait.c
  ${XPLATBASE_DIR}/src/xplatbase.c)

add_library(xplatbase ${XPLATBASE_KIND} ${XPLATBASE_SOURCES})
target_include_directories(xplatbase PUBLIC ${XPLATBASE_DIR}/src ${XPLATBASE_DIR}/include)
target_link_libraries(xplatbase PUBLIC Threads::Threads m ${CMAKE_DL_LIBS})
if(XPLATBASE_SHARED)
  # BUILD_SHARED liga a visibilidade default no macro; USE_SHARED vai para quem consome.
  target_compile_definitions(xplatbase PRIVATE XPLATBASE_BUILD_SHARED)
  target_compile_definitions(xplatbase PUBLIC  XPLATBASE_USE_SHARED)
  set_target_properties(xplatbase PROPERTIES C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
  # Executaveis acham a .so ao lado deles, sem depender de LD_LIBRARY_PATH.
  set(CMAKE_BUILD_RPATH "$ORIGIN")
  set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

add_library(yason STATIC
  ${YASON_DIR}/src/yason.c
  ${YASON_DIR}/src/yason_compat.c)
target_include_directories(yason PUBLIC ${YASON_DIR}/src ${YASON_DIR}/include)
target_link_libraries(yason PUBLIC xplatbase)

# ============================================================================
#  Servidor HTTP (lib appserver)
# ============================================================================
add_library(appserver_lib STATIC
  appserver/src/appclient.c
  appserver/src/appserver.c
  appserver/src/event_server.c
  appserver/src/server_type.c
  appserver/src/utils/activity_binder.c
  appserver/src/utils/callback_binder.c
  appserver/src/utils/message_assembler.c
  appserver/src/utils/message_parser.c
  appserver/src/utils/websocket_util.c
  appserver/src/utils/xpb_compat.c)
target_include_directories(appserver_lib PUBLIC ${ROOT}/appserver/include ${ROOT}/appserver/src ${ROOT}/appserver/src/utils)
target_compile_definitions(appserver_lib PRIVATE XPLATBASE_NO_AUTO_INIT)
target_link_libraries(appserver_lib PUBLIC yason xplatbase)
app_compat(appserver_lib)

# Otimizacao ISOLADA: so os alvos de codec, mesmo num build Debug. As deps de terceiros
# ganham isso no escopo de codecs/CMakeLists.txt; estes alvos moram aqui, entao ganham um
# a um. As COMPILE_OPTIONS do alvo entram DEPOIS de CMAKE_C_FLAGS_<CONFIG> na linha de
# comando, entao o -O3 vence o -O0 que o gcc assume quando so ha -g; o -g continua.
#
# Motivo e preco estao escritos por extenso em codecs/CMakeLists.txt.
function(codec_otimiza alvo)
  if(NOT MSVC)
    target_compile_options(${alvo} PRIVATE -O3)
  endif()
endfunction()

# ============================================================================
#  libyuv (mesmos 22 fontes do libyuv.vcxproj; os _win/_neon se auto-excluem por #if)
# ============================================================================
set(YUV "${ROOT}/codecs/vpx/third_party/libyuv")
add_library(yuv STATIC
  ${YUV}/source/scale.cc ${YUV}/source/scale_common.cc ${YUV}/source/scale_any.cc
  ${YUV}/source/scale_gcc.cc ${YUV}/source/scale_win.cc ${YUV}/source/scale_neon.cc
  ${YUV}/source/scale_neon64.cc ${YUV}/source/row_common.cc ${YUV}/source/row_any.cc
  ${YUV}/source/row_gcc.cc ${YUV}/source/row_win.cc ${YUV}/source/row_neon.cc
  ${YUV}/source/row_neon64.cc ${YUV}/source/cpu_id.cc ${YUV}/source/planar_functions.cc
  ${YUV}/source/convert.cc ${YUV}/source/rotate.cc ${YUV}/source/rotate_common.cc
  ${YUV}/source/rotate_any.cc ${YUV}/source/rotate_gcc.cc ${YUV}/source/rotate_win.cc
  ${YUV}/source/video_common.cc)
target_include_directories(yuv PUBLIC ${YUV}/include)
codec_otimiza(yuv)

# ============================================================================
#  dav1d (decoder AV1) -- C + assembly NASM em x86-64 (elf64); ARM64 em C puro
# ============================================================================
set(DAV1D "${ROOT}/codecs/dav1d")
file(GLOB DAV1D_SRC ${DAV1D}/src/*.c)
list(FILTER DAV1D_SRC EXCLUDE REGEX "_tmpl\\.c$")          # entram pelos wrappers por bitdepth
file(GLOB DAV1D_BITDEPTH ${DAV1D}/build/bitdepth/*.c)
set(DAV1D_ASM_OBJS)
if(APP_ARCH STREQUAL "x64")
  find_program(NASM nasm REQUIRED)
  list(APPEND DAV1D_SRC ${DAV1D}/src/x86/cpu.c)
  # Mesma lista do meson.build (e do codecs.vcxproj): 9 comuns + 16 de 8 bits + 21 de 16 bits.
  set(DAV1D_ASM
    cpuid msac pal refmvs itx_avx512 cdef_avx2 itx_avx2 cdef_sse itx_sse
    cdef_avx512 filmgrain_avx512 ipred_avx512 loopfilter_avx512 looprestoration_avx512 mc_avx512
    filmgrain_avx2 ipred_avx2 loopfilter_avx2 looprestoration_avx2 mc_avx2
    filmgrain_sse ipred_sse loopfilter_sse looprestoration_sse mc_sse
    cdef16_avx512 filmgrain16_avx512 ipred16_avx512 itx16_avx512 loopfilter16_avx512
    looprestoration16_avx512 mc16_avx512 cdef16_avx2 filmgrain16_avx2 ipred16_avx2 itx16_avx2
    loopfilter16_avx2 looprestoration16_avx2 mc16_avx2 cdef16_sse filmgrain16_sse ipred16_sse
    itx16_sse loopfilter16_sse looprestoration16_sse mc16_sse)
  set(_asm_dir "${CMAKE_CURRENT_BINARY_DIR}/dav1d_asm")
  file(MAKE_DIRECTORY "${_asm_dir}")
  foreach(a ${DAV1D_ASM})
    set(obj "${_asm_dir}/${a}.o")
    add_custom_command(OUTPUT "${obj}"
      COMMAND ${NASM} -f elf64 -I ${DAV1D}/src/ -I ${DAV1D}/build/ -o "${obj}" ${DAV1D}/src/x86/${a}.asm
      DEPENDS ${DAV1D}/src/x86/${a}.asm ${DAV1D}/build/config.asm ${DAV1D}/src/ext/x86/x86inc.asm
      COMMENT "nasm dav1d ${a}.asm"
      VERBATIM)
    list(APPEND DAV1D_ASM_OBJS "${obj}")
  endforeach()
  set_source_files_properties(${DAV1D_ASM_OBJS} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
endif()
add_library(dav1d STATIC ${DAV1D_SRC} ${DAV1D_BITDEPTH} ${DAV1D_ASM_OBJS})
target_include_directories(dav1d
  PUBLIC  ${DAV1D}/include
  PRIVATE ${DAV1D} ${DAV1D}/include/dav1d ${DAV1D}/src ${DAV1D}/build)
target_link_libraries(dav1d PUBLIC Threads::Threads m)
codec_otimiza(dav1d)

# ============================================================================
#  Camada de codecs (encode/decode, seletor de backend)
# ============================================================================
#  Um alvo por codec, como no Visual Studio. O nucleo (codec_core) nao conhece o nome de
#  nenhum deles: as tabelas do enc_select.c e do dec_select.c sao a unica ligacao, e e por
#  isso que cada um pode virar plugin depois.
#
#  codecs_media continua existindo como alvo INTERFACE: quem linkava ele nao muda de linha.

set(CODEC_DEFS HAVE_LIBVPX HAVE_SVTAV1 HAVE_OPENH264 HAVE_X265 HAVE_LIBOPUS HAVE_DAV1D)

function(codec_alvo nome)
  add_library(${nome} STATIC ${ARGN})
  target_include_directories(${nome}
    PUBLIC  ${ROOT}/codecs/media
    PRIVATE ${OPENH264_WELS_DIR} ${DE265_HDR_DIR})
  target_compile_definitions(${nome} PRIVATE ${CODEC_DEFS})
  target_link_libraries(${nome} PUBLIC codecs dav1d xplatbase PkgConfig::OPENH264 PkgConfig::DE265)
  codec_otimiza(${nome})
  app_compat(${nome})
endfunction()

# Um codec como PLUGIN: biblioteca carregada em tempo de execucao. O nome do arquivo
# (libcodec_<x>.so) e o que o carregador procura -- ver codec_loader.c.
#   codec_plugin(<nome> <define> <fontes...>)
# Os backends de decode de H26x levam a PROPRIA copia do image_plane.c em vez de linkar o
# nucleo. So e seguro porque o xplatbase e unico no processo: o que um modulo aloca, o
# outro libera.
function(codec_plugin nome define)
  add_library(${nome} SHARED ${ARGN})
  target_include_directories(${nome}
    PUBLIC  ${ROOT}/codecs/media
    PRIVATE ${OPENH264_WELS_DIR} ${DE265_HDR_DIR})
  target_compile_definitions(${nome} PRIVATE ${CODEC_DEFS} CODEC_PLUGIN_BUILD ${define})
  target_link_libraries(${nome} PRIVATE codecs dav1d xplatbase PkgConfig::OPENH264 PkgConfig::DE265)
  set_target_properties(${nome} PROPERTIES C_VISIBILITY_PRESET hidden)
  codec_otimiza(${nome})
  app_compat(${nome})
endfunction()

codec_alvo(codec_core  codecs/media/media_codec.c
                       codecs/media/codec_registry.c
                       codecs/media/codec_loader.c
                       codecs/media/enc_select.c
                       codecs/media/dec_select.c
                       codecs/media/image_plane.c
                       codecs/media/cpu_features.c)
# _GNU_SOURCE pela linha de comando, e nao no .c: o -include do msvc_compat.h (app_compat)
# entra ANTES do arquivo, entao um #define la dentro chega tarde e o dladdr/Dl_info some.
# Mesma armadilha que ja tinha aparecido no mod_duplicado.
target_compile_definitions(codec_core PRIVATE _GNU_SOURCE)
codec_plugin(codec_h264 CODEC_H264_PLUGIN
  codecs/media/codec_h264.c codecs/media/codec_h264_dec.c codecs/media/image_plane.c)
codec_plugin(codec_h265 CODEC_H265_PLUGIN
  codecs/media/codec_h265.c codecs/media/codec_h265_dec.c codecs/media/image_plane.c)
codec_plugin(codec_vp9 CODEC_VP9_PLUGIN codecs/media/codec_vp9.c)
codec_plugin(codec_av1 CODEC_AV1_PLUGIN codecs/media/codec_av1.c)
codec_plugin(codec_opus CODEC_OPUS_PLUGIN codecs/media/codec_opus.c)

codec_alvo(codec_hw    codecs/media/hw_mf.c codecs/media/hw_dec_mf.c)   # fora do Windows, so os stubs

add_library(codecs_media INTERFACE)
target_link_libraries(codecs_media INTERFACE
  codec_core codec_hw
  # o nucleo referencia os backends pelas tabelas, e os backends referenciam o nucleo:
  # repetido para o linkador estatico fechar o ciclo
  codec_core)

# ============================================================================
#  MediaGateway / MediaFragmenter
#  Dependem um do outro (o gateway abre MP4 pelo fragmentador; o fragmentador usa os muxers
#  do gateway). O CMake repete as libs estaticas no link para resolver o ciclo.
# ============================================================================
add_library(mediagateway STATIC
  MediaGateway/media/sync_gateway.c
  MediaGateway/media/media_sink.c
  MediaGateway/media/sink_mp4.c
  MediaGateway/media/sink_hls.c
  MediaGateway/media/sink_dash.c
  MediaGateway/media/sink_live.c
  MediaGateway/media/live_store.c
  MediaGateway/media/mux_webm.c
  MediaGateway/media/mux_mp4.c
  MediaGateway/media/audio_aac.c
  MediaGateway/media/mp4_timing.c
  MediaGateway/media/h26x_util.c
  MediaGateway/media/gw_path.c
  MediaGateway/media/device_enum.c
  MediaGateway/media/source_camera.c
  MediaGateway/media/gw_decode.c
  MediaGateway/media/demux_webm.c
  MediaGateway/media/source_file.c)

add_library(mediafragmenter STATIC
  MediaFragmenter/src/BufferUtil.c
  MediaFragmenter/src/VideoCodecWrapper.c
  MediaFragmenter/src/DashBuilder.c
  MediaFragmenter/src/DashDiag.c
  MediaFragmenter/src/FileUtil.c
  MediaFragmenter/src/H264Builder.c
  MediaFragmenter/src/H265Builder.c
  MediaFragmenter/src/ImageTools.c
  MediaFragmenter/src/MediaFragmenter.c
  MediaFragmenter/src/MediaFragmenterType.c
  MediaFragmenter/src/Mp4Builder.c
  # Mp4BuilderTest.c: codigo de teste sem chamadores, que chama funcoes que nao existem
  # (build_avcC em vez de _build_avcC). No MSVC so nao quebra porque o objeto nunca e
  # puxado pelo link; no gcc >= 14 a declaracao implicita ja e erro de compilacao.
  MediaFragmenter/src/Mp4Diag.c
  MediaFragmenter/src/Mp4H265MetadataUtil.c
  MediaFragmenter/src/Mp4MetadataUtil.c
  MediaFragmenter/src/media_source.c
  MediaFragmenter/src/frag_session.c
  MediaFragmenter/src/pipeline.c
  MediaFragmenter/src/embed_ops.c)

target_include_directories(mediagateway PUBLIC ${ROOT}/MediaGateway/media)
target_compile_definitions(mediagateway PRIVATE HAVE_LIBYUV)
target_include_directories(mediafragmenter PUBLIC
  ${ROOT}/MediaFragmenter/include ${ROOT}/MediaFragmenter/src ${OPENH264_WELS_DIR} ${DE265_HDR_DIR})

target_link_libraries(mediagateway    PUBLIC mediafragmenter codecs_media yuv yason xplatbase)
target_link_libraries(mediafragmenter PUBLIC mediagateway    codecs_media yuv yason xplatbase)
set_property(TARGET mediagateway mediafragmenter PROPERTY LINK_INTERFACE_MULTIPLICITY 3)
app_compat(mediagateway)
app_compat(mediafragmenter)

# ============================================================================
#  Servidor de teste (UI do player + rotas)
# ============================================================================
add_executable(appservertester
  test/appservertester/appservertester.c
  test/appservertester/hls_controller.c
  test/appservertester/session_controller.c
  test/appservertester/device_controller.c
  test/appservertester/live_controller.c)
target_include_directories(appservertester PRIVATE ${ROOT}/test/appservertester)
target_link_libraries(appservertester PRIVATE mediafragmenter mediagateway appserver_lib)
set_target_properties(appservertester PROPERTIES LINKER_LANGUAGE CXX)   # x265/libyuv sao C++
app_compat(appservertester)

# ============================================================================
#  Testes unitarios (os MESMOS arquivos .c que o Visual Studio roda no Windows)
#  Aqui quem chama e o runner_main.c; la, o registro.cpp entrega ao Gerenciador.
# ============================================================================
enable_testing()

add_executable(unittests
  test/unittests/ctest_core.c
  test/unittests/test_xplatbase.c
  test/unittests/test_pipeline.c
  test/unittests/test_hardware.c
  test/unittests/test_codecs.c
  test/unittests/test_gateway_matrix.c
  test/unittests/test_plugins.c
  test/unittests/bench_h264.c
  test/unittests/test_instancia_modulos.c
  test/unittests/runner_main.c)
target_include_directories(unittests PRIVATE
  ${ROOT}/test/unittests ${ROOT}/codecs/media ${ROOT}/MediaGateway/media
  ${ROOT}/MediaFragmenter/include ${ROOT}/MediaFragmenter/src)
target_link_libraries(unittests PRIVATE mediafragmenter mediagateway xplatbase)
set_target_properties(unittests PROPERTIES LINKER_LANGUAGE CXX)   # x265/libyuv sao C++
app_compat(unittests)

# Um teste do CTest por CASO: a lista sai do proprio executavel, entao nao ha uma segunda
# lista para manter em sincronia.
foreach(caso
    "InstanciaUnica.ProcessoTemUmaSoInstancia"
    "InstanciaUnica.InstanciaVemDaCompartilhada"
    "PoolDeMemoria.ContabilizaAlocacaoELiberacao"
    "PoolDeMemoria.EscreveELeBlocoGrande"
    "PoolDeTarefas.ExecutaTodasAsTarefas"
    "Pipeline.WebmRoundtripVp9"
    "Pipeline.WebmRoundtripAv1"
    "Pipeline.GatewayDashVp9"
    "Pipeline.GatewayHlsH264"
    "Pipeline.GatewayMemoriaNaoCresce"
    "Bench.H264Threads"
    "Bench.TodosCodecs"
    "Bench.Varredura"
    "Plugins.Carregado"
    "Plugins.UsaOPoolDoHost"
    "Plugins.NaoSaiEmUso"
    "Plugins.DescarregaEVolta"
    "GatewayMatriz.AlternaCodec"
    "GatewayMatriz.AlternaResolucaoEFps"
    "GatewayMatriz.AlternaEntrada"
    "Codecs.IdaEVoltaH264"
    "Codecs.IdaEVoltaH265"
    "Codecs.IdaEVoltaVp9"
    "Codecs.IdaEVoltaAv1"
    "Codecs.EncodeOpus"
    "Hardware.EncoderPorHardwareH264"
    "Hardware.DecodificaOQueOEncoderProduziu")
  add_test(NAME ${caso} COMMAND unittests ${caso})
endforeach()

# ---- modulos auxiliares dos testes de instancia unica ----------------------
#  compartilhado: consome a MESMA biblioteca do host (o caso correto)
#  duplicado    : compila o xplatbase DENTRO dele, de proposito, para provocar a segunda
#                 instancia -- e o unico jeito de provar que o detector acusa
add_library(mod_compartilhado SHARED test/unittests/mod_compartilhado.c)
target_link_libraries(mod_compartilhado PRIVATE xplatbase)
set_target_properties(mod_compartilhado PROPERTIES C_VISIBILITY_PRESET hidden)
app_compat(mod_compartilhado)

add_library(mod_duplicado SHARED test/unittests/mod_duplicado.c ${XPLATBASE_SOURCES})
target_include_directories(mod_duplicado PRIVATE ${XPLATBASE_DIR}/src ${XPLATBASE_DIR}/include)
target_link_libraries(mod_duplicado PRIVATE Threads::Threads m ${CMAKE_DL_LIBS})
set_target_properties(mod_duplicado PROPERTIES C_VISIBILITY_PRESET hidden)
# Sem o app_compat aqui: o -include do msvc_compat.h entra ANTES do _GNU_SOURCE que o
# xplat_instance.c define, e ai o dladdr/Dl_info some. Estas fontes sao do xplatbase e nao
# precisam dos apelidos do MSVC.
target_compile_definitions(mod_duplicado PRIVATE _GNU_SOURCE)

add_dependencies(unittests mod_compartilhado mod_duplicado
                 codec_h264 codec_h265 codec_vp9 codec_av1 codec_opus)

foreach(caso
    "InstanciaEntreModulos.ModuloCompartilhadoUsaMesmoPool"
    "InstanciaEntreModulos.ModuloCompartilhadoMesmasThreads"
    "InstanciaEntreModulos.ModuloUsaMesmoPoolDeTarefas"
    "InstanciaEntreModulos.DuplicataEDenunciada"
    "InstanciaEntreModulos.CicloCarregaDescarrega")
  add_test(NAME ${caso} COMMAND unittests ${caso})
endforeach()
