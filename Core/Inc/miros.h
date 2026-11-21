/*
 * miros.h
 *
 *  Created on: Feb 6, 2025
 *      Author: guilh
 */
#ifndef INC_MIROS_H_
#define INC_MIROS_H_

namespace rtos {

typedef void (*OSThreadHandler)();   /* tem que vir antes do TCB (campo job) */

/* Thread Control Block (TCB) */
typedef struct {
    void *sp;                 /* stack pointer -- TEM que ser o 1º campo (a PendSV usa o offset 0) */
    uint32_t timeout;         /* contador do OS_delay */
    const char *name;
    OSThreadHandler job;      /* função de UM job (run-to-completion); 0 = não periódica */
    uint32_t period;          /* período em ticks (0 = não periódica) */
    uint32_t nextRelease;     /* tick absoluto da próxima liberação */
    uint8_t  prio;            /* prioridade RM: menor período = maior prioridade */
    uint32_t deadlineMisses;  /* quantos deadlines essa thread ja perdeu (deadline = fim do periodo) */
} OSThread;

const uint16_t TICKS_PER_SEC = 10000U;

/* relogio do kernel por software (ticks desde o OS_run); incrementado no OS_tick() */
extern volatile uint32_t OS_tickCount;

/* IDs dos eventos de chamada de API no SystemView*/
enum OSTraceApiId {
    TRACE_ID_OS_DELAY   = 32U,
    TRACE_ID_YIELD      = 33U,
    TRACE_ID_SEM_WAIT   = 34U,
    TRACE_ID_SEM_SIGNAL = 35U,
    TRACE_ID_DEADLINE_MISS = 36U   /* nao e API: aviso do kernel de deadline perdido */
};

void OS_init(void *stkSto, uint32_t stkSize);

/* callback to handle the idle condition */
void OS_onIdle(void);

/* this function must be called with interrupts DISABLED */
/* returns true when a context switch (PendSV) was requested */
bool OS_sched(void);

/* transfer control to the RTOS to run the threads */
void OS_run(void);

/* blocking delay */
void OS_delay(uint32_t ticks);

/* yields CPU to next thread */
void yield(void);

/* process all timeouts */
void OS_tick(void);

/* callback to configure and start interrupts */
void OS_onStartup(void);

void OSThread_start(
    OSThread *me,
    OSThreadHandler threadHandler,
	const char *name,
    void *stkSto, uint32_t stkSize);

void OS_waitNextPeriod();

void OSThread_startPeriodic(
    OSThread *me,
    OSThreadHandler job,
    const char *name,
    uint32_t period,
    void *stkSto, uint32_t stkSize);

class OSSemaphore {
private:
    uint32_t counter;
    uint32_t waitSet; // Fila de bloqueio usando mapa de bits

public:
    // Construtor: inicializa o contador e garante que a fila comece vazia
    explicit OSSemaphore(uint32_t initial_count = 0);

    void wait();   // Função para tentar adquirir o semáforo (P / Acquire)
    void signal(); // Função para liberar/sinalizar o semáforo (V / Release)
};


}

#endif /* INC_MIROS_H_ */
