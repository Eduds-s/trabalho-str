# TheWorstAçaí — RTOS didático no STM32G474RE

Trabalho final de Sistemas de Tempo Real. Partimos do MiROS (mini RTOS didático do Miro Samek), portamos para a NUCLEO-G474RE em **C++20** e vamos evoluindo ele fase por fase. Também rodamos no **Renode**, com o mesmo firmware: nada no código depende do simulador.

| Fase | Tema | Status |
|------|------|--------|
| 1 | `yield()` e semáforo sem busy-waiting | ✅ |
| 2 | Trace com SEGGER SystemView | ✅ |
| 3 | Produtor/consumidor | ✅ |
| 4 | Tarefas periódicas + escalonador Rate Monotonic | ✅ |
| 5 | Protocolo de compartilhamento de recursos | ⏳ |

**Ambiente:** NUCLEO-G474RE a 170 MHz · STM32CubeIDE (GCC, `-std=c++20`) · Renode 1.17 · SEGGER SystemView.

```
Core/Src/miros.cpp         kernel: escalonador, delay, yield, semáforo, PendSV
Core/Inc/miros.h           API do kernel + IDs dos eventos de trace
Core/Src/main.cpp          aplicação: produtor/consumidor como tarefas periódicas (fases 3 e 4)
Core/Src/stm32g4xx_it.cpp  tabela de vetores + SysTick_Handler
Core/Config, Core/SEGGER   SystemView/RTT (configuração + código da SEGGER)
sysview/                   arquivo de descrição do nosso OS para o SystemView
nucleog474re.repl          placa para o Renode
str-renode.resc            script da simulação
```

---

## Kernel

- **TCB** (`OSThread`): `sp` (topo da pilha salvo, sempre o 1º campo), `timeout` (contador do delay), `name` e, desde a fase 4, os campos de tarefa periódica (`job`, `period`, `nextRelease`, `prio`, `deadlineMisses`).
- **Threads** ficam em `OS_thread[33]`. O índice 0 é a **idle**, criada no `OS_init()`; as da aplicação vêm depois. Quem está pronto fica no bitmask `OS_readySet`.
- **Escalonador** (`OS_sched`): até a fase 3 era round-robin; na fase 4 virou **Rate Monotonic** (ver abaixo). Se a escolhida for outra thread, pede a PendSV e retorna `true`.
- **Tick:** SysTick a `TICKS_PER_SEC = 10000` (100 µs). O `OS_tick()` avança o relógio do kernel, acorda quem terminou o `OS_delay` e libera as tarefas periódicas.
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
| `OS_delay` / `wait` bloqueando / `OS_waitNextPeriod` | `OnTaskStopReady` (motivo 1 = Delay, 2 = Semáforo, 3 = Período) |
| PendSV → `OS_traceContextSwitch` | `OnTaskStartExec` ou `OnIdle` |
| `SysTick_Handler` | `RecordEnterISR` na entrada; na saída, `RecordExitISRToScheduler` se o `OS_sched()` pediu troca, senão `RecordExitISR` |
| início de `OS_delay`, `yield`, `wait`, `signal` | evento de API, IDs 32–35 |
| `OS_tick` (fase 4) | evento `DeadlineMiss` (ID 36) quando uma tarefa periódica perde o deadline |

Os eventos de API existem porque um `wait()` com crédito ou um `signal()` sem ninguém esperando não mudam o estado de nenhuma thread e não apareceriam no trace.

A idle é criada antes do `Start`, então o nome dela não vai para o trace. Não faz falta: o SystemView reconhece a idle pelo evento `OnIdle`.

O arquivo `sysview/SYSVIEW_TheWorstAcai.txt` faz os IDs 32–36 aparecerem com nome (`OS_delay Ticks=10`, `DeadlineMiss Task=produtor`), traduz o motivo do bloqueio e, com `Option ReversePriority`, faz o SystemView entender que prioridade menor é mais importante (na fase 4 a prioridade enviada é o período, ver abaixo). Ele tem que ser **copiado** para `C:\Program Files\SEGGER\SystemView\Description\`.

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

## Fase 4 — Tarefas periódicas e Rate Monotonic

Escolhemos o **Rate Monotonic (RM)**: prioridade fixa, e quanto menor o período, maior a prioridade. É o mais simples dos três que o enunciado aceita (só depende do período) e combina com a fase 5, já que herança e teto de prioridade são definidos em cima de prioridades fixas.

### Tarefa periódica (run-to-completion)

A aplicação não escreve mais `while(1)`: ela entrega uma função que faz **um job** e retorna. Quem repete é o kernel:

```cpp
void OS_periodicLoop() {          // corpo de toda thread periódica
    while (1) {
        OS_curr->job();           // um job, até o fim
        OS_waitNextPeriod();      // dorme até o próximo período
    }
}

rtos::OSThread_startPeriodic(&produtor, &main_produtor, "produtor",
                             PERIODO_PRODUTOR, stack_produtor, sizeof(stack_produtor));
```

O `OSThread_startPeriodic` guarda `job`, `period` e `nextRelease` no TCB e cria a thread com o `OSThread_start` de sempre, começando no `OS_periodicLoop`. Não tem alocação dinâmica: TCB e pilha continuam sendo globais da aplicação. A função não pode simplesmente "retornar" para terminar, porque o LR inicial da pilha é falso e daria HardFault; por isso o laço fica no kernel.

### Modelo de tempo (independente do hardware)

- **Relógio por software:** `OS_tickCount` é incrementado no `OS_tick()`. Todo o resto (liberação, deadline) usa só esse contador. O SysTick só serve de gatilho, e as únicas partes ligadas ao STM32 são o `OS_onStartup()` (configura o SysTick) e o `SysTick_Handler` (chama o `OS_tick()`). Para portar o kernel, basta trocar quem chama o `OS_tick()`.
- **Tempo absoluto:** ao terminar um job, o `OS_waitNextPeriod` faz `nextRelease += period` em vez de "esperar `period` ticks a partir de agora". Assim o período não escorrega quando o job demora.
- **Liberação:** no `OS_tick`, quando `OS_tickCount == nextRelease` a thread volta para o `readySet`. A comparação é por igualdade de propósito: o `nextRelease` só muda quando o job termina, então uma thread ainda rodando (ou bloqueada num semáforo) nunca é liberada por engano.
- **Job atrasado:** se o job terminar depois da próxima liberação, o `OS_waitNextPeriod` não bloqueia e o próximo job roda em seguida.

### Escalonador RM

No `OS_onStartup()`, antes de ligar o SysTick, o kernel **ordena** `OS_thread[1..N]` por período crescente (a idle fica no 0) e refaz o `readySet`. Depois disso, **índice menor = maior prioridade**, e o `OS_sched()` só precisa achar o bit mais baixo do `readySet`:

```cpp
uint32_t bit = OS_readySet & ~(OS_readySet - 1U);   // isola o bit mais baixo
OS_currIdx = (uint8_t)(32U - __CLZ(bit));            // índice da pronta mais prioritária
```

Isso tem tempo constante, independente do número de threads. A ordenação só pode ser feita antes de qualquer thread rodar (os bits do `readySet`/`waitSet` são índices), e nenhuma thread é criada depois do `OS_run()`.

Efeitos que vêm junto:
- o `signal()` do semáforo acorda o bit mais baixo do `waitSet`, ou seja, a thread **mais prioritária** que estava esperando;
- o `signal()` também chama o `OS_sched()`: se a thread acordada for mais prioritária que a atual, ela **preempta na hora**, sem esperar o próximo tick;
- o `SysTick_Handler` continua chamando o `OS_sched()`, que é o que garante a preempção quando o **tempo** libera uma tarefa.

### Deadlines

O deadline de cada job é o fim do período (`nextRelease + period`). O kernel confere quando o job termina, no `OS_waitNextPeriod`: se o relógio já passou do deadline, incrementa `deadlineMisses` no TCB e grava o evento `DeadlineMiss` no SystemView. Como cada job termina uma vez só, cada atraso é contado uma vez, inclusive quando a tarefa está atrasada há vários períodos.

A limitação é que um job que nunca termina (travado para sempre) não é contado. Isso seria um deadlock, e aparece na timeline de outro jeito: a tarefa some.

### No SystemView

- A prioridade enviada no `SendTaskInfo` é o **próprio período** (com `Option ReversePriority` no `.txt`), então a lista de tarefas aparece na ordem do RM.
- A espera pelo próximo período aparece como *Task Block (Periodo)*, separada do `OS_delay` (*Delay*) e do semáforo (*Semaforo*).

### Análise do trace

Trace com o produtor em **1 tick** (100 µs, mais prioritário) e o consumidor em **2 ticks** (200 µs), fila de 32. Foram gravados 54 ms até o buffer encher (o último evento é o aviso de *Overflow* do SystemView, marcando onde a gravação parou).

- O produtor é mais rápido, então a fila enche em ~6,6 ms. Daí em diante ele bloqueou **239 vezes** pelo semáforo, **todas com a fila em exatamente 32** itens, e a ocupação nunca passou de 32.
- Nas **239** vezes, o `signal()` do consumidor acordou o produtor e ele **preemptou o consumidor na hora** (`OSSemaphore_signal` → *Task Run* do produtor, sem SysTick no meio). É a preempção por prioridade funcionando.
- O consumidor terminou todos os jobs dentro do período: 270 bloqueios por *Periodo*, nenhum por semáforo.
- O produtor quase sempre está atrasado (bloqueado esperando vaga), então na maior parte das vezes emenda um job no outro em vez de esperar o período.

---

## Renode

- **`nucleog474re.repl`:** clocks em 170 MHz, batendo com o `SystemClock_Config()`. RCC, PWR e FLASH são emulados por scripts Python que devolvem os bits de "pronto" que a HAL espera.
- **`str-renode.resc`:** carrega `Debug/trabalho-str.elf`, abre o GDB na porta 3333 e deixa `LogFunctionNames False`. Com `True` a simulação fica muito lenta, porque a idle chama `OS_onIdle()` em loop.

---

## Uso de IA generativa

Usamos o **Claude Opus 5.5** (via Claude Code) ao longo do projeto como auxílio de pesquisa, como revisor para encontrar erros e, em momentos pontuais, como ferramenta para acelerar o desenvolvimento de certas funções.
