#ifndef SYSVIEW_HOOKS_H_
#define SYSVIEW_HOOKS_H_

#define USE_SYSVIEW 1      /* 1 = liga o SystemView, 0 = desliga tudo */

#if USE_SYSVIEW
extern "C" {
#include "SEGGER_SYSVIEW.h"
}
#else
#define SEGGER_SYSVIEW_Conf()                       ((void)0)
#define SEGGER_SYSVIEW_Start()                      ((void)0)
#define SEGGER_SYSVIEW_OnIdle()                     ((void)0)
#define SEGGER_SYSVIEW_OnTaskStartExec(id)          ((void)(id))
#define SEGGER_SYSVIEW_OnTaskStartReady(id)         ((void)(id))
#define SEGGER_SYSVIEW_OnTaskStopReady(id, cause)   ((void)(id), (void)(cause))
#define SEGGER_SYSVIEW_RecordEnterISR()             ((void)0)
#define SEGGER_SYSVIEW_RecordExitISRToScheduler()   ((void)0)
#endif

#endif
