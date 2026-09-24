@echo off
REM ============================================================================
REM  build-codecs.bat  -  Compila TODAS as libs de codec UMA VEZ.
REM  Rode isto so quando mexer em codec (versao/flags). O build normal do
REM  appserver NAO recompila codec: ele so linka os .lib/.dll gerados aqui.
REM
REM  Saidas:
REM    codecs\x64\Debug\codecs.lib      (Opus+VP9, estatica)
REM    codecs\x64\Release\codecs.dll    (Opus+VP9+AV1+x265, exporta a API)  + codecs.lib (import)
REM    codecs\svtav1\Bin\{Debug,Release}\SvtAv1Enc.lib
REM    codecs\x265\_vsbuild\{Debug,Release}\x265-static.lib
REM
REM  Pre-requisitos ja no repo: nasm em codecs\tools\nasm.exe (2.16.03).
REM  Projetos cmake ja gerados em _vsbuild (nao precisa re-rodar cmake).
REM
REM  x265 COM assembly: o _vsbuild do x265 foi gerado com ENABLE_ASSEMBLY=ON. Se precisar
REM  regenerar (pasta apagada, outra maquina), o NASM tem de ser passado explicitamente --
REM  sem ele o CMake do x265 registra NASM_EXECUTABLE-NOTFOUND e DESLIGA o assembly em
REM  silencio (foi assim que o build anterior saiu sem ele). E o CMAKE_GENERATOR_INSTANCE
REM  leva version= porque o registro do VS 18 no Installer desta maquina esta quebrado:
REM    cmake -S codecs\x265\source -B codecs\x265\_vsbuild -G "Visual Studio 18 2026" -A x64 ^
REM          -DCMAKE_GENERATOR_INSTANCE="C:/Program Files/Microsoft Visual Studio/18/Community,version=18.10.12201.205" ^
REM          -DENABLE_ASSEMBLY=ON -DNASM_EXECUTABLE=%ROOT%\codecs\tools\nasm.exe
REM  (a versao sai de Common7\IDE\devenv.isolation.ini, chave InstallationVersion)
REM ============================================================================
setlocal
set ROOT=%~dp0..
REM  Visual Studio 2026 (toolset v145): e o toolset de TODOS os projetos da solution.
REM  Compilar as libs de codec com outro toolset mistura CRT no link final.
call "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1

echo.
echo [1/4] AV1 encode (SVT-AV1)  Debug + Release ...
msbuild "%ROOT%\codecs\svtav1\_vsbuild\Source\Lib\SvtAv1Enc.vcxproj" /p:Configuration=Debug   /p:Platform=x64 /m /v:minimal /nologo || goto :err
msbuild "%ROOT%\codecs\svtav1\_vsbuild\Source\Lib\SvtAv1Enc.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo || goto :err

echo.
echo [2/4] H265 (x265)  Debug + Release ...
msbuild "%ROOT%\codecs\x265\_vsbuild\x265-static.vcxproj" /p:Configuration=Debug   /p:Platform=x64 /m /v:minimal /nologo || goto :err
msbuild "%ROOT%\codecs\x265\_vsbuild\x265-static.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo || goto :err

echo.
echo [3/4] codecs (Opus+VP9+dav1d)  Debug=.lib  Release=.dll ...
msbuild "%ROOT%\codecs\codecs.vcxproj" /p:Configuration=Debug   /p:Platform=x64 /m /v:minimal /nologo || goto :err
msbuild "%ROOT%\codecs\codecs.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo || goto :err

echo.
echo [4/4] libyuv (rescale I420)  Debug + Release  static .lib ...
msbuild "%ROOT%\codecs\libyuv.vcxproj" /p:Configuration=Debug   /p:Platform=x64 /m /v:minimal /nologo || goto :err
msbuild "%ROOT%\codecs\libyuv.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /v:minimal /nologo || goto :err

echo.
echo ======== OK: todas as libs/dll de codec geradas ========
exit /b 0

:err
echo.
echo ******** FALHOU (codigo %errorlevel%) ********
exit /b 1
