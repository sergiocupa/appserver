# ============================================================================
#  SVT-AV1: otimizacao no Debug, sem tocar no fonte vendorizado.
#
#  O CMakeLists.txt do SVT-AV1 faz, incondicionalmente para MSVC:
#      check_both_flags_add(TYPE DEBUG /Od)
#  ou seja, ele mesmo empurra /Od para CMAKE_C_FLAGS_DEBUG. Passar
#  -DCMAKE_C_FLAGS_DEBUG="/O2" na linha de comando nao adianta: o /Od dele entra
#  DEPOIS, e no MSVC a ultima opcao de otimizacao e a que vale.
#
#  A saida e entrar por outra categoria. As COMPILE_OPTIONS do diretorio vao para
#  o fim da linha de comando, depois de CMAKE_C_FLAGS_<CONFIG>, entao um /O2 posto
#  aqui ganha do /Od deles. Este arquivo entra via -DCMAKE_PROJECT_INCLUDE, que o
#  CMake processa logo apos o project() -- nada do fonte de terceiros muda.
#
#  Por que otimizar no Debug: os codecs sao a unica parte do build onde a falta de
#  otimizacao muda o resultado de uma medicao, nao so o tempo de espera. O CRT
#  continua o de Debug (/MDd), entao quem linka com o SvtAv1Enc.lib nao percebe
#  diferenca nenhuma.
#
#  Uso: ver regen-codecs-cmake.bat
# ============================================================================
#  So para C e C++: o SVT-AV1 tambem compila .asm pelo NASM, e um /O2 solto ia parar na
#  linha de comando do nasm.exe, que recusa a opcao e falha o build inteiro.
if(MSVC)
    add_compile_options($<$<AND:$<CONFIG:Debug>,$<COMPILE_LANGUAGE:C,CXX>>:/O2>)
endif()
