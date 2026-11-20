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
	uint32_t id;
};

produto fila[CAPACIDADE];

produto *head = fila[0];
produto *tail = fila[0];

uint32_t stack_produtor[512];
rtos::OSThread produtor;
void main_produtor(){
	while(1){

	}
}

uint32_t stack_consumidor[512];
rtos::OSThread consumidor;
void main_consumidor(){
	while(1){

	}
}

uint32_t stack_idleThread[512];

int main(void)
{
	  rtos::OS_init(stack_idleThread, sizeof(stack_idleThread));

	  SEGGER_SYSVIEW_Conf(); /* Configure and initialize SystemView */
	  SEGGER_SYSVIEW_Start(); // Start SystemView

	  rtos::OSSemaphore MutexFila(1U);
	  rtos::OSSemaphore EspaçoFila(CAPACIDADE);

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


