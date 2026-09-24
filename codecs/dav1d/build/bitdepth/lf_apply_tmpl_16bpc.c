/* Gerado por scratchpad/bitdepth_wrappers.py -- NAO editar a mao.
 *
 * O dav1d compila este fonte uma vez por profundidade de bits. Um .vcxproj nao
 * pode listar o mesmo arquivo duas vezes (a IDE recusa o projeto), entao cada
 * profundidade ganha seu proprio arquivo, que so fixa o BITDEPTH e inclui o
 * original.
 */
#define BITDEPTH 16
#include "../../src/lf_apply_tmpl.c"
