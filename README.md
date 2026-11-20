# TheWorstAçaí — RTOS didático no STM32G474RE

Trabalho final de Sistemas de Tempo Real. Partimos do MiROS (mini RTOS didático do Miro Samek), portamos para a NUCLEO-G474RE em **C++20** e vamos evoluindo ele fase por fase. Também rodamos no **Renode**, com o mesmo firmware: nada no código depende do simulador.

| Fase | Tema | Status |
|------|------|--------|
| 1 | `yield()` e semáforo sem busy-waiting | ✅ |
| 2 | Trace com SEGGER SystemView | ✅ |
| 3 | Produtor/consumidor | ✅ |
| 4 | Tarefas periódicas + escalonador | ⏳ |
| 5 | Protocolo de compartilhamento de recursos | ⏳ |

**Ambiente:** NUCLEO-G474RE a 170 MHz · STM32CubeIDE (GCC, `-std=c++20`) · Renode 1.17 · SEGGER SystemView.

```
Core/Src/miros.cpp         kernel: escalonador, delay, yield, semáforo, PendSV
Core/Inc/miros.h           API do kernel + IDs dos eventos de trace
Core/Src/main.cpp          aplicação: produtor/consumidor (fase 3)
Core/Src/stm32g4xx_it.cpp  tabela de vetores + SysTick_Handler
Core/Config, Core/SEGGER   SystemView/RTT (configuração + código da SEGGER)
sysview/                   arquivo de descrição do nosso OS para o SystemView
nucleog474re.repl          placa para o Renode
str-renode.resc            script da simulação
```

---

## Kernel

- **TCB** (`OSThread`): `sp` (topo da pilha salvo), `timeout` (contador do delay) e `name`.
- **Threads** ficam em `OS_thread[33]`. O índice 0 é a **idle**, criada no `OS_init()`; as da aplicação vêm depois. Quem está pronto fica no bitmask `OS_readySet`.
- **Escalonador** (`OS_sched`): round-robin entre as threads prontas, ou a idle se não houver nenhuma. Se a escolhida for outra thread, pede a PendSV e retorna `true`.
- **Tick:** SysTick a `TICKS_PER_SEC = 10000` (100 µs). O `OS_tick()` decrementa os timeouts e acorda quem zerou.
- **API:** `OS_delay(ticks)`, `yield()`, `OSSemaphore::wait()`/`signal()`. O semáforo tem um contador e um bitmask `waitSet`: quem bloqueia sai do `readySet`, sem busy-waiting.

### Pilha e troca de contexto

O `OSThread_start` monta a pilha de cada thread (2 KB) como se ela já tivesse sido interrompida uma vez. Assim a primeira entrada pela PendSV funciona igual às outras:

```
alto  ┌──────────────┐
      │ xPSR, PC, LR │ ┐ frame empilhado/desempilhado
      │ R12, R3..R0  │ ┘ pelo hardware na exceção
      ├──────────────┤
      │ R11..R4      │ ← salvos pela PendSV (software)
      ├──────────────┤ ← me->sp
      │ 0xDEADBEEF…  │   resto da pilha (para ver o uso)
baixo └──────────────┘
```

```
SysTick (prioridade mais alta)        PendSV (prioridade mais baixa)
  OS_tick()                             PUSH r4-r11 ; OS_curr->sp = SP
  OS_sched() → pede PendSV  ───────►    SP = OS_next->sp ; OS_curr = OS_next
  sai da ISR                            OS_traceContextSwitch()  (trace)
                                        POP r4-r11 ; BX LR → nova thread
```

A PendSV tem a menor prioridade, então a troca nunca acontece no meio de outra ISR.

---

## Fase 2 — Trace com SystemView

### Coleta: single-shot + dump de memória

A NUCLEO tem ST-LINK, não J-Link, então não dá para gravar continuamente via RTT. Usamos a gravação **single-shot** do manual do SystemView (UM08027, 3.15.2):

1. o `SEGGER_SYSVIEW_Start()` liga a gravação, e os eventos vão para um buffer RTT de 32 KB na RAM;
2. paramos a CPU, salvamos o buffer e abrimos no SystemView (*File → Load Recording*).

```gdb
target remote :3333                     # Renode (str-renode.resc) ou ST-Link
p _SEGGER_RTT.aUp[1]                    # confere sName = "SysView" e o WrOff
dump binary memory trace.SVDat _SEGGER_RTT.aUp[1].pBuffer _SEGGER_RTT.aUp[1].pBuffer+_SEGGER_RTT.aUp[1].WrOff
```

Pelo STM32CubeIDE também dá: aba **Memory** → Export → *RAW Binary*, a partir de `pBuffer`, com tamanho `WrOff`.

Quando o buffer enche, os eventos novos são descartados. No single-shot o SystemView **não** mostra evento de *Overflow* (ninguém esvazia o buffer), então a perda é conferida no debugger: `p _SYSVIEW_Globals.MainContext.DropCount` tem que dar 0. Por isso paramos antes de encher.

### Configuração

- **Timestamp:** `DWT->CYCCNT >> 4` (28 bits). O `ENABLE_DWT_CYCCNT = 1` liga o contador mesmo sem debugger, senão os timestamps saem zerados na placa.
- **ID da interrupção** lido do `ICSR`; base de RAM dos IDs em `0x20000000`.
- **Descrição do sistema:** `N=TheWorstAcai,O=TheWorstAcai,D=Cortex-M4`, `I#15=SysTick`.

### Instrumentação

| Onde | Evento |
|------|--------|
| `OSThread_start` | `OnTaskCreate` + `SendTaskInfo` |
| `OS_makeReady` (fim do delay, `signal`) | `OnTaskStartReady` |
| `OS_delay` / `wait` bloqueando | `OnTaskStopReady` (motivo 1 = Delay, 2 = Semáforo) |
| PendSV → `OS_traceContextSwitch` | `OnTaskStartExec` ou `OnIdle` |
| `SysTick_Handler` | `RecordEnterISR` na entrada; na saída, `RecordExitISRToScheduler` se o `OS_sched()` pediu troca, senão `RecordExitISR` |
| início de `OS_delay`, `yield`, `wait`, `signal` | evento de API, IDs 32–35 |

Os eventos de API existem porque um `wait()` com crédito ou um `signal()` sem ninguém esperando não mudam o estado de nenhuma thread e não apareceriam no trace.

A idle é criada antes do `Start`, então o nome dela não vai para o trace. Não faz falta: o SystemView reconhece a idle pelo evento `OnIdle`.

O arquivo `sysview/SYSVIEW_TheWorstAcai.txt` faz os IDs 32–35 aparecerem com nome (`OS_delay Ticks=10`) e traduz o motivo do bloqueio. Ele tem que ser **copiado** para `C:\Program Files\SEGGER\SystemView\Description\`.

### Análise da timeline

Na fase 2 a aplicação de teste tinha três threads, `blink1`, `blink2` e `blink3`, fazendo `OS_delay` de 10, 20 e 5 ticks. Na timeline aparecem o SysTick (ISR 15) a cada 100 µs, a idle ocupando a maior parte do tempo e as threads acordando nos múltiplos de 5 ticks (o `blink3` com mais frequência).

Num tick que acorda uma thread, a sequência é sempre:

```
ISR Enter (SysTick) → Task Ready (blinkX) → ISR Exit to Scheduler
  → Task Run (blinkX) → OS_delay → Task Block (Delay) → Idle
```

Nos ticks em que ninguém acorda, aparece só `ISR Enter` → `ISR Exit`.

**Sem perda de dados:** o dump é feito antes de o buffer encher (`WrOff` < 32768), com `DropCount` = 0.

---

## Fase 3 — Produtor/consumidor

### Fila

A fila é um **buffer circular estático**: um vetor global de `CAPACIDADE` (32) posições e dois índices.

```cpp
struct produto {
    uint16_t id;
};

produto fila[CAPACIDADE];   // memória reservada em tempo de link (.bss)
uint32_t head = 0;          // onde o produtor escreve
uint32_t tail = 0;          // de onde o consumidor lê
```

Pensamos primeiro numa fila encadeada com `malloc`, mas no microcontrolador isso não é uma boa: o `malloc` tem tempo variável, não é protegido entre threads no nosso kernel, fragmenta a memória e deixaria a fila sem tamanho máximo (o produtor nunca bloquearia). No buffer circular a ordem vem da posição no vetor, então não precisa de `next`/`previous`: os índices andam com `% CAPACIDADE` e voltam para 0 no fim.

### Semáforos

Os três são os `OSSemaphore` da fase 1, globais para as duas threads enxergarem:

| Semáforo | Inicial | Função |
|----------|---------|--------|
| `MutexFila` | 1 | exclusão mútua em `fila`, `head` e `tail` |
| `BloqProdutor` | `CAPACIDADE` | vagas livres: o produtor bloqueia quando zera (fila cheia) |
| `BloqConsumidor` | 0 | itens na fila: o consumidor bloqueia quando zera (fila vazia) |

```cpp
// produtor                          // consumidor
BloqProdutor.wait();                 BloqConsumidor.wait();
MutexFila.wait();                    MutexFila.wait();
fila[head] = item;                   item = fila[tail];
head = (head + 1) % CAPACIDADE;      tail = (tail + 1) % CAPACIDADE;
MutexFila.signal();                  MutexFila.signal();
BloqConsumidor.signal();             BloqProdutor.signal();
```

O semáforo de crédito vem **antes** do mutex. Ao contrário, o produtor poderia pegar o mutex e bloquear com a fila cheia segurando ele, e o consumidor nunca entraria para liberar espaço (deadlock).

O bloqueio é passivo: quem faz `wait()` sem crédito sai do `readySet` e vai para o `waitSet` do semáforo. O `signal()` passa o crédito direto para a thread acordada, então nenhum sinal se perde.

### Conferindo a ordem

O produtor numera os itens (`id` = 0, 1, 2…) e o consumidor confere se eles chegam em sequência. Se algum item fosse perdido, duplicado ou trocado de lugar, a variável global `erros` seria incrementada. Na demonstração ela fica em **0** (dá para acompanhar na aba Expressions do debugger).

### Cenários

Os cenários mudam só com os valores de `OS_delay` de cada thread:

| Delays | O que acontece |
|--------|----------------|
| produtor mais rápido | a fila enche e o **produtor bloqueia** (motivo Semáforo) até o consumidor liberar uma vaga |
| consumidor mais rápido | a fila esvazia e o **consumidor bloqueia** até chegar item novo |
| iguais | a fila fica entre 0 e 1 item e o consumidor bloqueia esperando cada item |

Num trace com o produtor em 2 ticks e o consumidor em 3, o produtor bloqueou 161 vezes pelo semáforo, **todas com a fila em exatamente 32 itens**. A ocupação nunca passou de 32 e as contas bateram (257 produzidos − 225 consumidos = 32 na fila). Na timeline, cada bloqueio aparece como `OSSemaphore_wait` → *Task Block (Semaforo)*, e a thread só volta a rodar depois do `OSSemaphore_signal` da outra.

---

## Renode

- **`nucleog474re.repl`:** clocks em 170 MHz, batendo com o `SystemClock_Config()`. RCC, PWR e FLASH são emulados por scripts Python que devolvem os bits de "pronto" que a HAL espera.
- **`str-renode.resc`:** carrega `Debug/trabalho-str.elf`, abre o GDB na porta 3333 e deixa `LogFunctionNames False`. Com `True` a simulação fica muito lenta, porque a idle chama `OS_onIdle()` em loop.

---

## Uso de IA generativa

Usamos o **Claude Opus 5.5** (via Claude Code) para revisar a fase 2. Já sabíamos o que precisava entrar e usamos a IA para conferir com o enunciado e o manual do SystemView o que faltava, além de ajudar na instrumentação do SysTick, nos eventos de API e neste README. O plano que ela propôs era mais complexo do que o necessário, e simplificamos.

Na fase 3 o código foi feito pela equipe. Usamos a IA só para discutir a definição da struct da fila (alocação estática em vez de `malloc`, buffer circular sem `next`/`previous`).
