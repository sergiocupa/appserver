@echo off
REM ============================================================================
REM  regen-codecs-cmake.bat  -  Regera os projetos CMake do SVT-AV1 e do x265.
REM
REM  Rode isto quando a pasta _vsbuild sumir, ao clonar o repositorio em outra
REM  maquina, ou depois de atualizar o Visual Studio. As _vsbuild nao sao
REM  versionadas; quem versiona e este script.
REM
REM  O QUE ELE GARANTE, alem de gerar:
REM
REM  1) x265 COM assembly. Sem o NASM no caminho, o CMake do x265 registra
REM     NASM_EXECUTABLE-NOTFOUND e DESLIGA o assembly em silencio. Ja aconteceu.
REM
REM  2) Otimizacao LIGADA tambem na configuracao Debug, mantendo o CRT de Debug
REM     (MultiThreadedDebugDLL). Os codecs sao a unica parte do build onde a
REM     falta de otimizacao muda o RESULTADO de uma medicao, e nao so o tempo de
REM     espera: o libvpx do Linux se compila pelo ./configure dele e sai sempre
REM     otimizado, enquanto o do Windows saia /Od. O mesmo teste de ida e volta
REM     marcava 420 ms num lado e 60 ms no outro.
REM     Como o CRT nao muda, quem linka com estas .lib nao percebe diferenca.
REM
REM  3) /RTC desligado. O MSVC recusa /RTC junto com /O2.
REM
REM  O SVT-AV1 precisa de um tratamento a parte: o CMakeLists dele empurra /Od
REM  para o Debug por conta propria, e passar CMAKE_C_FLAGS_DEBUG nao vence isso.
REM  Ver svtav1-otimiza-debug.cmake, que entra por CMAKE_PROJECT_INCLUDE.
REM ============================================================================

setlocal
set AQUI=%~dp0
set VS=C:\Program Files\Microsoft Visual Studio\18\Community
set CMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe

if not exist "%CMAKE%" (
    echo ERRO: cmake nao encontrado em "%CMAKE%"
    echo Ajuste a variavel VS no topo deste script.
    exit /b 1
)

echo.
echo ==== SVT-AV1 ====
"%CMAKE%" -S "%AQUI%svtav1" -B "%AQUI%svtav1\_vsbuild" ^
    -DCMAKE_GENERATOR_INSTANCE="%VS%" ^
    -DCMAKE_PROJECT_INCLUDE="%AQUI%svtav1-otimiza-debug.cmake" ^
    -DCMAKE_C_FLAGS_DEBUG="/Zi /Ob2 /O2" ^
    -DCMAKE_CXX_FLAGS_DEBUG="/Zi /Ob2 /O2" ^
    -DBUILD_SHARED_LIBS=OFF -DBUILD_APPS=OFF -DBUILD_TESTING=OFF || goto :err

echo.
echo ==== x265 (assembly ON, NASM explicito) ====
"%CMAKE%" -S "%AQUI%x265\source" -B "%AQUI%x265\_vsbuild" ^
    -DCMAKE_GENERATOR_INSTANCE="%VS%" ^
    -DCMAKE_C_FLAGS_DEBUG="/MDd /Zi /Ob2 /O2" ^
    -DCMAKE_CXX_FLAGS_DEBUG="/MDd /Zi /Ob2 /O2" ^
    -DENABLE_ASSEMBLY=ON -DNASM_EXECUTABLE="%AQUI%tools\nasm.exe" ^
    -DENABLE_SHARED=OFF -DENABLE_CLI=OFF -DENABLE_TESTS=OFF || goto :err

echo.
echo ======== OK: projetos regerados. Agora rode build-codecs.bat ========
exit /b 0

:err
echo.
echo ******** FALHOU (codigo %errorlevel%) ********
exit /b 1
