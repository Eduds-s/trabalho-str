/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */

#define CAPACIDADE 32
#define PERIODO_PRODUTOR   1U
#define PERIODO_CONSUMIDOR 2U

#include "main.h"
#include <cstdint>
#include "miros.h"
#include "SEGGER_SYSVIEW.h"

/*struct produto{
	uint16_t id;
};

produto fila[CAPACIDADE];

uint32_t erros = 0;

uint32_t head = 0;
uint32_t tail = 0;

rtos::OSSemaphore MutexFila(1U);
rtos::OSSemaphore BloqProdutor(CAPACIDADE);
rtos::OSSemaphore BloqConsumidor(0U);

uint32_t stack_produtor[512];
rtos::OSThread produtor;
void main_produtor(){
	produto item;
	static uint16_t proximo = 0;
	item.id = proximo++;
	BloqProdutor.wait();
	MutexFila.wait();
	fila[head] = item;
	head = (head+1) % CAPACIDADE;
	MutexFila.signal();
	BloqConsumidor.signal();
}

uint32_t stack_consumidor[512];
rtos::OSThread consumidor;
void main_consumidor(){
	static uint16_t esperado = 0;
	produto item;
	BloqConsumidor.wait();
	MutexFila.wait();
	item = fila[tail];
	tail = (tail+1)% CAPACIDADE;
	MutexFila.signal();
	BloqProdutor.signal();
	if (item.id != esperado) {
		erros++;
	}
	esperado = item.id + 1;
}
*/

#define PERIODO_ALTA   20U
#define PERIODO_MEDIA  30U
#define PERIODO_BAIXA 100U

rtos::OSMutex recursoAltaBaixa;

#define LOOPS_POR_TICK 800U

static void ocupar_cpu(uint32_t ticks) {
	for (volatile uint32_t i = 0U; i < (ticks * LOOPS_POR_TICK); i++) {
		/* so gasta tempo */
	}
}

uint32_t stack_alta[512];
rtos::OSThread alta;
void job_alta(){
	recursoAltaBaixa.lock();
	ocupar_cpu(1U);      /* secao critica curta */
	recursoAltaBaixa.unlock();
}

uint32_t stack_media[512];
rtos::OSThread media;
void job_media(){
	ocupar_cpu(15U);     /* nao usa o recurso: so ocupa a CPU */
}

uint32_t stack_baixa[512];
rtos::OSThread baixa;
void job_baixa(){
	recursoAltaBaixa.lock();
	ocupar_cpu(20U);     /* secao critica longa */
	recursoAltaBaixa.unlock();
}

uint32_t stack_idleThread[512];

int main(void)
{
	  rtos::OS_init(stack_idleThread, sizeof(stack_idleThread));

	  SEGGER_SYSVIEW_Conf(); /* Configure and initialize SystemView */
	  SEGGER_SYSVIEW_Start(); // Start SystemView
	  /*
	  rtos::OSThread_startPeriodic(&produtor,
			  &main_produtor,
			  "produtor",
			  PERIODO_PRODUTOR,
			  stack_produtor,
			  sizeof(stack_produtor));

	  rtos::OSThread_startPeriodic(&consumidor,
			  &main_consumidor,
			  "consumidor",
			  PERIODO_CONSUMIDOR,
			  stack_consumidor,
			  sizeof(stack_consumidor));
	  */

	  rtos::OSThread_startPeriodic(&alta,  &job_alta,  "alta",
			  PERIODO_ALTA,  stack_alta,  sizeof(stack_alta));

	  rtos::OSThread_startPeriodic(&media, &job_media, "media",
			  PERIODO_MEDIA, stack_media, sizeof(stack_media));

	  rtos::OSThread_startPeriodic(&baixa, &job_baixa, "baixa",
			  PERIODO_BAIXA, stack_baixa, sizeof(stack_baixa));


	  /* transfer control to the RTOS to run the threads */
	  rtos::OS_run();
}


