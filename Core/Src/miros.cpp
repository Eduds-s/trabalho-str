/****************************************************************************
* MInimal Real-time Operating System (MiROS), GNU-ARM port.
*
* This software is a teaching aid to illustrate the concepts underlying
* a Real-Time Operating System (RTOS). The main goal of the software is
* simplicity and clear presentation of the concepts, but without dealing
* with various corner cases, portability, or error handling. For these
* reasons, the software is generally NOT intended or recommended for use
* in commercial applications.
*
* Copyright (C) 2018 Miro Samek. All Rights Reserved.
*
* SPDX-License-Identifier: GPL-3.0-or-later
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <https://www.gnu.org/licenses/>.
*
* Git repo:
* https://github.com/QuantumLeaps/MiROS
****************************************************************************/
#include <cstdint>
#include <cstring>
#include "miros.h"
#include "qassert.h"
#include "stm32g4xx.h"
#include "SEGGER_SYSVIEW.h"

Q_DEFINE_THIS_FILE

namespace rtos {

OSThread * volatile OS_curr; /* pointer to the current thread */
OSThread * volatile OS_next; /* pointer to the next thread to run */

OSThread *OS_thread[32 + 1]; /* array of threads started so far */
uint32_t OS_readySet; /* bitmask of threads that are ready to run */

uint8_t OS_threadNum; /* number of threads started */
uint8_t OS_currIdx; /* current thread index for the circular array */

static inline void OS_makeReady(uint8_t threadIdx) {
    OS_readySet |= (1U << (threadIdx - 1U));
    SEGGER_SYSVIEW_OnTaskStartReady((U32)OS_thread[threadIdx]);
}

OSThread idleThread;
void main_idleThread() {
    while (1) {
        OS_onIdle();
    }
}

bool OS_sched(void) {
    if (OS_readySet == 0U) { /* idle condition? */
    	OS_currIdx = 0U; /* the idle thread */
    } else {
    	do{ /* find the next ready thread*/
            OS_currIdx++;
            if(OS_currIdx == OS_threadNum){
            	OS_currIdx = 1;
            }
            OS_next = OS_thread[OS_currIdx];
    	}while((OS_readySet & (1U <<(OS_currIdx - 1U))) == 0 );
    }
    OS_next = OS_thread[OS_currIdx];

    /* trigger PendSV, if needed */
    if(OS_next != OS_curr){
    		*(uint32_t volatile *)0xE000ED04 = (1U << 28);
    		return true;
    }
    return false;
}

void OS_run(void) {
    /* callback to configure and start interrupts */
    OS_onStartup();

    __disable_irq();
    OS_sched();
    __enable_irq();

    /* the following code should never execute */
    Q_ERROR();
}

void OS_tick(void) {
	uint8_t n = 0;
	for(n=1U;n<OS_threadNum; n++){ 				/* cycle through every thread but the idle */
		if(OS_thread[n]->timeout != 0U){
			OS_thread[n]->timeout--;			/* decrease the timeout */
			if(OS_thread[n]->timeout == 0U){
				OS_makeReady(n);	/* if the thread is ready mask the corresponding bit */
			}
		}
	}
}

void OS_delay(uint32_t ticks) {
    __asm volatile ("cpsid i");

    /* never call OS_delay from the idleThread */
    Q_REQUIRE(OS_curr != OS_thread[0]);

    OS_curr->timeout = ticks;
    OS_readySet &= ~(1U << (OS_currIdx - 1U));
    SEGGER_SYSVIEW_OnTaskStopReady((U32)OS_curr, 1U);
    OS_sched();
    __asm volatile ("cpsie i");
 }


void yield(void) {
	__asm volatile ("cpsid i");
    OS_sched();
    __asm volatile ("cpsie i");
 }

void OSThread_start(
    OSThread *me,
    OSThreadHandler threadHandler,
    const char *name,
    void *stkSto, uint32_t stkSize)
{
    SEGGER_SYSVIEW_TASKINFO Info;

    /* round down the stack top to the 8-byte boundary
     * NOTE: ARM Cortex-M stack grows down from hi -> low memory
     */
    uint32_t *sp = (uint32_t *)((((uint32_t)stkSto + stkSize) / 8) * 8);
    uint32_t *stk_limit;

    /* thread number must be in range
     * and must be unused
     */
    Q_REQUIRE((OS_threadNum < Q_DIM(OS_thread)) && (OS_thread[OS_threadNum] == (OSThread *)0));

    *(--sp) = (1U << 24);  /* xPSR */
    *(--sp) = (uint32_t)threadHandler; /* PC */
    *(--sp) = 0x0000000EU; /* LR  */
    *(--sp) = 0x0000000CU; /* R12 */
    *(--sp) = 0x00000003U; /* R3  */
    *(--sp) = 0x00000002U; /* R2  */
    *(--sp) = 0x00000001U; /* R1  */
    *(--sp) = 0x00000000U; /* R0  */
    /* additionally, fake registers R4-R11 */
    *(--sp) = 0x0000000BU; /* R11 */
    *(--sp) = 0x0000000AU; /* R10 */
    *(--sp) = 0x00000009U; /* R9 */
    *(--sp) = 0x00000008U; /* R8 */
    *(--sp) = 0x00000007U; /* R7 */
    *(--sp) = 0x00000006U; /* R6 */
    *(--sp) = 0x00000005U; /* R5 */
    *(--sp) = 0x00000004U; /* R4 */

    /* save the top of the stack in the thread's attribute */
    me->sp = sp;
    me->name = name;

    /* round up the bottom of the stack to the 8-byte boundary */
    stk_limit = (uint32_t *)(((((uint32_t)stkSto - 1U) / 8) + 1U) * 8);

    /* pre-fill the unused part of the stack with 0xDEADBEEF */
    for (sp = sp - 1U; sp >= stk_limit; --sp) {
        *sp = 0xDEADBEEFU;
    }

    /* register the thread with the OS */
    OS_thread[OS_threadNum] = me;


    /* Notifica o SEGGER SystemView */
    SEGGER_SYSVIEW_OnTaskCreate((U32)me);

    memset(&Info, 0, sizeof(Info));
    Info.TaskID    = (U32)me;
    Info.sName     = me->name;
    Info.Prio      = OS_threadNum;       /* No MiROS, o índice no array é a prioridade */
    Info.StackBase = (U32)stkSto;        /* Base da stack (endereço mais baixo) */
    Info.StackSize = stkSize;            /* Tamanho total em bytes */
    SEGGER_SYSVIEW_SendTaskInfo(&Info);

    /* make the thread ready to run (exceto a idle thread no índice 0) */
    if (OS_threadNum > 0U) {
    		OS_makeReady(OS_threadNum);
    }

    /* increment the number of registered threads */
    ++OS_threadNum;
}
/***********************************************/
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */

  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
  RCC_OscInitStruct.PLL.PLLN = 85;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    //Default_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    //Default_Handler();
  }
}

void OS_init(void *stkSto, uint32_t stkSize) {
	SystemClock_Config();
	SysTick->CTRL = 0U;
	SystemCoreClockUpdate();

	/* set the PendSV interrupt priority to the lowest level 0xFF */
    *(uint32_t volatile *)0xE000ED20 |= (0xFFU << 16);

    /* start idleThread thread */
    OSThread_start(&idleThread,
                   &main_idleThread,
				   "idle_thread",
                   stkSto, stkSize);
}

void OS_onStartup(void) {
    SysTick_Config(SystemCoreClock / TICKS_PER_SEC);

    /* set the SysTick interrupt priority (highest) */
    NVIC_SetPriority(SysTick_IRQn, 0U);
}

void OS_onIdle(void) {
#ifdef NDBEBUG
    __WFI(); /* stop the CPU and Wait for Interrupt */
#endif
}

OSSemaphore::OSSemaphore(uint32_t initial_count) {
    counter = initial_count;
    waitSet = 0U;
}

void OSSemaphore::wait() {
	uint32_t primask = __get_PRIMASK();
	__disable_irq();  // Início da seção crítica

    if (counter > 0U) {
        counter--; // Tem crédito: consome e continua rodando
    } else {
        // Sem créditos: bloqueia a thread atual
        // 1. Tira do estado de "Pronta" no sistema operacional
        OS_readySet &= ~(1U << (OS_currIdx - 1U));

        // 2. Coloca o bit desta thread na fila de espera deste semáforo específico
        waitSet |= (1U << (OS_currIdx - 1U));

        // 3. Chama o escalonador para passar a CPU para outra tarefa
        SEGGER_SYSVIEW_OnTaskStopReady((U32)OS_curr, 2U);
        OS_sched();
    }

    __set_PRIMASK(primask); // Fim da seção crítica
}

void OSSemaphore::signal() {
	uint32_t primask = __get_PRIMASK();
	 __disable_irq(); // Início da seção crítica

    if (waitSet != 0U) {
        // Se waitSet é diferente de 0, tem pelo menos uma thread bloqueada.
        // O truque bitwise (waitSet & ~(waitSet - 1U)) isola apenas o primeiro bit que for 1.
        uint32_t waitingThreadBit = waitSet & ~(waitSet - 1U);
        waitSet &= ~waitingThreadBit;

        uint8_t threadIdx = (uint8_t)(32U - __CLZ(waitingThreadBit));
        OS_makeReady(threadIdx);

    } else {
        counter++;
    }

    __set_PRIMASK(primask); // Fim da seção crítica
}

}//fim namespace

void Q_onAssert(char const *module, int loc) {
    /* TBD: damage control */
    (void)module; /* avoid the "unused parameter" compiler warning */
    (void)loc;    /* avoid the "unused parameter" compiler warning */
    NVIC_SystemReset();
}

/***********************************************/

extern "C" void OS_traceContextSwitch(void) {
    if (rtos::OS_curr == &rtos::idleThread) {
        SEGGER_SYSVIEW_OnIdle();
    } else {
        SEGGER_SYSVIEW_OnTaskStartExec((U32)rtos::OS_curr);
    }
}

extern "C"
__attribute__ ((naked, optimize("-fno-stack-protector")))
void PendSV_Handler(void) {
__asm volatile (

    /* __disable_irq(); */
    "  CPSID         I                 \n"

    /* if (OS_curr != (OSThread *)0) { */
    "  LDR           r1,=_ZN4rtos7OS_currE       \n"
    "  LDR           r1,[r1,#0x00]     \n"
    "  CBZ           r1,PendSV_restore \n"

    /*     push registers r4-r11 on the stack */
    "  PUSH          {r4-r11}          \n"

    /*     OS_curr->sp = sp; */
    "  LDR           r1,=_ZN4rtos7OS_currE       \n"
    "  LDR           r1,[r1,#0x00]     \n"
    "  STR           sp,[r1,#0x00]     \n"
    /* } */

    "PendSV_restore:                   \n"
    /* sp = OS_next->sp; */
    "  LDR           r1,=_ZN4rtos7OS_nextE       \n"
    "  LDR           r1,[r1,#0x00]     \n"
    "  LDR           sp,[r1,#0x00]     \n"

    /* OS_curr = OS_next; */
    "  LDR           r1,=_ZN4rtos7OS_nextE       \n"
    "  LDR           r1,[r1,#0x00]     \n"
    "  LDR           r2,=_ZN4rtos7OS_currE       \n"
    "  STR           r1,[r2,#0x00]     \n"

	/* Notifica o SystemView preservando EXC_RETURN (lr) e alinhamento de 8 bytes (r0) */
	"  PUSH          {r0, lr}          \n"
	"  BL            OS_traceContextSwitch \n"
	"  POP           {r0, lr}          \n"

    /* pop registers r4-r11 */
    "  POP           {r4-r11}          \n"

    /* __enable_irq(); */
    "  CPSIE         I                 \n"

    /* return to the next thread */
    "  BX            lr                \n"
    );
}
