/*********************************************************************
*                   (c) SEGGER Microcontroller GmbH                  *
*                        The Embedded Experts                        *
*                           www.segger.com                           *
**********************************************************************
*                                                                    *
*        SEGGER RTT * Real Time Transfer for embedded targets        *
*                  https://github.com/SEGGERMicro/RTT                *
*                                                                    *
**********************************************************************

---------------------------END-OF-HEADER------------------------------
Purpose : User configuration file for RTT.
          For available configuration,
          refer to SEGGER_RTT_ConfDefaults.h.

----------------------------------------------------------------------
*/

#ifndef SEGGER_RTT_CONF_H
#define SEGGER_RTT_CONF_H


/*********************************************************************
*
*       Defines, configurable
*
**********************************************************************
*/
#define BUFFER_SIZE_UP   4
#define BUFFER_SIZE_DOWN   4

#define SEGGER_RTT_LOCK()   {                                                 \
                              unsigned int LockState;                         \
                              __asm volatile ("mrs   %0, primask  \n\t"       \
                                              "cpsid i            \n\t"       \
                                              : "=r" (LockState)              \
                                              :                               \
                                              : "memory"                      \
                                              );

#define SEGGER_RTT_UNLOCK()   __asm volatile ("msr   primask, %0  \n\t"       \
                                              :                               \
                                              : "r" (LockState)               \
                                              : "memory"                      \
                                              );                              \
                            }

#endif
/*************************** End of file ****************************/
