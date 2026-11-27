# TheWorstAçaí — RTOS para o STM32G474RE

Kernel de tempo real em **C++20** para a NUCLEO-G474RE (Cortex-M4F, 170 MHz), construído a partir do MiROS (Miro Samek). O kernel tem escalonamento preemptivo **Rate Monotonic**, tarefas periódicas *run-to-completion* com base de tempo por software, semáforos com bloqueio passivo e **mutex com Protocolo de Herança de Prioridade (PIP)**. O comportamento temporal é verificado com traces do **SEGGER SystemView**. O mesmo firmware roda na placa e no simulador **Renode**.

**Autores:** Pedro Paranaguá de Faria Gripp (24103280) · Eduardo Schroeder Dias (24101945)

**Ambiente:** STM32CubeIDE (GCC, `-std=c++20`) · Renode 1.17 · SEGGER SystemView.

| Requisito do enunciado | Onde está |
|---|---|
| Arquitetura desenvolvida | [1. Arquitetura](#1-arquitetura-do-kernel) |
| Diagramas das pilhas e transições de contexto | [2. Pilhas e troca de contexto](#2-pilhas-e-troca-de-contexto) |
| Priorização baseada em herança | [4. Mutex com herança de prioridade](#4-mutex-com-herança-de-prioridade-pip) |
| Modelo de temporização independente | [5. Modelo de temporização](#5-modelo-de-temporização) |
| Análise das linhas de tempo do trace | [7. Resultados e análise dos traces](#7-resultados-e-análise-dos-traces) |
| Uso de IA generativa | [Uso de IA generativa](#uso-de-ia-generativa) |

```
Core/Src/miros.cpp         kernel: escalonador, tick, delay, yield, semáforo, mutex, PendSV
Core/Inc/miros.h           API do kernel, TCB e IDs dos eventos de trace
Core/Src/main.cpp          aplicação de demonstração (PIP ativo; produtor/consumidor comentado)
Core/Src/stm32g4xx_it.cpp  tabela de vetores e SysTick_Handler
Core/Config, Core/SEGGER   SystemView/RTT (configuração e código da SEGGER)
sysview/                   arquivo de descrição do kernel para o SystemView
nucleog474re.repl          descrição da placa para o Renode
str-renode.resc            script da simulação
```

---

## 1. Arquitetura do kernel

### Thread Control Block

```cpp
typedef struct {
    void *sp;                 // topo da pilha salvo (1º campo: a PendSV lê o offset 0)
    uint32_t timeout;         // contador do OS_delay
    const char *name;         // nome (aparece no trace)
    OSThreadHandler job;      // função de um job (tarefa periódica)
    uint32_t period;          // período em ticks (0 = thread sem período)
    uint32_t nextRelease;     // tick absoluto da próxima liberação
    uint32_t prio;            // prioridade efetiva (pode ser elevada pela herança)
    uint32_t basePrio;        // prioridade original
    uint32_t deadlineMisses;  // deadlines perdidos
} OSThread;
```

TCB e pilha de cada thread são variáveis globais da aplicação. O kernel não faz alocação dinâmica: guarda só ponteiros no vetor `OS_thread[33]`, em que o índice 0 é a thread idle.

### Conjuntos de threads

O estado é representado por bitmasks de 32 bits, em que o bit `n-1` corresponde à thread `n`:

- `OS_readySet`: threads prontas. A idle não tem bit e roda quando o conjunto está vazio;
- `waitSet` de cada semáforo e de cada mutex: threads bloqueadas naquele objeto.

Bloquear uma thread é desligar o bit dela no `OS_readySet` e ligar no `waitSet`. Desbloquear é a operação inversa. Nenhuma thread bloqueada consome CPU.

### Escalonador

A política é **Rate Monotonic**: a prioridade de uma tarefa periódica é o seu período (número menor = mais prioritária), e threads sem período ficam com a menor prioridade (`0xFFFFFFFF`). O `OS_sched()` escolhe a thread pronta de menor `prio`:

```cpp
uint8_t best = 0U;                                   // 0 = idle
for (uint8_t n = 1U; n < OS_threadNum; n++) {
    if (((OS_readySet & (1U << (n - 1U))) != 0U) &&
        ((best == 0U) || (OS_thread[n]->prio < OS_thread[best]->prio))) {
        best = n;
    }
}
```

O laço percorre sempre as N threads registradas, e N não muda depois do `OS_run()`. O custo do escalonador tem portanto um **pior caso fixo**, independente de quais threads estão prontas e dos valores de prioridade. Como a decisão usa a prioridade efetiva (`prio`), o mesmo laço atende à herança de prioridade sem nenhum custo adicional.

O escalonador é chamado:
- no `SysTick_Handler`, depois do `OS_tick()`: **preempção por tempo**;
- nas primitivas que acordam threads (`signal()`, `unlock()`): **preempção imediata** quando a thread acordada é mais prioritária;
- nas primitivas que bloqueiam (`OS_delay`, `wait()`, `lock()`, `OS_waitNextPeriod`) e no `yield()`.

Quando a thread escolhida é diferente da atual, o `OS_sched()` pede a PendSV pelo bit 28 (PENDSVSET) do registrador ICSR, e a troca acontece assim que as interrupções forem reabilitadas.

### Seções críticas

As estruturas do kernel são protegidas desabilitando interrupções. O `yield()`, o semáforo e o mutex salvam e restauram o PRIMASK (`__get_PRIMASK` / `__disable_irq` / `__set_PRIMASK`), então funcionam corretamente mesmo quando chamados com as interrupções já desabilitadas.

---

## 2. Pilhas e troca de contexto

Cada thread tem pilha própria (2 KB na aplicação de demonstração). O `OSThread_start` monta a pilha inicial como se a thread já tivesse sido interrompida uma vez, de modo que a primeira entrada pela PendSV é igual a todas as outras:

```
alto  ┌──────────────┐
      │ xPSR (bit T) │ ┐
      │ PC = handler │ │ frame empilhado/desempilhado
      │ LR, R12      │ │ pelo hardware na exceção
      │ R3..R0       │ ┘
      ├──────────────┤
      │ R11..R4      │ ← salvos pela PendSV
      ├──────────────┤ ← me->sp
      │ 0xDEADBEEF…  │   área livre (permite medir o uso da pilha)
baixo └──────────────┘
```

O xPSR inicial tem o bit 24 (Thumb) ligado; sem ele o Cortex-M gera HardFault na primeira execução da thread.

A troca de contexto é feita pela `PendSV_Handler` do MiROS. Para o C++20 ela foi declarada como `extern "C"` (nome sem *mangling*, para a tabela de vetores) e `__attribute__((naked))` (sem prólogo nem epílogo gerados pelo compilador), e recebeu uma chamada ao trace. A transição entre duas threads é:

```
SysTick (prioridade mais alta)          PendSV (prioridade mais baixa)
  OS_tick()                               salva R4–R11 na pilha da thread atual
  OS_sched() → pede PendSV  ───────────►  OS_curr->sp = SP
  sai da ISR                              SP = OS_next->sp ; OS_curr = OS_next
                                          registra a troca no SystemView
                                          restaura R4–R11 ; retorna da exceção
                                          → o hardware restaura o resto do frame
```

A PendSV tem a menor prioridade de exceção (0xFF, configurada no SHPR3 no `OS_init`), então a troca nunca acontece no meio de outra ISR.

---

## 3. Semáforo

`OSSemaphore` é um semáforo contador com fila de bloqueio:

- `wait()`: se `counter > 0`, consome um crédito e segue. Senão, a thread sai do `OS_readySet`, entra no `waitSet` do semáforo e o escalonador entrega a CPU para outra thread. É um bloqueio passivo, sem *busy-waiting*.
- `signal()`: se há threads esperando, acorda a **mais prioritária** do `waitSet` (mesmo laço do escalonador) e entrega o crédito diretamente a ela. Senão, incrementa o contador. Depois de acordar alguém, chama o `OS_sched()`, o que dá preempção imediata se a thread acordada for mais prioritária.

O semáforo não tem dono: uma thread pode fazer `wait()` e outra `signal()`. Por isso ele serve para sinalização e contagem de recursos (como as vagas de um buffer), mas não para herança de prioridade.

---

## 4. Mutex com herança de prioridade (PIP)

### Inversão de prioridade

Com três tarefas, **alta**, **média** e **baixa**, em que só a alta e a baixa usam um recurso R protegido por um semáforo binário:

```
baixa pega R → alta é liberada, tenta pegar R e bloqueia
             → média é liberada e preempta a baixa
             → enquanto a média roda, a baixa não libera R e a alta espera
```

A alta passa a esperar uma tarefa que nem usa o recurso, por um tempo que depende de quanto as tarefas intermediárias executam: é a **inversão de prioridade ilimitada**. O semáforo não consegue evitar isso porque não sabe quem está com o recurso.

### `OSMutex`

O mutex registra o **dono** (`owner`), a thread que fez o `lock()` e que é a única autorizada a fazer o `unlock()`.

```cpp
class OSMutex {
    OSThread *owner;   // quem está com o recurso (nullptr = livre)
    uint32_t waitSet;  // quem está esperando (bit n-1 = thread n)
public:
    void lock();
    void unlock();
};
```

**`lock()`**
- livre: quem chamou vira o dono e segue;
- ocupado: se quem chamou tem prioridade maior que a do dono, o dono **herda** a prioridade (`owner->prio = OS_curr->prio`, evento `PrioInherit`). Depois quem chamou bloqueia (motivo *Mutex*) e o escalonador escolhe o dono, que agora não pode ser preemptado por tarefas de prioridade intermediária.

**`unlock()`**
- o dono volta para a prioridade original (`owner->prio = owner->basePrio`, evento `PrioRestore`);
- se há threads esperando, o mutex passa **diretamente** para a mais prioritária, que vira o novo dono e fica pronta, e o `OS_sched()` permite a preempção imediata. Senão, o mutex fica livre.

As regras de uso são verificadas com `Q_REQUIRE` (o dono não trava o mesmo mutex de novo; só o dono destrava). Uma violação chama `Q_onAssert`, que reinicia o microcontrolador.

### Modelagem da prioridade

Cada thread tem duas prioridades:

| Campo | Definido em | Significado |
|---|---|---|
| `basePrio` | `OSThread_start` (= período, pela regra RM) | prioridade original, nunca muda |
| `prio` | `= basePrio` na criação; alterado só pelo mutex | prioridade efetiva, usada pelo escalonador |

A herança é uma atribuição em `prio` e a restauração é a cópia de volta de `basePrio`. O escalonador e a escolha do esperador no `unlock()` usam o mesmo laço de N voltas, então a herança não altera o custo de pior caso do kernel.

Com o PIP, o tempo que uma tarefa fica bloqueada por uma de menor prioridade é **limitado pela seção crítica** desta, o que torna o termo de bloqueio B calculável na análise de tempo de resposta (R = C + B + interferência das mais prioritárias).

**Limitação:** cada thread segura no máximo um mutex por vez (não há herança encadeada entre vários mutexes).

---

## 5. Modelo de temporização

### Relógio do kernel

O tempo do kernel é o contador `OS_tickCount`, incrementado no `OS_tick()`. Com `TICKS_PER_SEC = 10000`, um tick vale 100 µs. Liberações, deadlines e `OS_delay` são calculados apenas com esse contador:

```cpp
void OS_tick(void) {
    OS_tickCount++;
    for (n = 1U; n < OS_threadNum; n++) {
        // OS_delay: contador regressivo em timeout
        if ((OS_thread[n]->period != 0U) &&
            (OS_tickCount == OS_thread[n]->nextRelease)) {
            OS_makeReady(n);                       // liberação periódica
        }
    }
}
```

Os únicos pontos ligados ao hardware são o `OS_onStartup()`, que configura o SysTick, e o `SysTick_Handler`, que chama `OS_tick()` e `OS_sched()`. Para portar o kernel para outra plataforma, basta que outra interrupção periódica chame essas duas funções.

### Tarefas periódicas

A aplicação fornece uma função que executa **um job** e retorna; o kernel é quem repete:

```cpp
void OS_periodicLoop() {          // corpo de toda thread periódica
    while (1) {
        OS_curr->job();
        OS_waitNextPeriod();
    }
}
```

No fim de cada job, o `OS_waitNextPeriod()`:
1. confere o deadline (D = T): se `OS_tickCount` já passou de `nextRelease + period`, incrementa `deadlineMisses` e registra o evento `DeadlineMiss`;
2. avança a liberação em **tempo absoluto** (`nextRelease += period`), para o período não escorregar quando um job demora;
3. se a próxima liberação já passou (job atrasado), retorna e o próximo job roda em seguida; senão, a thread sai das prontas até o `OS_tick()` liberá-la.

A liberação no `OS_tick()` compara por igualdade de propósito: `nextRelease` só avança quando o job termina, então um job ainda em execução (ou bloqueado num semáforo) nunca é liberado de novo. Como cada job termina uma única vez, cada atraso é contado uma vez, mesmo que a tarefa esteja atrasada vários períodos. Um job que nunca termina não é contado; esse caso é um travamento e aparece na timeline como a tarefa deixando de executar.

---

## 6. Instrumentação e coleta do trace

### Eventos

| Onde | Evento SystemView |
|------|-------------------|
| `OSThread_start` | `OnTaskCreate` + `SendTaskInfo` (prioridade = período) |
| `OS_makeReady` | `OnTaskStartReady` |
| bloqueios | `OnTaskStopReady`, com o motivo: 1 = Delay, 2 = Semáforo, 3 = Período, 4 = Mutex |
| PendSV | `OnTaskStartExec`, ou `OnIdle` |
| `signal()` / `unlock()` que acordam alguém | `OnTaskStopExec` antes do `OS_sched()` (e `OnTaskStartExec` se não houver troca) |
| `SysTick_Handler` | `RecordEnterISR`; na saída `RecordExitISRToScheduler` se houver troca, senão `RecordExitISR` |
| `OS_delay`, `yield`, `wait`, `signal`, `lock`, `unlock` | eventos de API (IDs 32–35, 37, 38) |
| kernel | `DeadlineMiss` (36), `PrioInherit` (39), `PrioRestore` (40) |

O arquivo `sysview/SYSVIEW_TheWorstAcai.txt` dá nome e formato aos eventos 32–40, traduz o motivo do bloqueio e liga o `Option ReversePriority` (número menor = mais prioritária). Ele deve ser copiado para `C:\Program Files\SEGGER\SystemView\Description\`. O timestamp é o `DWT->CYCCNT >> 4` (resolução de 16 ciclos ≈ 94 ns a 170 MHz).

### Coleta *single-shot*

Os eventos são gravados num buffer RTT de 32 KB na RAM. Ao fim do experimento, a CPU é parada e o buffer é salvo pelo debugger:

```gdb
target remote :3333                     # Renode (str-renode.resc) ou ST-LINK
p _SEGGER_RTT.aUp[1]                    # confere sName = "SysView" e o WrOff
dump binary memory trace.SVDat _SEGGER_RTT.aUp[1].pBuffer _SEGGER_RTT.aUp[1].pBuffer+_SEGGER_RTT.aUp[1].WrOff
p _SYSVIEW_Globals.MainContext.DropCount   # tem que ser 0
```

Pelo STM32CubeIDE: aba **Memory** → Export → *RAW Binary*, a partir de `pBuffer`, com tamanho `WrOff`. O arquivo é aberto no SystemView em *File → Load Recording*.

Escolhemos esse método em vez de transmitir os eventos pela UART por três motivos:

- **Taxa de eventos.** Na aplicação de demonstração o kernel gera cerca de **105 KB/s** de dados de trace (32 KB em ~310 ms). Uma UART a 115200 bit/s transmite cerca de 11,5 KB/s; mesmo a 921600 bit/s não acompanharia, e os eventos seriam descartados.
- **Interferência mínima.** Durante a execução, gravar um evento é só copiar alguns bytes para a RAM. Não há periférico, interrupção de transmissão nem DMA disputando a CPU com as tarefas, então o trace não altera a temporização que está sendo medida.
- **Janela suficiente.** 32 KB cobrem cerca de 300 ms, o equivalente a ~50 hiperperíodos da demonstração da seção 7. Isso basta para observar o comportamento em regime e repetido.

O método funciona igual na placa (ST-LINK) e no Renode. A perda de eventos é conferida pelo `DropCount`, já que na gravação *single-shot* o SystemView não registra *overflow*.

---

## 7. Resultados e análise dos traces

### Execução básica

Com três threads fazendo `OS_delay` de 10, 20 e 5 ticks, a timeline mostra o SysTick (ISR 15) a cada 100 µs (~4,9 µs por execução) e a idle ocupando o tempo livre. Num tick que acorda uma thread, a sequência registrada é sempre:

```
ISR Enter (SysTick) → Task Ready → ISR Exit to Scheduler
  → Task Run → OS_delay → Task Block (Delay) → Idle
```

Nos ticks sem liberação aparecem apenas `ISR Enter` e `ISR Exit`.

### Produtor/consumidor

Buffer circular estático de 32 posições, com três semáforos: `MutexFila` (binário, inicia em 1) garante a exclusão mútua em `fila`, `head` e `tail`; `BloqProdutor` (vagas, inicia em 32) e `BloqConsumidor` (itens, inicia em 0) controlam o fluxo. O semáforo de crédito é adquirido antes do `MutexFila`; na ordem inversa, o produtor bloquearia com a fila cheia segurando a exclusão mútua, e o consumidor nunca liberaria espaço (*deadlock*).

Com o produtor a cada 2 ticks e o consumidor a cada 3:
- o produtor bloqueou **161 vezes** no semáforo, todas com a fila em exatamente **32 itens**;
- produzidos − consumidos = 257 − 225 = 32, igual à capacidade;
- a verificação de sequência no consumidor (`erros`) permaneceu em **0**: nenhum item perdido, duplicado ou fora de ordem.

Na timeline, cada bloqueio aparece como `OSSemaphore_wait` → *Task Block (Semaforo)*, e a thread só volta a executar após o `OSSemaphore_signal` da outra.

### Rate Monotonic e PIP

**Conjunto de tarefas** (execução simulada pelo laço calibrado `ocupar_cpu`):

| Tarefa | Período T | Execução C | Recurso R |
|--------|-----------|------------|-----------|
| alta | 15 ticks (1,5 ms) | 1 tick | sim, o job inteiro |
| média | 20 ticks (2 ms) | 9 ticks | não |
| baixa | 60 ticks (6 ms) | 13 ticks | sim, o job inteiro |

U = 1/15 + 9/20 + 13/60 ≈ 0,73, abaixo do limite de Liu & Layland para 3 tarefas (0,78). O período da baixa é o MMC dos outros dois, então todos os jobs da baixa encontram as mesmas liberações e o cenário de inversão se repete a cada 6 ms: a baixa entra na seção crítica em t = 10, a alta é liberada em t = 15 e bloqueia, e a média é liberada em t = 20 com a baixa ainda segurando R.

**Escalonamento RM.** As 206 liberações da alta no trace (evento *Task Ready* dentro do SysTick) ocorreram a intervalos de 1500,0 µs ± 0,1 µs: a base de tempo por software não acumula deriva, porque `nextRelease` é absoluto. O início da execução pode atrasar alguns µs no tick em que as três tarefas são liberadas juntas, já que o `OS_tick()` demora mais. O job da média (9 ticks) mediu 0,900 ms, o que confirma a calibração do `ocupar_cpu`.

**Resultados** (traces de ~310–320 ms, ≈ 52 ciclos cada):

| | Sem protocolo (semáforo binário) | Com PIP (`OSMutex`) |
|---|---|---|
| Bloqueios da alta no recurso | 54 (motivo Semáforo) | 52 (motivo Mutex) |
| Tempo máximo da alta bloqueada | 1,756 ms | 0,853 ms |
| Média executando enquanto a alta esperava | 0,904 ms (o job inteiro) | 0 ms |
| `PrioInherit` / `PrioRestore` | — | 52 / 52 |
| `DeadlineMiss` | alta: 54 (um por ciclo) | 0 |

Sem protocolo, em todos os 54 ciclos a média é liberada com a alta bloqueada e executa o job inteiro (0,904 ms) antes de a baixa conseguir liberar R. A alta fica bloqueada 1,756 ms e perde o deadline em todo ciclo: é a inversão de prioridade, com a tarefa intermediária atrasando a de maior prioridade sem usar o recurso.

Com PIP, o `PrioInherit Task=baixa NovaPrio=15` aparece no mesmo instante em que a alta bloqueia, e o `PrioRestore Task=baixa Prio=60` quando a baixa libera R. Logo após o `unlock()`, a alta passa a executar sem esperar o próximo tick (entrega direta do mutex + preempção imediata). Em nenhum dos 52 bloqueios a média executou: a interferência da tarefa intermediária foi eliminada.

**Análise.** Para a alta, o bloqueio máximo é a seção crítica da baixa: R = C + B = 1 + 13 = 14 ticks ≤ T = 15, então o deadline é garantido no pior caso. No trace o bloqueio foi menor (~8,5 ticks) porque a baixa já tinha executado parte da seção crítica quando a alta foi liberada. Para a média, o teste clássico com B = 13 é pessimista: R = 9 + 13 + ⌈R/15⌉·1 = 24 > 20, e não dá garantia. Esse limite supõe que a média é liberada no início da seção crítica da baixa, o que não acontece neste conjunto: a baixa só começa a executar depois que a média termina o job. No padrão periódico real a média termina cerca de 15 ticks após ser liberada, como mostra o trace (nenhum deadline perdido). O PIP torna o bloqueio **limitado e calculável**, mas não menor que a seção crítica: com uma seção crítica maior que T − C da alta, nenhum protocolo de herança evitaria a perda de deadline.

---

## 8. Como usar o kernel

```cpp
#include "miros.h"
#include "SEGGER_SYSVIEW.h"

uint32_t stack_idleThread[512];

uint32_t stack_controle[512];
rtos::OSThread controle;
void job_controle() {            // um job: executa e retorna
    /* leitura, cálculo, atuação */
}

rtos::OSMutex recurso;           // recurso compartilhado com herança de prioridade
rtos::OSSemaphore itens(0U);     // semáforo de contagem

int main(void) {
    rtos::OS_init(stack_idleThread, sizeof(stack_idleThread));
    SEGGER_SYSVIEW_Conf();
    SEGGER_SYSVIEW_Start();

    rtos::OSThread_startPeriodic(&controle, &job_controle, "controle",
                                 10U /* período em ticks */,
                                 stack_controle, sizeof(stack_controle));

    rtos::OS_run();              // não retorna
}
```

| Função | Uso |
|---|---|
| `OS_init(stk, size)` | configura o clock e a PendSV e cria a idle; chamar primeiro |
| `OSThread_startPeriodic(...)` | tarefa periódica; prioridade RM = período |
| `OSThread_start(...)` | thread comum com laço próprio (`while(1)`); menor prioridade |
| `OS_run()` | inicia o SysTick e o escalonamento |
| `OS_delay(ticks)` | bloqueia a thread pelo número de ticks |
| `yield()` | cede a CPU sem bloquear |
| `OSSemaphore::wait()` / `signal()` | sinalização e contagem |
| `OSMutex::lock()` / `unlock()` | exclusão mútua com herança de prioridade |

Regras de uso:
- no máximo 32 threads além da idle;
- as primitivas do kernel são chamadas apenas a partir de threads, nunca de ISRs;
- `lock()` e `unlock()` de um mutex são feitos pela mesma thread, e cada thread segura um mutex por vez;
- um job periódico deve retornar; ele não pode terminar a thread.

### Reproduzindo os experimentos

- **PIP:** `main.cpp` como está (`rtos::OSMutex recursoAltaBaixa;`).
- **Sem protocolo:** trocar por `rtos::OSSemaphore recursoAltaBaixa(1U);` e, em `job_alta` e `job_baixa`, `lock()`/`unlock()` por `wait()`/`signal()`.
- **Produtor/consumidor:** descomentar o bloco do produtor/consumidor e a criação das duas threads no `main()`, e comentar a criação de `alta`, `media` e `baixa`.
- **Calibração:** `LOOPS_POR_TICK` é o número de voltas do `ocupar_cpu` em um tick. Vale **660** no Renode e cerca de **900** na placa a 170 MHz. Para recalibrar, medir no trace a duração de um job sem preempção e aplicar: novo valor = valor atual × tempo esperado / tempo medido.

### Renode

- `nucleog474re.repl`: clocks em 170 MHz, como no `SystemClock_Config()`. RCC, PWR e FLASH são emulados por scripts Python que devolvem os bits de "pronto" esperados pela HAL.
- `str-renode.resc`: carrega `Debug/trabalho-str.elf` (o CubeIDE nomeia o `.elf` pelo nome do projeto no `.project`), abre o servidor GDB na porta 3333 e mantém `LogFunctionNames False` (com `True` a simulação fica muito lenta). Se o projeto for importado com outro nome, basta definir `$binpath=@<caminho do .elf>` antes de incluir o script.

---

## Uso de IA generativa

Usamos o **Claude Opus 5.5** (via Claude Code) ao longo do projeto como auxílio de pesquisa, como revisor para encontrar erros e, em momentos pontuais, como ferramenta para acelerar o desenvolvimento de certas funções.
