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

#include "main.h"
#include <cstdint>
#include "miros.h"
#include "SEGGER_SYSVIEW.h"

struct produto{
	uint16_t id;
};

produto fila[CAPACIDADE];

uint32_t erros = 0;   // itens fora de ordem/perdidos (ver no debugger: deve ficar 0)

uint32_t head = 0;
uint32_t tail = 0;

rtos::OSSemaphore MutexFila(1U);
rtos::OSSemaphore BloqProdutor(CAPACIDADE);
rtos::OSSemaphore BloqConsumidor(0U);

uint32_t stack_produtor[512];
rtos::OSThread produtor;
void main_produtor(){
	while(1){
		produto item;
		static uint16_t proximo = 0;
		item.id = proximo++;
		BloqProdutor.wait();
		MutexFila.wait();
		fila[head] = item;
		head = (head+1) % CAPACIDADE;
		MutexFila.signal();
		BloqConsumidor.signal();
		rtos::OS_delay(3U);
	}
}

uint32_t stack_consumidor[512];
rtos::OSThread consumidor;
void main_consumidor(){
	while(1){
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
		}   // fila FIFO: os ids devem chegar em sequencia
		esperado = item.id + 1;
		rtos::OS_delay(3U);
	}
}

uint32_t stack_idleThread[512];

int main(void)
{
	  rtos::OS_init(stack_idleThread, sizeof(stack_idleThread));

	  SEGGER_SYSVIEW_Conf(); /* Configure and initialize SystemView */
	  SEGGER_SYSVIEW_Start(); // Start SystemView

	  //5tart produtor
	  rtos::OSThread_start(&produtor,
			  &main_produtor,
			  "produtor",
			  stack_produtor,
			  sizeof(stack_produtor));

	  //start consumidor
	  rtos::OSThread_start(&consumidor,
	  			  &main_consumidor,
	  			  "consumidor",
	  			  stack_consumidor,
	  			  sizeof(stack_consumidor));


	  /* transfer control to the RTOS to run the threads */
	  rtos::OS_run();
}


