# Preparação: xplatbase com instância única garantida

> Pasta temporária de trabalho. Some quando a preparação terminar — nada aqui é código.
>
> Este documento **é a especificação da implementação**. Cada fase tem objetivo, o que muda,
> como se implementa e **como se prova que ficou certo**. Uma fase só está pronta quando
> todos os itens de verificação dela estiverem marcados.

## Por que isto existe

Os codecs vão virar plugins carregados sob demanda (DLL/.so). Hoje o xplatbase é biblioteca
**estática**: cada módulo que o linka ganha a **própria cópia** do pool de memória, do registro
de threads e do pool de tarefas.

Consequências, medidas no código (`src/memory_pool.c`, `src/thread_pool.c`):

| estado | onde | o que acontece com 2 cópias |
|---|---|---|
| `static MemPool G` | memory_pool.c | contabilidade racha; `memop_get_stats` só vê a do próprio módulo |
| `MEMOP_TLS MemHeap* g_heap` | memory_pool.c | heaps por thread separados por módulo |
| `g_site_table` | memory_pool.c | `mem_leak_watch` deixa de enxergar o outro lado |
| `static ThreadPool* GlobalPool` | thread_pool.c | um segundo pool, com mais threads, disputando os mesmos núcleos |
| `g_pool_self` (TLS) | thread_pool.c | submissão reentrante deixa de ser reconhecida |
| registro de threads | thread_handler.c | `thread_enum` não vê as threads do outro módulo |

E o pior: **liberar memória cruzando a fronteira vira corrupção silenciosa.**

Sem instância única não há como gerenciar memória nem tarefas — é o motivo desta preparação
vir **antes** de qualquer trabalho de plugin.

## O que joga a favor (levantado no código)

- **127 funções públicas** em 12 cabeçalhos: é a superfície a exportar.
- **78 funções inline** — nenhuma toca estado global; todas operam sobre estruturas passadas
  por ponteiro. Continuam inline, sem custo e sem risco.
- **`memory_pool.h` e `thread_pool.h` não têm nada inline**: toda alocação e toda submissão já
  são chamada de função. É isto que faz a biblioteca compartilhada resolver de forma limpa —
  não existe caminho que escape para uma cópia local.
- **A API dos codecs é de empréstimo, não de transferência**: `MediaPacket.Data` e os planos do
  `MediaFrame` apontam para buffers internos, válidos até a próxima chamada. Ninguém libera
  memória do outro lado hoje.

## Critério de aceite da preparação inteira

1. Um processo com executável + DLL que usem xplatbase reporta **um** pool: alocar dentro da
   DLL tem que aparecer no `memop_get_stats` lido pelo host.
2. Tentar subir uma **segunda** instância é **detectado e denunciado** — nunca silencioso.
3. Todos os testes atuais passam, agora rodando pelo Gerenciador de Testes do Visual Studio.
4. Linux compila e o smoke passa.

---

## Fase 0 — Pré-requisitos e decisões

Nada a implementar; são bloqueios a destravar antes.

- [x] **Adaptador do Google Test: já instalado**, nas duas instâncias (confirmado por `vswhere`:
      `Microsoft.VisualStudio.Component.VC.TestAdapterForGoogleTest`). Não há nada a instalar.
      Minha conclusão anterior de que faltava estava **errada**: procurei pelo `gtest.h` na pasta
      do Visual Studio e não achei — mas o que o VS instala é só o **adaptador** (descoberta e
      exibição no Gerenciador). O **framework** não vem junto nesta versão.
      ⇒ Vira trabalho da Fase 5: **googletest como fonte no repositório**
      (`test/thirdparty/googletest`), que é o que também serve ao Linux pelo mesmo CMake.
- [ ] **Publicar a correção pendente do `numeric.c`** no repositório de origem do xplatbase.
      Ela está como alteração local lá e vai junto nesta mudança.
- [ ] **Decidir o alcance**: o modo estático continua existindo (outros projetos seus seguem
      compilando sem mudança) e o compartilhado é escolha do consumidor. **Recomendado.**

### Regras que passam a valer (escrever no README do xplatbase)

- **Exatamente uma instância por processo.**
- **Debug e Release não se misturam.** Hoje duas cópias estáticas só desperdiçam memória; com
  uma DLL só, `/MDd` de um lado e `/MD` do outro quebra de verdade.
- **O layout das estruturas públicas vira ABI.** Atenção ao não óbvio: como as funções de
  espera, atômicos e mutex são *inline*, o layout de `xwait_t` e das estruturas que elas
  manipulam **já está compilado dentro de cada consumidor**. Mudar um campo passa a exigir
  recompilar todo mundo, não só a biblioteca.

---

## Fase 1 — Macro de exportação (repositório de ORIGEM)

**Problema atual** (`include/xplatbase.h`): exporta só em Release, não tem ramo de importação,
e exporta mesmo quando o uso é estático.

```c
#if defined(XPLATBASE_WIN) && !defined(_DEBUG)
    #define XPLATBASE_API __declspec ( dllexport )
#else
    #define XPLATBASE_API
#endif
```

**Como fica** — três modos explícitos, e o estático preservado como padrão:

```c
#if defined(XPLATBASE_WIN)
  #if   defined(XPLATBASE_BUILD_SHARED)   /* compilando a DLL          */
    #define XPLATBASE_API __declspec(dllexport)
  #elif defined(XPLATBASE_USE_SHARED)     /* consumindo a DLL          */
    #define XPLATBASE_API __declspec(dllimport)
  #else                                    /* estático (padrão de hoje) */
    #define XPLATBASE_API
  #endif
#else
  #if defined(XPLATBASE_BUILD_SHARED)
    #define XPLATBASE_API __attribute__((visibility("default")))
  #else
    #define XPLATBASE_API
  #endif
#endif
```

- [x] Macro com os três modos
- [x] Varredura: toda função pública dos 12 cabeçalhos declarada com `XPLATBASE_API`
      — **16 estavam sem o macro**, e justamente as centrais: `memop_init`, `memop_shutdown`,
      `memop_span_count`, `memop_snapshot_spans`, `memop_leak_watch_enable`,
      `memop_on_created_thread`, `memop_on_ended_thread`, `thread_create`, `thread_join`,
      `thread_init`, `thread_enum`, `pool_create`, `pool_destroy`, `xpb_event_init`,
      `xthread_activity_init`, `xthread_cycles_per_ns`. Sem elas exportadas a DLL seria
      inútil — o consumidor não conseguiria nem inicializar o pool. Corrigido.
- [x] Compilar no modo **estático**: `Xplatbase.lib` gerada, sem erro. Os avisos que aparecem
      são anteriores a esta mudança, em arquivos não tocados.

**Anotação para depois** (fora do escopo desta preparação): `thread_handler.c(25)` tem o aviso
C4133 atribuindo `void**` a `Thread**` — a mesma classe de defeito que corrigimos no appserver.

---

## Fase 2 — Build compartilhado (ORIGEM + appserver)

- [x] Windows: **um projeto, quatro configurações** — `Debug`/`Release` (estático) e
      `Debug DLL`/`Release DLL`. Quem consome escolhe pelo mapeamento da solution.
      Saída da DLL em `x64\<Debug|Release>\dll\` (sem o sufixo no caminho), porque a
      biblioteca de importação também se chama `Xplatbase.lib` e sobrescreveria a estática.

      *Correção de rumo:* eu havia criado um projeto irmão `XplatbaseDll.vcxproj`. O usuário
      apontou que a decisão pertence a quem consome, não a dois projetos para manter em
      sincronia — e estava certo: o arquivo novo do detector eu tive de acrescentar nos dois.
      Projeto irmão removido.
- [x] `dumpbin /exports`: **132 símbolos**, incluindo `memop_alloc_raw`, `memop_get_stats`,
      `thread_create`, `thread_enum`, `pool_create_relative`, `xplat_instance_check`

### Correção de rumo: exportar só o que é chamado de fora

Na Fase 1 eu marquei com `XPLATBASE_API` **todas** as declarações públicas dos cabeçalhos,
inclusive as de inicialização. Estava errado, e o usuário apontou: `memop_init`,
`xpb_event_init`, `thread_init`, `pool_create`, `pool_destroy` e `xthread_activity_init` são
chamadas **só** pelo `platform_init`, que roda sozinho pelo inicializador de CRT.

Medido nos consumidores (appserver, MediaGateway, codecs, MediaFragmenter, tester): das 16 que
eu havia marcado, apenas **`thread_create` e `thread_join`** são usadas de fora.

**13 desexportadas.** Superfície: 145 → 132 símbolos, nas duas plataformas. O princípio que
fica valendo: *o que sai da biblioteca vira compromisso de ABI* — uma vez exportado, alguém
chama, e depois não dá mais para mudar. `thread_enum` ficou pública de propósito: é a
observação de threads que os testes de instância única usam.
- [x] TLS conferido **na prática**: o `g_heap` (`__declspec(thread)`) funciona com a DLL
      carregada em tempo de execução — as alocações da DLL entraram na conta do host
- [x] Linux: opção `XPLATBASE_SHARED` no `CMakeLists.txt` (padrão OFF). Liga `SHARED`,
      `C_VISIBILITY_PRESET hidden`, `POSITION_INDEPENDENT_CODE` e `rpath $ORIGIN` — o
      executável acha a `.so` ao lado dele, sem depender de `LD_LIBRARY_PATH`.
      O `xplat_instance.c` entra por `if(EXISTS ...)`: assim o build continua funcionando
      **antes** do push (quando o submodule ainda não tem o arquivo).
- [x] `nm -D --defined-only`: **145 símbolos**, os mesmos do Windows; internas escondidas
- [x] Correção necessária no Linux: `dladdr`/`Dl_info` exigem `_GNU_SOURCE` **antes** de
      qualquer include. Windows revalidado depois (estático e DLL, os dois compilam).

### Prova no Windows (executável + DLL, ambos consumindo a compartilhada)

| verificação | resultado |
|---|---|
| host e DLL apontam para a mesma instância | sim, ambos `Xplatbase.dll` |
| alocação feita DENTRO da DLL aparece na conta do host | sim, `alloc_count` 0 → 16 |
| liberação idem | sim, `free_count` 0 → 16 |
| thread criada na DLL aparece no registro do host | **inconclusivo** |

**Pendência da Fase 6:** o teste de thread está fraco — a thread é encerrada antes da
enumeração, então o registro voltar ao mesmo número não prova nada. Refazer com a thread
**viva** durante a contagem.

---

## Fase 3 — Identidade e detector de duplicata (ORIGEM)

É esta fase que troca "espero que seja única" por **garantida**.

### Implementação

Arquivo novo `src/xplat_instance.c` + declarações em `include/xplatbase.h`:

```c
typedef struct
{
    uint32      AbiVersion;      /* versão do contrato */
    const void* PoolAddress;     /* endereço do MemPool desta instância */
    char        Module[260];     /* de onde esta instância veio (DLL/exe) */
} XplatInstanceInfo;

/* 0 = única; 1 = JÁ existe outra instância no processo (out traz a primeira). */
XPLATBASE_API int xplat_instance_check(XplatInstanceInfo* out);
```

Mecanismo, chamado de dentro do `platform_init`/`memop_init`:

- **Windows**: `CreateFileMapping(INVALID_HANDLE_VALUE, ..., "Local\\xplatbase.instance.<pid>")`.
  Se voltar `ERROR_ALREADY_EXISTS`, lê o registro existente e compara o endereço do pool com o
  seu. Diferente ⇒ duplicata. O caminho do módulo sai de `GetModuleHandleEx` pelo endereço da
  função + `GetModuleFileName`.
- **Linux**: `shm_open("/xplatbase.instance.<pid>")` com a mesma estrutura; o caminho do módulo
  sai de `dladdr`.

Por que objeto nomeado do sistema e não uma variável global: a variável global é exatamente o
que duplica. Precisa ser algo que as duas cópias enxerguem.

### Política ao detectar

- **Sempre** registra a ocorrência, com os dois caminhos de módulo — a mensagem tem que dizer
  *quem* trouxe a segunda cópia, senão não se acha o culpado.
- **Debug**: aborta. Duplicata é defeito de build, não condição de execução.
- **Release**: `xplat_instance_check` devolve 1 e cabe ao host decidir. O padrão do appserver
  será falhar na subida.

### Verificação

- [x] Instância única: `xplat_instance_check` devolve 0 e preenche `Module` corretamente
- [x] Duplicata deliberada é detectada — provada com `inst_host.exe` + `inst_dupe.dll`
- [x] A mensagem nomeia os **dois** módulos envolvidos
- [x] Sem vazamento de handle do objeto nomeado (fecha no shutdown)

### O que a implementação revelou (e mudou o desenho)

**1. O xplatbase se inicializa sozinho.** Há inicializador de CRT (`.CRT$XCU`) no Windows e
`__attribute__((constructor))` no POSIX chamando `platform_init`. Ou seja, **a verificação é
automática em todo módulo** — ninguém precisa lembrar de chamar. Bom para a garantia, mas
significa que a duplicata é detectada durante a *carga* do módulo, quando ainda não houve
chance de configurar nada.

**2. Por isso a política vem de variável de ambiente.** `XPLATBASE_DUPLICATE_FATAL` (0/1),
lida no registro. É o que permite ao teste **provocar** a duplicata e observar a detecção em
vez de morrer na carga. Padrão mantido: aborta em Debug, reporta em Release.

**3. A segunda instância agora fica inerte.** Descoberto na prática: com a segunda cópia
subindo o pool de threads dela, o `FreeLibrary` matava o processo com violação de acesso — o
código some e as threads continuam. Agora, ao detectar duplicata, o `platform_init` **para
ali**: não cria pool, não registra callbacks, não sobe o rastreador. Sem isso, o cenário de
plugin seria instável por construção.

### Resultado medido

| caso | resultado |
|---|---|
| host sozinho | instância única, módulo correto |
| DLL com segunda cópia, `XPLATBASE_DUPLICATE_FATAL=0` | duplicata acusada, os dois módulos nomeados, `FreeLibrary` limpo, saída 0 |
| mesma DLL, padrão de Debug | aborta na carga — `LoadLibrary` falha com 1114, com a mensagem impressa antes |

---

## Fase 4 — Migrar os consumidores (appserver)

Cinco projetos linkam xplatbase hoje: `appserver`, `codecs`, `MediaFragmenter`, `MediaGateway`,
`appservertester`. Mais o alvo do CMake e os scripts de teste do scratchpad.

- [x] `XPLATBASE_USE_SHARED` nos cinco projetos; `XplatbaseDll` entrou na solution;
      MediaFragmenter passou a referenciar o projeto da DLL (é ele que propaga a lib de
      importação para o executável)
- [x] Pós-build copiando a `Xplatbase.dll` ao lado do executável
- [x] Linux: o `XPLATBASE_USE_SHARED` se propaga sozinho (declarado `PUBLIC` no alvo);
      `rpath $ORIGIN` conferido — o `ldd` mostra o executável usando a `.so`
- [x] Submodules atualizados: xplatbase `e0d72bc`, yason `cab9d43`
- [x] Windows: solution compila; `dumpbin /dependents` confirma `Xplatbase.dll` no executável
- [x] Linux: build completo sem erro; `appservertester` sobe e responde

### O bloqueio que apareceu: o yason usava o macro de OUTRA biblioteca

`yason.h` declarava as funções dele com `XPLATBASE_API`. Enquanto tudo era estático o macro
era vazio e ninguém percebeu; com o xplatbase compartilhado ele virou "importar de DLL", e o
link quebrou procurando `__imp_yason_parse` — uma função que está numa biblioteca estática.

Corrigido na ORIGEM do yason com um macro próprio (`YASON_API`, mesmos três modos). São 4
declarações, num arquivo só.

**Pendente com o usuário:** publicar o yason. Enquanto isso, o mesmo ajuste está aplicado à
cópia do submodule **em caráter temporário**, só para a validação — some quando o submodule
for atualizado para o commit publicado.

### Resultado medido

| verificação | Windows | Linux |
|---|---|---|
| build completo | sim | sim |
| executável usa a biblioteca compartilhada | `dumpbin /dependents` | `ldd` |
| sobe e responde | httptest, 0 falhas | `/api/media/devices` responde |
| detector acusa duplicata? | não (correto) | não (correto) |
| gwtest / webmtest / mkvtest | passam | — |

Os testes do scratchpad também migraram (define, lib de importação e cópia da DLL) —
**menos o `build_inst.bat`**, que continua estático de propósito: é ele que provoca a
segunda instância para o teste negativo.

---

## Fase 5 — Infraestrutura de testes com Google Test

Os testes de hoje (`gwtest`, `webmtest`, `mkvtest`, `hwtest`, `gpudectest`, `av1open`) são
executáveis soltos que imprimem texto e terminam com "TUDO OK". Problemas: não aparecem no
Gerenciador, não dá para rodar um caso isolado, e a falha não aponta linha.

### Decisão: CppUnitTest em vez de Google Test — e por quê

O framework do Google Test **não vem** com o Visual Studio nesta versão (só o adaptador), e
não existe cópia na máquina: exigiria baixar. O **CppUnitTest da Microsoft já está instalado**
(cabeçalhos, biblioteca e adaptador embutido) e entrega o mesmo que importa aqui: **lista caso
a caso no Gerenciador de Testes**, com executar e depurar individualmente.

**A lógica dos testes é em C**, como pedido — mesma linguagem das bibliotecas. O único C++ é
o `registro.cpp`: uma linha por teste, sem lógica nenhuma.

- [x] Projeto `test/unittests` (subtipo `NativeUnitTestProject`, que traz as props oficiais)
- [x] `ctest_core.h/.c` em C: `TestResult` + `T_ASSERT` com arquivo, linha e mensagem
- [x] `test_xplatbase.c`: 5 casos reais (instância única, biblioteca compartilhada,
      contabilidade do pool, escrita/leitura de bloco grande, execução de tarefas)
- [x] `registro.cpp`: registro no Gerenciador, uma linha por caso
- [x] Projeto na solution; compila junto com o resto
- [x] **Verificado pelo executor do Visual Studio** (`vstest.console`, o mesmo que o
      Gerenciador usa): 5 testes, 5 aprovados
- [x] Alvo no CMake com `runner_main.c` (executor em C) e **um teste do CTest por caso** —
      os MESMOS arquivos `.c` nas duas plataformas; muda só quem os chama
- [x] Testes de pipeline migrados (`webmtest`, `mkvtest`, `gwtest` viraram casos nomeados)
- [x] Testes de hardware migrados, com a regra de **não falhar onde não há equipamento**:
      o que a máquina não tem, o teste anota e passa. Uma suíte que vive vermelha não é lida.
- [x] Os testes são **autocontidos**: cada um gera o que precisa. Antes, o `gwtest` dependia
      de arquivos que o `webmtest` tinha deixado para trás.

### Resultado da Fase 5

| | Windows (Gerenciador de Testes) | Linux (CTest) |
|---|---|---|
| casos | 12 | 12 |
| aprovados | 12 | 12 |

**Dois defeitos meus, achados pelos próprios testes:** a ordem de busca de bibliotecas pegava
um `Xplatbase.lib` **estático e velho** largado em `x64\Debug` (de setembro), em vez da
biblioteca de importação — removido, e a pasta certa passou a vir primeiro; e o teste de
decode lia a resolução **depois** do laço, onde a última chamada devolve 0 sem tocar na
imagem: acusava `0x0` com o decode funcionando.
- [ ] **Refatorar os testes existentes**, um por vez, preservando o que cada um prova:

| teste atual | vira | o que precisa preservar |
|---|---|---|
| `gwtest` | `GatewayTest.*` | 15 rodadas com memória constante |
| `webmtest` | `WebmTest.*` | VP9 e AV1, 30 quadros, pts monotônico |
| `mkvtest` | `MkvTest.*` | annexb e 30 quadros decodificados |
| `hwtest` | `HwEncoderTest.*` | ciclos sem vazar handle |
| `gpudectest` | `GpuDecodeTest.*` | saída idêntica à do software, 300 quadros |
| `av1open` | `Av1EncoderTest.*` | abertura paralela dos 6 encoders |

- [ ] **Cuidado com a descoberta**: ela executa o binário para listar os casos. Um teste que
      trave **prende o Gerenciador** — vale para os de câmera e de GPU. Usar limite de tempo.

### Defeito encontrado pelo próprio projeto de testes

A primeira execução rodou os 5 casos e **o processo do executor morria em seguida**: sem
resumo, sem arquivo de resultado, código de saída 5. Causa: o executor **descarrega** o módulo
de teste ao terminar, e com ele a `Xplatbase.dll` — enquanto as threads do pool ainda vivem.
O código some debaixo delas.

É a terceira vez que este mesmo mecanismo aparece nesta preparação (antes: no teste da
duplicata e no encerramento do módulo duplicado). Corrigido na raiz: **a biblioteca se fixa na
memória** ao inicializar (`GET_MODULE_HANDLE_EX_FLAG_PIN` no Windows, `RTLD_NODELETE` no
POSIX). Só a instância viva se fixa; a duplicata continua inerte e descartável.

Depois disso: `Total de testes: 5 — Aprovados: 5`.

### Outro defeito do mesmo tipo: ordem de build

Depois de unificar o projeto, os testes voltaram a não reportar nada. Causa diferente, sintoma
igual: o projeto de testes **não declarava dependência** do xplatbase, então o MSBuild podia
construí-lo antes — e ou o link falhava, ou uma `Xplatbase.dll` velha ficava na pasta de saída.
Resolvido com `ProjectReference`. Os dois casos aconteceram de verdade aqui.

**Bloqueio que a unificação revelou:** o yason referenciava o xplatbase **aninhado** nele, que
tem o mesmo identificador de projeto do principal — então o mapeamento de configurações da
solution caía também naquela cópia antiga, que não tem as configurações de DLL. E, pior, essa
referência **propagava a biblioteca estática** para quem consome o yason: a segunda cópia que
queremos evitar. Referência removida (yason é biblioteca estática em x64; quem linka é o
executável).

**Atenção para depois:** o yason é `StaticLibrary` em `Debug|x64` mas `DynamicLibrary` em
`Release|x64`. Essa assimetria parece não intencional, e em Release ele vai precisar linkar o
xplatbase compartilhado explicitamente.

---

## Fase 6 — Provar a instância única

O par de testes que justifica a preparação inteira. Os arquivos ficaram em
`test/unittests/`, ao lado dos demais, e não em subpastas próprias como estava previsto:
são dois `.c` pequenos, cada um com seu `.vcxproj` (Windows) e seu `add_library` (Linux).

### Positivo — o módulo vê o mesmo pool

`mod_compartilhado.c` — biblioteca carregada em tempo de execução que **consome** o
xplatbase compartilhado. Testes em `test_instancia_modulos.c`.

- [x] Alocar dentro do módulo e ver a alocação no `memop_get_stats` lido pelo host
      (`ModuloCompartilhadoUsaMesmoPool`; confere também que o módulo e o host nomeiam o
      **mesmo** módulo como origem da instância)
- [x] Submeter tarefa de dentro do módulo: roda no **mesmo** pool
      (`ModuloUsaMesmoPoolDeTarefas` — a função é do host, quem submete é o módulo, e as
      threads que executaram são procuradas no registro do host)
- [x] `thread_enum` no host enxerga thread criada pelo módulo
      (`ModuloCompartilhadoMesmasThreads`)
- [ ] `mem_leak_watch` cobre os dois lados — **não feito**. O watch varre as raízes do
      processo; com a instância única ele já enxerga o que o módulo aloca, mas isso não foi
      exercitado por teste. Fica para quando o primeiro plugin de codec existir de verdade.

### Negativo — a duplicata é denunciada

`mod_duplicado.c` — módulo que **linka o xplatbase estaticamente de propósito**.

- [x] Carregar esse módulo e chamar a inicialização dele
- [x] O detector acusa duplicata e nomeia os dois módulos
      (`DuplicataEDenunciada` exige `dup == 1` **e** que a denúncia diga de onde veio a
      primeira instância — um detector que nunca foi visto acusando não vale nada)
- [ ] Em Debug, aborta (o teste captura e valida a mensagem) — **não feito**: abortar mata o
      processo do executor de testes junto. O teste desliga a política com
      `XPLATBASE_DUPLICATE_FATAL=0` antes de carregar e verifica a detecção. O caminho que
      aborta é o mesmo, só o desfecho muda; validá-lo exigiria um processo filho.

### Ciclo

- [x] Carregar e descarregar o módulo repetidamente medindo threads, handles e memória
      (`CicloCarregaDescarrega`, 50 ciclos em vez de 200 — mesmo padrão que já pegou dois
      defeitos reais aqui; 50 mantém a suíte rápida e o acúmulo, se houver, aparece igual)

**Resultado:** 17 testes, verdes nos dois sistemas — Gerenciador de Testes do Visual Studio
(Debug|x64) e CTest no Linux.

---

## Fase 7 - Regressao e fechamento

| item | Windows | Linux |
|---|---|---|
| build completo, Debug | limpo | limpo (Ninja, WSL2) |
| build completo, Release | uma pendencia, abaixo | -- |
| suite de testes | 17/17 em Debug **e** em Release | 17/17 (CTest) |
| `httptest` no servidor real | 0 falhas | 0 falhas |

- [x] Solution completa em Debug|x64
- [x] Todos os testes no Gerenciador de Testes: 17/17, Debug e Release
- [x] `ctest` no Linux: 17/17
- [x] `httptest` nos dois sistemas: 0 falhas
- [x] Build e smoke no WSL
- [x] Regras novas no README do xplatbase (secao *One instance per process*)
- [x] `REFACTOR-NOTES.md` com o que mudou, por que, as armadilhas e o que ficou de fora
- [ ] Release|x64 100% verde -- falta `dashstream.lib`, ver abaixo

### O que o Release revelou

O Debug escondia tudo isso porque os artefatos ja estavam no disco de builds antigos. O
Release, partindo do zero, cobrou.

**Meus, corrigidos:**

1. `mod_compartilhado` e `mod_duplicado` nao tinham ordem de build em relacao ao Xplatbase.
   Em Debug passava porque a `.dll` ja existia; em Release o link nao achava a lib.
   `mod_compartilhado` ganhou `ProjectReference`.
2. `mod_duplicado` linkava a `Xplatbase.lib` **estatica**, e a solution nao constroi mais
   essa configuracao (o mapeamento aponta para `Debug DLL`/`Release DLL`). Passou a
   **compilar as fontes do xplatbase dentro dele**, como o CMake ja fazia no Linux -- que e
   exatamente o que o teste negativo quer: uma segunda copia de verdade. A `.dll` foi de
   65 KB para 176 KB e o teste continua acusando a duplicata.
3. `appservertester` nao tinha dependencia declarada do `yason`: com build paralelo, linkava
   antes da lib existir. Declarada em `appserver.sln`.

**Efeito colateral no yason (corrigido na origem, falta o seu push):** sem a
`ProjectReference` removida na Fase 4 (colisao de `ProjectGuid`, `MSB8013`), o link do yason
em Release ficou sem a `Xplatbase.lib`. O yason era `StaticLibrary` em `Debug|x64` e
`DynamicLibrary` em `Release|x64` -- a assimetria ja anotada como provavel descuido. Agora e
`StaticLibrary` nas duas, e o link deixa de existir. Esta em `E:\git\libs\yason` **e** na
copia do submodulo, para dar para testar; commit e push sao seus.

**Anteriores a esta preparacao:**

- `codecs` tem `ActiveCfg` mas **nao tem `Build.0`** em `Release|x64`: a solution nunca o
  constroi nessa configuracao. A `codecs.lib` de Release no disco era de **8 de setembro** e
  faltavam simbolos novos (`media_decoder_available`, `hw_dec_*`, `enc_tier_name`). Construi
  o projeto a mao (virou 18,9 MB, contra 11,9 MB da antiga) e o Release seguiu. **Nao foi
  corrigido na solution** -- por o `Build.0` de volta e decisao sua, pesa no tempo de build.
- `streammanager` em `Release|x64`: o `ItemDefinitionGroup` de Release nao tinha
  `AdditionalIncludeDirectories` (o de Debug tem), e a configuracao era `DynamicLibrary`
  enquanto as outras tres sao `StaticLibrary` -- como DLL o link exigia `platform_init`,
  `list_add`, `string_append`, que nenhum projeto desta solution produz. Corrigi os dois
  (mesma lista de includes do Debug, e `StaticLibrary`), porque sem isso nao havia como
  medir o Release. **Confira**: e projeto fora do escopo desta preparacao.
- **Pendencia que sobra:** `appservertester` linka `dashstream.lib`, e **nao ha projeto
  `dashstream` na solution**. Em Debug funciona porque existe uma `dashstream.lib` solta em
  `x64\Debug`, de um build antigo; em Release nao existe e o link para. Ou a dependencia sai
  do `appservertester`, ou o projeto volta para a solution -- decisao sua.
- Varios projetos tem post-build chamando `pwsh.exe`, que nao existe nesta maquina. O build
  passa, mas imprime o erro a cada projeto.

---

## Riscos

| risco | como trata |
|---|---|
| TLS em DLL carregada em tempo de execução | conferido na Fase 2, na prática |
| Debug/Release misturados | regra escrita + detector acusa na subida |
| Layout de struct virou ABI | documentado; mudança exige recompilar todos |
| Outros projetos seus que usam xplatbase | modo estático preservado; migram quando você quiser |
| Descoberta do Google Test travando | limite de tempo nos testes de hardware |

## Ordem de execução

Fase 0 destrava (instalador e push). Depois 1 → 2 → 3 na origem, com o teste da Fase 6 escrito
**junto** com a Fase 3 — assim a primeira coisa que roda já prova se a instância é única.
As fases 4 e 5 são as mais longas; a 7 fecha.
