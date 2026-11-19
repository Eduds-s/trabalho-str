# Registro de alterações feitas com auxílio de IA

> Data: 18/11/2026 · Fase 2 · Ferramenta: Claude Opus 5.5 (Claude Code)

## Alteração que ficou no projeto

### `str-renode.resc`: `LogFunctionNames True` → `False`

```diff
-sysbus.cpu0 LogFunctionNames True
+# LogFunctionNames True loga TODA entrada de funcao (a idle chama OS_onIdle em loop):
+# a simulacao fica minutos por segundo simulado. Ligue so para depurar.
+sysbus.cpu0 LogFunctionNames False
```

**Motivo:** com `True`, o Renode escreve no log uma linha para **cada entrada de função** executada pela CPU simulada. A thread idle (`main_idleThread`) chama `OS_onIdle()` num laço infinito, então o log recebe milhões de linhas e a simulação fica muito lenta.

**Medição** (rodando o Renode sem interface gráfica):

| Configuração | Tempo simulado | Tempo real |
|--------------|---------------|------------|
| `LogFunctionNames True` | 3 s | não terminou em 200 s |
| `LogFunctionNames False` | 10 s | 16 s |

Para depurar o fluxo de funções, basta voltar para `True` temporariamente.

**Isso não afeta o firmware:** é só uma opção de log do simulador.

## Alterações desfeitas

Um script Python do Renode (`renode/sysview_rtt.py`) que esvaziava o buffer RTT a cada SysTick chegou a ser criado e testado. A equipe decidiu **remover**: ele só funcionaria no simulador, e a ideia é manter um fluxo reproduzível no hardware real (dump de memória). O script, as linhas que o carregavam no `.resc`, a pasta `trace/` e a linha `/trace/` no `.gitignore` foram apagados. O `.gitignore` está idêntico ao do último commit.

## O que **não** foi alterado

- Nenhum arquivo do firmware (`Core/`, `Drivers/`, linker scripts).
- **`nucleog474re.repl` não foi alterado pela IA.** A modificação dele que aparece no `git status` é de 17/11/2026 12:00, antes desta sessão. O mesmo vale para `miros.h`, `miros.cpp`, `.settings/*` e para a linha `$binpath` do `.resc`.

## Observação útil para o dump de memória (nada foi implementado)

Ao planejar o dump via GDB, considerem que o buffer do SystemView (`SEGGER_SYSVIEW_RTT_BUFFER_SIZE = 32768`) usa o modo `SEGGER_RTT_MODE_NO_BLOCK_SKIP`. **Quando ele enche, os eventos novos são descartados.** Ou seja, um dump captura no máximo os primeiros 32 KB de eventos desde o `SEGGER_SYSVIEW_Start()`. Com o firmware atual, isso dá alguns minutos de execução.
