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
#define PERIODO_PRODUTOR   20U
#define PERIODO_CONSUMIDOR 30U

#include "main.h"
#include <cstdint>
#include "miros.h"
#include "SEGGER_SYSVIEW.h"

struct produto{
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

uint32_t stack_idleThread[512];

int main(void)
{
	  rtos::OS_init(stack_idleThread, sizeof(stack_idleThread));

	  SEGGER_SYSVIEW_Conf(); /* Configure and initialize SystemView */
	  SEGGER_SYSVIEW_Start(); // Start SystemView

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


	  /* transfer control to the RTOS to run the threads */
	  rtos::OS_run();
}


