/*
 * miros.h
 *
 *  Created on: Feb 6, 2025
 *      Author: guilh
 */

#ifndef INC_MIROS_H_
#define INC_MIROS_H_

namespace rtos {
/* Thread Control Block (TCB) */
typedef struct {
    void *sp; /* stack pointer */
    uint32_t timeout; /* timeout delay down-counter */
    /* ... other attributes associated with a thread */
} OSThread;

const uint16_t TICKS_PER_SEC = 100U;

typedef void (*OSThreadHandler)();

void OS_init(void *stkSto, uint32_t stkSize);

/* callback to handle the idle condition */
void OS_onIdle(void);

/* this function must be called with interrupts DISABLED */
void OS_sched(void);

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
