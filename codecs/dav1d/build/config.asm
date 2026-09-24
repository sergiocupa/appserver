; config.asm -- escrito a mao; e o que o meson do dav1d geraria para x86-64.
; Valores de meson.build ("Configuration data for config.asm"). Vale para Windows (nasm
; -f win64) e Linux (nasm -f elf64): o formato do objeto vem da linha de comando.
; PREFIX nao e definido: o meson so o liga em Windows x86 32 bits e macOS.
%define private_prefix dav1d
%define ARCH_X86_64 1
%define ARCH_X86_32 0
%define PIC 1
%define STACK_ALIGNMENT 16
%define FORCE_VEX_ENCODING 0
