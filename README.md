# TheWorstAçaí — RTOS didático no STM32G474RE

Trabalho final de Sistemas de Tempo Real. Partimos do MiROS (mini RTOS didático do Miro Samek), portamos para a NUCLEO-G474RE em **C++20** e evoluímos ele fase por fase até ter escalonamento Rate Monotonic e herança de prioridade. Também rodamos no **Renode**, com o mesmo firmware: nada no código depende do simulador.

| Fase | Tema | Status |
|------|------|--------|
| 1 | `yield()` e semáforo sem busy-waiting | ✅ |
| 2 | Trace com SEGGER SystemView | ✅ |
| 3 | Produtor/consumidor | ✅ |
| 4 | Tarefas periódicas + escalonador Rate Monotonic | ✅ |
| 5 | Mutex com Protocolo de Herança de Prioridade (PIP) | ✅ |

**Ambiente:** NUCLEO-G474RE a 170 MHz · STM32CubeIDE (GCC, `-std=c++20`) · Renode 1.17 · SEGGER SystemView.

```
Core/Src/miros.cpp         kernel: escalonador, tick, delay, yield, semáforo, mutex, PendSV
Core/Inc/miros.h           API do kernel, TCB e IDs dos eventos de trace
Core/Src/main.cpp          aplicação: demo da fase 5 (e produtor/consumidor comentado)
Core/Src/stm32g4xx_it.cpp  tabela de vetores + SysTick_Handler
Core/Config, Core/SEGGER   SystemView/RTT (configuração + código da SEGGER)
sysview/                   arquivo de descrição do nosso OS para o SystemView
nucleog474re.repl          placa para o Renode
str-renode.resc            script da simulação
```

---

## Arquitetura do kernel

### TCB

Cada thread tem um TCB (`OSThread`):

| Campo | Para que serve | Desde |
|-------|----------------|-------|
| `sp` | topo da pilha salvo na troca de contexto (**tem** que ser o 1º campo: a PendSV lê o offset 0) | fase 1 |
| `timeout` | contador regressivo do `OS_delay` | fase 1 |
| `name` | nome (aparece no trace) | fase 2 |
| `job` | função de um job (tarefa periódica) | fase 4 |
| `period` | período em ticks (0 = thread sem período) | fase 4 |
| `nextRelease` | tick absoluto da próxima liberação | fase 4 |
| `deadlineMisses` | quantos deadlines a thread perdeu | fase 4 |
| `prio` | prioridade **efetiva** (número menor = mais prioritária; pode subir na herança) | fase 4/5 |
| `basePrio` | prioridade original (= período) | fase 5 |

### Threads, prontas e escalonador

- As threads ficam em `OS_thread[33]`. O índice 0 é a **idle**, criada no `OS_init()`; as da aplicação vêm na ordem em que são criadas.
- Quem está pronto fica no bitmask `OS_readySet`: o bit `n-1` representa a thread `n` (a idle não tem bit). Os semáforos e o mutex usam o mesmo formato no `waitSet`.
- O `OS_sched()` escolhe, entre as prontas, a de **menor `prio`** (maior prioridade). Se ninguém está pronto, roda a idle. Se a escolhida for outra thread, ele pede a PendSV e retorna `true`.

```cpp
uint8_t best = 0U;                                   // 0 = idle
for (uint8_t n = 1U; n < OS_threadNum; n++) {        // SEMPRE todas as threads
    if (((OS_readySet & (1U << (n - 1U))) != 0U) &&
        ((best == 0U) || (OS_thread[n]->prio < OS_thread[best]->prio))) {
        best = n;
    }
}
```

O laço passa sempre por todas as threads, e o `OS_threadNum` não muda depois do `OS_run()`. Então o tempo do escalonador tem um **pior caso fixo**, que não depende de quem está pronto nem do valor das prioridades: é isso que mantém o kernel determinístico mesmo com a prioridade mudando na herança (fase 5).

### Tick

SysTick a `TICKS_PER_SEC = 10000` (um tick a cada 100 µs). O `OS_tick()` avança o relógio do kernel (`OS_tickCount`), acorda quem terminou o `OS_delay` e libera as tarefas periódicas. O `SysTick_Handler` chama o `OS_tick()` e depois o `OS_sched()`, que é o que faz a **preempção por tempo**.

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

O xPSR inicial tem o bit 24 (bit T, modo Thumb) ligado; sem ele o Cortex-M dá HardFault na primeira execução da thread.

```
SysTick (prioridade mais alta)        PendSV (prioridade mais baixa)
  OS_tick()                             PUSH r4-r11 ; OS_curr->sp = SP
  OS_sched() → pede PendSV  ───────►    SP = OS_next->sp ; OS_curr = OS_next
  sai da ISR                            OS_traceContextSwitch()  (trace)
                                        POP r4-r11 ; BX LR → nova thread
```

A PendSV tem a menor prioridade (`0xFF` no registrador SHPR3), então a troca nunca acontece no meio de outra ISR. Ela é pedida escrevendo o bit 28 (PENDSVSET) do registrador ICSR.

---

## Fase 1 — `yield()` e semáforo

- **`PendSV_Handler`** é `extern "C"` e `__attribute__((naked))`, escrita em assembly inline, para o compilador não colocar prólogo/epílogo de função C++ no meio da troca de contexto.
- **`yield()`** chama o escalonador sem bloquear: a thread cede a CPU voluntariamente. Com o RM (fase 4), ele só troca de thread se houver alguém pronto mais prioritário.
- **`OSSemaphore`** tem um contador e um `waitSet` (bitmask de quem está esperando):
  - `wait()`: se tem crédito, consome e segue. Senão, a thread **sai do `readySet`** e entra no `waitSet`, e o escalonador passa a CPU para outra. É um bloqueio **passivo**, sem busy-waiting;
  - `signal()`: se tem alguém esperando, acorda a thread **mais prioritária** do `waitSet` e passa o crédito direto para ela (nenhum sinal se perde). Senão, incrementa o contador. Depois de acordar alguém, chama o `OS_sched()`: se a acordada for mais prioritária que a atual, ela **preempta na hora**, sem esperar o próximo tick.

As seções críticas do kernel salvam e restauram o `PRIMASK` (`__get_PRIMASK` / `__disable_irq` / `__set_PRIMASK`).

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
- **Prioridade enviada no `SendTaskInfo`:** o período da thread (fase 4), com `Option ReversePriority` no `.txt` (número menor = mais prioritária).

### Instrumentação

| Onde | Evento |
|------|--------|
| `OSThread_start` | `OnTaskCreate` + `SendTaskInfo` |
| `OS_makeReady` (fim do delay, liberação periódica, `signal`, `unlock`) | `OnTaskStartReady` |
| bloqueios | `OnTaskStopReady` com o motivo: 1 = Delay, 2 = Semáforo, 3 = Período, 4 = Mutex |
| PendSV → `OS_traceContextSwitch` | `OnTaskStartExec` ou `OnIdle` |
| `signal()` / `unlock()` que acordam alguém | `OnTaskStopExec` antes do `OS_sched()` e, se não houver troca, `OnTaskStartExec` da mesma thread, para a preempção aparecer na timeline |
| `SysTick_Handler` | `RecordEnterISR` na entrada; na saída, `RecordExitISRToScheduler` se o `OS_sched()` pediu troca, senão `RecordExitISR` |
| início de `OS_delay`, `yield`, `wait`, `signal`, `lock`, `unlock` | eventos de API (IDs 32–35, 37, 38) |
| fim de job atrasado (fase 4) | `DeadlineMiss` (ID 36) |
| herança e restauração (fase 5) | `PrioInherit` (ID 39) e `PrioRestore` (ID 40) |

Os eventos de API existem porque um `wait()` com crédito ou um `signal()` sem ninguém esperando não mudam o estado de nenhuma thread e não apareceriam no trace.

A idle é criada antes do `Start`, então o nome dela não vai para o trace. Não faz falta: o SystemView reconhece a idle pelo evento `OnIdle`.

O arquivo `sysview/SYSVIEW_TheWorstAcai.txt` faz os IDs 32–40 aparecerem com nome (`OS_delay Ticks=10`, `DeadlineMiss Task=alta`, `PrioInherit Task=baixa NovaPrio=20`…), traduz o motivo do bloqueio e liga o `ReversePriority`. Ele tem que ser **copiado** para `C:\Program Files\SEGGER\SystemView\Description\`.

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

### Sincronização

| Objeto | Tipo | Inicial | Função |
|--------|------|---------|--------|
| `MutexFila` | `OSMutex` (fase 5; na fase 3 era um `OSSemaphore(1)`) | livre | exclusão mútua em `fila`, `head` e `tail` |
| `BloqProdutor` | `OSSemaphore` | `CAPACIDADE` | vagas livres: o produtor bloqueia quando zera (fila cheia) |
| `BloqConsumidor` | `OSSemaphore` | 0 | itens na fila: o consumidor bloqueia quando zera (fila vazia) |

```cpp
// produtor (while(1))                // consumidor (while(1))
BloqProdutor.wait();                  BloqConsumidor.wait();
MutexFila.lock();                     MutexFila.lock();
fila[head] = item;                    item = fila[tail];
head = (head + 1) % CAPACIDADE;       tail = (tail + 1) % CAPACIDADE;
MutexFila.unlock();                   MutexFila.unlock();
BloqConsumidor.signal();              BloqProdutor.signal();
OS_delay(PERIODO_PRODUTOR);           OS_delay(PERIODO_CONSUMIDOR);
```

- O semáforo de crédito vem **antes** do mutex. Ao contrário, o produtor poderia pegar o mutex e bloquear com a fila cheia segurando ele, e o consumidor nunca entraria para liberar espaço (deadlock).
- Os semáforos de crédito **não** são mutex: uma thread faz `wait()` e **outra** faz `signal()`, então não existe "dono". Por isso só a fila usa o `OSMutex`.
- O produtor e o consumidor são threads comuns (`OSThread_start`, sem período), com `while(1)` + `OS_delay`. Assim eles não têm deadline, e o trace mostra só a sincronização.

### Conferindo a ordem

O produtor numera os itens (`id` = 0, 1, 2…) e o consumidor confere se eles chegam em sequência. Se algum item fosse perdido, duplicado ou trocado de lugar, a variável global `erros` seria incrementada. Na demonstração ela fica em **0** (dá para acompanhar na aba Expressions do debugger).

### Cenários

| `OS_delay` | O que acontece |
|------------|----------------|
| produtor mais rápido | a fila enche e o **produtor bloqueia** (motivo Semáforo) até o consumidor liberar uma vaga |
| consumidor mais rápido | a fila esvazia e o **consumidor bloqueia** até chegar item novo |
| iguais | a fila fica entre 0 e 1 item e o consumidor bloqueia esperando cada item |

Num trace com o produtor em 2 ticks e o consumidor em 3, o produtor bloqueou 161 vezes pelo semáforo, **todas com a fila em exatamente 32 itens**. A ocupação nunca passou de 32 e as contas bateram (257 produzidos − 225 consumidos = 32 na fila). Na timeline, cada bloqueio aparece como `OSSemaphore_wait` → *Task Block (Semaforo)*, e a thread só volta a rodar depois do `OSSemaphore_signal` da outra.

---

## Fase 4 — Tarefas periódicas e Rate Monotonic

Escolhemos o **Rate Monotonic (RM)**: prioridade fixa, e quanto menor o período, maior a prioridade. É o mais simples dos três que o enunciado aceita (só depende do período) e combina com a fase 5, já que a herança de prioridade é definida em cima de prioridades fixas.

### Tarefa periódica (run-to-completion)

A aplicação entrega uma função que faz **um job** e retorna. Quem repete é o kernel:

```cpp
void OS_periodicLoop() {          // corpo de toda thread periódica
    while (1) {
        OS_curr->job();           // um job, até o fim
        OS_waitNextPeriod();      // dorme até o próximo período
    }
}

rtos::OSThread_startPeriodic(&alta, &job_alta, "alta", PERIODO_ALTA,
                             stack_alta, sizeof(stack_alta));
```

O `OSThread_startPeriodic` guarda `job`, `period` e `nextRelease` no TCB e cria a thread com o `OSThread_start` de sempre, começando no `OS_periodicLoop`. Não tem alocação dinâmica: TCB e pilha continuam sendo globais da aplicação. O job não pode simplesmente "retornar" para terminar a thread, porque o LR inicial da pilha é falso e daria HardFault; por isso o laço fica no kernel.

### Modelo de tempo (independente do hardware)

- **Relógio por software:** `OS_tickCount` é incrementado no `OS_tick()`. Liberação e deadline usam só esse contador. O SysTick só serve de gatilho, e as únicas partes ligadas ao STM32 são o `OS_onStartup()` (configura o SysTick) e o `SysTick_Handler` (chama o `OS_tick()`). Para portar o kernel, basta trocar quem chama o `OS_tick()`.
- **Tempo absoluto:** ao terminar um job, o `OS_waitNextPeriod` faz `nextRelease += period`, em vez de "esperar `period` ticks a partir de agora". Assim o período não escorrega quando o job demora.
- **Liberação:** no `OS_tick`, quando `OS_tickCount == nextRelease` a thread volta para o `readySet`. A comparação é por igualdade de propósito: o `nextRelease` só muda quando o job termina, então uma thread ainda rodando (ou bloqueada num semáforo) nunca é liberada por engano.
- **Job atrasado:** se o job terminar depois da próxima liberação, o `OS_waitNextPeriod` não bloqueia e o próximo job roda em seguida.

### Prioridade RM

A prioridade é o **próprio período**: no `OSThread_start`, `basePrio = prio = period` (sem período, `0xFFFFFFFF`, a menor). Como o RM diz que menor período = maior prioridade, não é preciso ordenar nada: o `OS_sched()` (laço da seção "Arquitetura") já escolhe a pronta de menor `prio`.

Na primeira versão da fase 4 as threads eram ordenadas por período no `OS_onStartup()` e o escalonador pegava o bit mais baixo do `readySet` (O(1)). Trocamos pelo laço na fase 5, porque na herança a prioridade **muda durante a execução**, e "prioridade = posição no vetor" não consegue representar isso.

Efeitos que vêm junto:
- o `signal()` acorda a thread **mais prioritária** do `waitSet` (mesmo laço) e chama o `OS_sched()`, então ela **preempta na hora**;
- o `SysTick_Handler` chama o `OS_sched()`, garantindo a preempção quando o **tempo** libera uma tarefa;
- o `yield()` nunca entrega a CPU para uma thread menos prioritária.

### Deadlines

O deadline de cada job é o fim do período (`nextRelease + period`). O kernel confere quando o job termina, no `OS_waitNextPeriod`: se o relógio já passou do deadline, incrementa `deadlineMisses` no TCB e grava o evento `DeadlineMiss` no SystemView. Como cada job termina uma vez só, cada atraso é contado uma vez, inclusive quando a tarefa está atrasada há vários períodos.

A limitação é que um job que nunca termina (travado para sempre) não é contado. Isso seria um deadlock, e aparece na timeline de outro jeito: a tarefa some.

### Análise do trace

Na fase 4 testamos o produtor e o consumidor como tarefas periódicas, com o produtor em **1 tick** (mais prioritário) e o consumidor em **2 ticks**, fila de 32:

- o produtor é mais rápido, então a fila enche em ~6,6 ms. Daí em diante ele bloqueou **239 vezes** pelo semáforo, **todas com a fila em exatamente 32** itens;
- nas 239 vezes, o `signal()` do consumidor acordou o produtor e ele **preemptou o consumidor na hora** (`OSSemaphore_signal` → *Task Run* do produtor, sem SysTick no meio);
- o produtor passou a perder deadline (226 `DeadlineMiss` em ~52 ms), um a cada 2 ticks. A CPU estava quase toda livre: o atraso vinha da **espera pela fila**, porque o consumidor só libera uma vaga a cada 2 ticks e o produtor queria produzir uma por tick.

Isso mostrou que o produtor/consumidor não é um bom exemplo de RM (os atrasos vêm da sincronização, não do escalonamento). Por isso ele voltou para threads sem período (fase 3), e o RM é demonstrado com as três tarefas da fase 5, que gastam CPU de verdade: na timeline, a `alta` (período 20) interrompe a `media` no meio do job dela.

---

## Fase 5 — Mutex com herança de prioridade (PIP)

### O problema

Com três tarefas, **alta**, **média** e **baixa**, em que só a alta e a baixa usam um recurso R:

```
B pega R → A é liberada, tenta pegar R e BLOQUEIA (R é da B)
         → M é liberada e PREEMPTA a B (M é mais prioritária que B)
         → enquanto M roda, B não solta R, e A fica esperando a M
```

A alta fica esperando uma tarefa que nem usa o recurso, por um tempo sem limite (depende de quanto as tarefas médias rodam): é a **inversão de prioridade ilimitada**. O semáforo não consegue evitar isso porque **não sabe quem está com o recurso**.

### `OSMutex`

O mutex é uma classe separada do semáforo porque tem **dono**: a mesma thread faz `lock()` e `unlock()`. É isso que permite saber quem precisa herdar a prioridade. (Os semáforos de sinalização, como o `BloqProdutor`, não têm dono, e a herança não se aplica a eles.)

```cpp
class OSMutex {
    OSThread *owner;   // quem está com o recurso (nullptr = livre)
    uint32_t waitSet;  // quem está esperando (bit n-1 = thread n)
public:
    void lock();
    void unlock();
};
```

**`lock()`:**
- livre → quem chamou vira o dono e segue;
- ocupado → se quem chamou é **mais prioritário** que o dono (`prio` menor), **o dono herda a prioridade dele** (`owner->prio = OS_curr->prio`, evento `PrioInherit`); depois, quem chamou bloqueia (motivo 4 = Mutex) e o `OS_sched()` escolhe o dono, que agora vale como a tarefa alta.

**`unlock()`:**
- o dono volta para a prioridade base (`owner->prio = owner->basePrio`, evento `PrioRestore`);
- se tem alguém esperando, o mutex passa **direto** para o esperador mais prioritário, que vira o novo dono, e o `OS_sched()` deixa ele preemptar na hora. Senão, o mutex fica livre.

Os dois verificam com `Q_REQUIRE` as regras de uso (o dono não trava de novo; só o dono destrava). Se a aplicação quebrar uma delas, o `Q_onAssert` reseta a placa em vez de seguir num estado inconsistente.

**Limitações:** cada thread segura um mutex por vez (sem herança encadeada entre vários mutexes). Para a demonstração, com um recurso, isso basta.

### Determinismo

A herança muda a prioridade, mas não o custo do kernel: o `OS_sched()`, o `signal()` e o `unlock()` usam o mesmo laço de N voltas (N fixo), e elevar ou restaurar a prioridade é uma atribuição. E é o PIP que **torna o bloqueio calculável**: a tarefa alta espera no máximo a seção crítica da baixa (o termo B da análise de tempo de resposta, R = C + B + interferência), em vez de um tempo sem limite.

### Demo

Três tarefas periódicas que gastam CPU de verdade:

| Tarefa | Período | Job | Prioridade RM |
|--------|---------|-----|---------------|
| `alta` | 20 ticks (2 ms) | pega o recurso, gasta 1 tick, solta | alta |
| `media` | 30 ticks (3 ms) | gasta 15 ticks, **sem** usar o recurso | média |
| `baixa` | 100 ticks (10 ms) | pega o recurso, gasta **20 ticks**, solta | baixa |

O "gastar" é o `ocupar_cpu(ticks)`, um laço vazio com `LOOPS_POR_TICK` voltas por tick. O laço só avança enquanto a thread está rodando: se ela for preemptada no meio, continua de onde parou, como um cálculo de verdade (um `OS_delay` não serviria, porque dormir libera a CPU). Calculamos `LOOPS_POR_TICK = 800` pelo assembly do laço (14 instruções por volta: ~900 voltas/tick na placa a 170 MHz, ~700 no Renode) e conferimos no trace: o job da `media` durou **1,515 ms** em média (planejado: 1,5 ms).

Os tempos foram escolhidos para os eventos caírem nesta ordem: a baixa pega o recurso em t≈16, a alta é liberada em t=20 e bloqueia, e a média é liberada em t=30, com a baixa ainda dentro da seção crítica. O padrão se repete a cada 100 ticks.

Para gravar os dois traces, muda só o tipo do recurso no `main.cpp`:
- **sem protocolo:** `rtos::OSSemaphore recursoAltaBaixa(1U);` com `wait()`/`signal()` (semáforo binário simples);
- **com PIP:** `rtos::OSMutex recursoAltaBaixa;` com `lock()`/`unlock()`.

### Resultados

| | Sem protocolo (semáforo binário) | Com PIP (`OSMutex`) |
|---|---|---|
| Bloqueios da alta no recurso | 37 (motivo Semáforo) | 36 (motivo Mutex) |
| **Tempo máximo da alta bloqueada** | **6,05 ms** | **2,41 ms** |
| **Média rodando enquanto a alta esperava** | **até 3,63 ms** | **0 ms** |
| `PrioInherit` / `PrioRestore` | — | 36 / 36 |
| `DeadlineMiss` | alta: 87 | alta: 12, média: 36 |

- **Sem protocolo:** a alta bloqueia em ~2,24 ms e a média roda 3,6 ms nesse meio tempo, sem usar o recurso. Na timeline, a média aparece rodando enquanto a alta está em *Task Block (Semaforo)*: é a inversão ilimitada.
- **Com PIP:** o `PrioInherit Task=baixa NovaPrio=20` aparece no mesmo instante em que a alta bloqueia (2,238 ms), e o `PrioRestore Task=baixa Prio=100` quando a baixa solta o recurso (4,629 ms). Em nenhum dos 36 bloqueios a média rodou: a interferência da tarefa intermediária foi eliminada, e o bloqueio máximo da alta caiu de 6,05 ms para 2,41 ms.

### Análise crítica

Com PIP ainda sobram deadlines perdidos, e isso não é falha do protocolo:
- **alta (12):** a seção crítica da baixa (20 ticks = 2 ms) é do tamanho do período da alta. O PIP **limita** o bloqueio à seção crítica, mas não deixa ele menor que ela: com B = 2 ms, R = C + B > T = 2 ms no pior caso.
- **média (36):** com a herança, a baixa roda na frente da média enquanto segura o recurso. É o custo do PIP: o bloqueio sai da tarefa alta e vai para a de prioridade intermediária. Com 2 ms de seção crítica + 1,5 ms de execução, a média não cabe no período de 3 ms.

Com uma seção crítica menor (por exemplo 15 ticks, e a média com período 25 e 12 ticks de execução) nenhum deadline seria perdido com PIP, e a inversão sem protocolo continuaria aparecendo. Mantivemos os valores acima porque eles deixam a diferença entre os dois traces mais visível.

---

## Como rodar cada demo

O `main.cpp` está com a demo da **fase 5** ativa (que também mostra o RM da fase 4).

- **Fase 5 sem protocolo:** trocar `rtos::OSMutex recursoAltaBaixa;` por `rtos::OSSemaphore recursoAltaBaixa(1U);` e, no `job_alta` e no `job_baixa`, `lock()`/`unlock()` por `wait()`/`signal()`.
- **Fase 3 (produtor/consumidor):** descomentar o bloco do produtor/consumidor e o bloco que cria as duas threads no `main()`, e comentar a criação da `alta`, `media` e `baixa`. Os tempos de `OS_delay` são o `PERIODO_PRODUTOR` e o `PERIODO_CONSUMIDOR` (para o trace ficar legível, algo como 10 e 20 ticks).

Depois de gravar, conferir `DropCount = 0` antes de analisar o trace.

---

## Renode

- **`nucleog474re.repl`:** clocks em 170 MHz, batendo com o `SystemClock_Config()`. RCC, PWR e FLASH são emulados por scripts Python que devolvem os bits de "pronto" que a HAL espera. A velocidade de execução das instruções é a padrão do Renode (100 MIPS), por isso o `LOOPS_POR_TICK` é um meio-termo entre a placa e o simulador.
- **`str-renode.resc`:** carrega `Debug/trabalho-str.elf`, abre o GDB na porta 3333 e deixa `LogFunctionNames False`. Com `True` a simulação fica muito lenta, porque a idle chama `OS_onIdle()` em loop.

---

## Uso de IA generativa

Usamos o **Claude Opus 5.5** (via Claude Code) ao longo do projeto como auxílio de pesquisa, como revisor para encontrar erros e, em momentos pontuais, como ferramenta para acelerar o desenvolvimento de certas funções.
