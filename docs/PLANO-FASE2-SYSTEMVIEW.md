# Plano de implementação — Fase 2 (Trace com SEGGER SystemView) — versão simplificada

> **Referência:** *SEGGER SystemView User Guide*, UM08027 v4.10. As páginas (p. N) são as impressas no rodapé.
> As linhas citadas são do código atual (18/11/2026). **Nada foi implementado ainda.**
> Decisões da equipe: sem `pfGetTime` (tempo relativo basta), sem chave liga/desliga, sem marcadores de LED. O dump single-shot via GDB fica como já está.

---

## O que já está pronto e conforme o manual

| Item | Onde | Manual |
|------|------|--------|
| Timestamp `CYCCNT >> 4` (28 bits) e ID de interrupção via `ICSR` | `SEGGER_SYSVIEW_Conf.h:42-43, 60` | 4.5.1 (p. 75–77), 4.6.1 (p. 100) |
| Criação, pronta, bloqueada (motivo 1 = delay, 2 = semáforo), executando, idle | `miros.cpp:53, 115, 181-189, 291, 328-335` | 6.1.1 (p. 138–140) |
| `SEGGER_SYSVIEW_Conf()` + `Start()` | `main.cpp:59-60` | 4.2 (p. 69), 4.4 (p. 73) |

---

## Etapa 1 — Criar a idle depois do `Start` (mudar só a ordem)

**Problema:** a idle é criada dentro de `OS_init()` (`miros.cpp:252`). Isso acontece **antes** de `SEGGER_SYSVIEW_Conf()`/`Start()` (`main.cpp:59-60`), então o `SendTaskInfo` dela é descartado e ela aparece sem nome.

**Restrição:** o `Conf()` precisa vir **depois** do `SystemClock_Config()`. Ele passa `SystemCoreClock` ao `SEGGER_SYSVIEW_Init()` como frequência do timestamp (7.2.1, p. 156). Antes disso o valor ainda é o do clock de reset (16 MHz) e a escala de tempo sairia errada.

**Mudança:** em `OS_init()`, chamar `Conf()` e `Start()` entre a configuração do clock e a criação da idle, e tirar as duas chamadas do `main`:

```cpp
void OS_init(void *stkSto, uint32_t stkSize) {
    SystemClock_Config();
    SysTick->CTRL = 0U;
    SystemCoreClockUpdate();

    SEGGER_SYSVIEW_Conf();    // <- veio do main.cpp:59
    SEGGER_SYSVIEW_Start();   // <- veio do main.cpp:60

    *(uint32_t volatile *)0xE000ED20 |= (0xFFU << 16);
    OSThread_start(&idleThread, ...);   // agora o SendTaskInfo da idle é gravado
}
```

**Ajuste de 1 linha que vale fazer junto:** `SEGGER_SYSVIEW_Config_NoOS.c:85`, trocar `ENABLE_DWT_CYCCNT` para `1`.
- Hoje a macro vale 0, então o firmware nunca liga `DEMCR.TRCENA` e o `CYCCNT` só conta se o debugger ligar esse bit.
- **Na placa real os timestamps podem sair todos 0.** No Renode funciona porque o modelo conta sempre.
- Manual 4.3 (p. 70–72): sem J-Link, a aplicação deve habilitar o contador.

---

## Etapa 2 — Registrar a interrupção do SysTick (exigido pela rubrica)

**Arquivo:** `Core/Src/stm32g4xx_it.cpp`, `SysTick_Handler`, linhas 110-117.

| Função | O que faz | Manual |
|--------|-----------|--------|
| `SEGGER_SYSVIEW_RecordEnterISR()` | Registra "entrou na ISR *n*". O número vem do `ICSR` (15 = SysTick) | 6.1.2.1 (p. 141), 7.6.8 (p. 213) |
| `SEGGER_SYSVIEW_RecordExitISR()` | "Saiu da ISR e volta para a mesma tarefa" | 6.1.2.2 (p. 141), 7.6.9 (p. 214) |
| `SEGGER_SYSVIEW_RecordExitISRToScheduler()` | "Saiu da ISR e vai haver troca de tarefa" | 6.1.2.2 (p. 141), 7.6.10 (p. 215) |

**Mudança no `OS_sched`:** ele passa a **retornar** se pediu troca (`bool OS_sched(void)`, retornando `OS_next != OS_curr`, que é a condição que já existe na linha 77). Quem chama `OS_sched` sem usar o retorno (`OS_delay`, `yield`, `wait`) não muda nada.

**SysTick resultante:**
```cpp
void SysTick_Handler(void) {
  SEGGER_SYSVIEW_RecordEnterISR();
  HAL_IncTick();
  rtos::OS_tick();
  __disable_irq();
  if (rtos::OS_sched()) {
      SEGGER_SYSVIEW_RecordExitISRToScheduler();
  } else {
      SEGGER_SYSVIEW_RecordExitISR();
  }
  __enable_irq();
}
```
O registro de saída fica **dentro** da seção crítica, junto da decisão do escalonador, como a equipe sugeriu. Não fica dentro de `OS_sched` porque ele também é chamado fora de interrupção (`OS_delay`, `yield`, `wait`). Aí o evento "saída de ISR" seria gravado sem ter havido ISR.

**Sequência esperada na timeline:**
`ISR Enter (SysTick)` → `Task Ready (blinkX)` → `ISR Exit to Scheduler` → `Task Run (blinkX)` (vindo da PendSV).

---

## Etapa 3 — Chamadas de API do kernel (mínimo pedido no enunciado)

**Por que é necessário, mesmo com os eventos de estado:** os eventos de tarefa só aparecem quando **o estado muda**.
- `OSSemaphore::wait()` com crédito disponível, `signal()` sem ninguém esperando (só `counter++`) e `yield()` sem outra tarefa pronta **não geram nenhum evento hoje**. Essas chamadas ficam invisíveis no trace.
- O enunciado pede explicitamente "operações de bloqueio e desbloqueio em semáforos **e chamadas de API do sistema**".

**Mudança mínima:** uma linha no início de cada função de API.

| ID | Onde (1ª linha da função) | Chamada | Manual |
|----|---------------------------|---------|--------|
| 32 | `OS_delay`, `miros.cpp:107` | `SEGGER_SYSVIEW_RecordU32(32, ticks);` | 7.8.2 (p. 226) |
| 33 | `yield`, `miros.cpp:121` | `SEGGER_SYSVIEW_RecordVoid(33);` | 7.8.1 (p. 225) |
| 34 | `OSSemaphore::wait`, `miros.cpp:276` | `SEGGER_SYSVIEW_RecordU32(34, (U32)this);` | 7.8.2 (p. 226) |
| 35 | `OSSemaphore::signal`, `miros.cpp:298` | `SEGGER_SYSVIEW_RecordU32(35, (U32)this);` | 7.8.2 (p. 226) |

Os IDs precisam ficar entre 32 e 511 (6.1.5.1, p. 144). Sugestão: um `enum` com esses valores em `miros.h`.

**Arquivo de descrição (opcional, só visual):** sem ele, os eventos aparecem como "ID 32", "ID 33". Com um arquivo texto `SYSVIEW_MiROS.txt` na pasta `description/` do SystemView, aparecem com nome. Isso exige mandar `O=MiROS` na descrição do sistema (`Config_NoOS.c:108`).
```
32  OS_delay            Ticks=%u
33  yield
34  OSSemaphore_wait    Sem=%p
35  OSSemaphore_signal  Sem=%p
TaskState 0xFF 1=Delay, 2=Semaforo
```
Manual 6.1.5 (p. 144–146).

---

## Resumo das mudanças

| Etapa | Arquivos | Linhas aprox. |
|-------|----------|---------------|
| 1 | `miros.cpp` (`OS_init`), `main.cpp` (remover 2 linhas), `Config_NoOS.c:85` | ~5 |
| 2 | `stm32g4xx_it.cpp` (`SysTick_Handler`), `miros.h`/`miros.cpp` (`OS_sched` retorna `bool`) | ~10 |
| 3 | `miros.cpp` (4 linhas), `miros.h` (enum de IDs), opcional `SYSVIEW_MiROS.txt` + `Config_NoOS.c:108` | ~10 |

O dump continua sendo feito como a equipe já faz (single-shot via GDB, manual 3.15.2, p. 57–58).
