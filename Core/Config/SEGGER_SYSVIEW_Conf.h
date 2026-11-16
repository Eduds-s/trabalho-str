/*********************************************************************
*                   (c) SEGGER Microcontroller GmbH                  *
*                        The Embedded Experts                        *
*                           www.segger.com                           *
**********************************************************************
*                                                                    *
*         SEGGER SystemView  * Real-time application analysis        *
*              https://github.com/SEGGERMicro/SystemView             *
*                                                                    *
**********************************************************************

---------------------------END-OF-HEADER------------------------------

Purpose : SEGGER SystemView configuration file.
          Set defines which deviate from the defaults (see SEGGER_SYSVIEW_ConfDefaults.h) here.          

Additional information:
  Required defines which must be set are:
    SEGGER_SYSVIEW_GET_TIMESTAMP
    SEGGER_SYSVIEW_GET_INTERRUPT_ID
  For known compilers and cores, these might be set to good defaults
  in SEGGER_SYSVIEW_ConfDefaults.h.
  
  SystemView needs a (nestable) locking mechanism.
  If not defined, the RTT locking mechanism is used,
  which then needs to be properly configured.
*/

#ifndef SEGGER_SYSVIEW_CONF_H
#define SEGGER_SYSVIEW_CONF_H

/*********************************************************************
*
*       Defines, configurable
*
**********************************************************************
*/

#define SEGGER_SYSVIEW_RTT_BUFFER_SIZE 64000
#define SEGGER_SYSVIEW_ID_SHIFT   0

#define SEGGER_SYSVIEW_GET_TIMESTAMP() ((*(U32 *)(0xE0001004)) >> 4)
#define SEGGER_SYSVIEW_TIMESTAMP_BITS (28)

/*********************************************************************
*
* SysView Id configuration
*/
// Default value for the lowest Id reported by the application.
// Can be overridden by the application via SEGGER_SYSVIEW_SetRAMBase().
#define SEGGER_SYSVIEW_ID_BASE 0x20000000
// Number of bits to shift the Id to save bandwidth.
// (e.g. 2 when all reported Ids (pointers) are 4 byte aligned)
#define SEGGER_SYSVIEW_ID_SHIFT 0
/*********************************************************************
*
* SysView interrupt configuration
*/
// Get the currently active interrupt Id. (read Cortex-M ICSR[8:0] = active vector)
#define SEGGER_SYSVIEW_GET_INTERRUPT_ID() ((*(U32 *)(0xE000ED04)) & 0x1FF)
/*********************************************************************
*
* SysView locking
*/
// Lock SysView (nestable)
#define SEGGER_SYSVIEW_LOCK() SEGGER_RTT_LOCK()
// Unlock SysView (nestable)
#define SEGGER_SYSVIEW_UNLOCK() SEGGER_RTT_UNLOCK()

/*********************************************************************
*
*       Define: SEGGER_SYSVIEW_SECTION
*
*  Description
*    Section to place the SystemView RTT Buffer into.
*  Default
*    undefined: Do not place into a specific section.
*  Notes
*    If SEGGER_RTT_SECTION is defined, the default changes to use
*    this section for the SystemView RTT Buffer, too.
*/
#if !(defined SEGGER_SYSVIEW_SECTION) && (defined SEGGER_RTT_BUFFER_SECTION)
  #define SEGGER_SYSVIEW_SECTION                  SEGGER_RTT_BUFFER_SECTION
#endif


/*********************************************************************
* TODO: Add your defines here.                                       *
**********************************************************************
*/


#endif  // SEGGER_SYSVIEW_CONF_H

/*************************** End of file ****************************/
