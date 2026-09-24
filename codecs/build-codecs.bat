@echo off
REM ============================================================================
REM  build-codecs.bat  -  Compila TODOS os projetos de codec, um a um.
REM
REM  Nenhum projeto desta pasta entra no build geral da solution: todos estao
REM  com ActiveCfg mas SEM Build.0. Um "Rebuild Solution" nao toca em codec --
REM  ele so linka o que ja foi gerado aqui. Rode este script quando mexer em
REM  codec, trocar versao de lib ou depois de clonar o repositorio.
REM
REM  ATENCAO ao preco disso: se voce alterar codec_core.c, enc_select.c ou
REM  qualquer fonte de codec e NAO rodar este script, o appservertester vai
REM  linkar a .lib antiga sem reclamar. Aconteceu duas vezes neste projeto:
REM  uma codecs.lib e uma libyuv.lib paradas em 8 de setembro causaram erros
REM  que pareciam de codigo e eram de build.
REM
REM  Uso:
REM     build-codecs.bat            ambas as configuracoes
REM     build-codecs.bat Debug      so Debug
REM     build-codecs.bat Release    so Release
REM
REM  Pre-requisitos ja no repo: nasm em codecs\tools\nasm.exe (2.16.03).
REM  Projetos cmake ja gerados em _vsbuild (nao precisa re-rodar cmake).
REM
REM  x265 COM assembly: o _vsbuild do x265 foi gerado com ENABLE_ASSEMBLY=ON. Se precisar
REM  regenerar (pasta apagada, outra maquina), o NASM tem de ser passado explicitamente --
REM  sem ele o CMake do x265 registra NASM_EXECUTABLE-NOTFOUND e DESLIGA o assembly em
REM  silencio (foi assim que o build anterior saiu sem ele).
REM ============================================================================

setlocal
set ROOT=%~dp0..
set CFGS=%1
if "%CFGS%"=="" set CFGS=Debug Release

for %%C in (%CFGS%) do (
    echo.
    echo ============================================================
    echo   %%C
    echo ============================================================

    echo [1/5] terceiros: SVT-AV1, x265, libyuv ...
    call :build "%ROOT%\codecs\svtav1\_vsbuild\Source\Lib\SvtAv1Enc.vcxproj" %%C || goto :err
    call :build "%ROOT%\codecs\x265\_vsbuild\x265-static.vcxproj"            %%C || goto :err
    call :build "%ROOT%\codecs\libyuv.vcxproj"                               %%C || goto :err

    echo [2/5] terceiros: libopus, libvpx, libdav1d ...
    call :build "%ROOT%\codecs\libopus.vcxproj"  %%C || goto :err
    call :build "%ROOT%\codecs\libvpx.vcxproj"   %%C || goto :err
    call :build "%ROOT%\codecs\libdav1d.vcxproj" %%C || goto :err

    echo [3/5] nucleo: codec_core, codec_hw ...
    call :build "%ROOT%\codecs\codec_core.vcxproj" %%C || goto :err
    call :build "%ROOT%\codecs\codec_hw.vcxproj"   %%C || goto :err

    echo [4/5] plugins: H264, H265, VP9, AV1, Opus ...
    call :build "%ROOT%\codecs\codec_h264_plugin.vcxproj" %%C || goto :err
    call :build "%ROOT%\codecs\codec_h265_plugin.vcxproj" %%C || goto :err
    call :build "%ROOT%\codecs\codec_vp9_plugin.vcxproj"  %%C || goto :err
    call :build "%ROOT%\codecs\codec_av1_plugin.vcxproj"  %%C || goto :err
    call :build "%ROOT%\codecs\codec_opus_plugin.vcxproj" %%C || goto :err

    echo [5/5] ok
)

echo.
echo ======== OK: todos os projetos de codec gerados ========
exit /b 0

:build
msbuild %1 /p:Configuration=%2 /p:Platform=x64 /p:SolutionDir="%ROOT%\" /m /v:minimal /nologo
exit /b %errorlevel%

:err
echo.
echo ******** FALHOU (codigo %errorlevel%) ********
exit /b 1
