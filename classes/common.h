/* common.h - shared include aggregator for all Neptune USB class drivers.
 *
 * Every class compiles the shared classes/class_main.c skeleton and pulls in
 * this header for the common NDK/Neptune includes. Per-class identity comes
 * from the class's CMakeLists via -D: the source uses CLASS_NAME directly, plus
 * VERSION_STRING - the $VER cookie built from it and the distribution version.
 */

#include "class_version.h"

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/alerts.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/interrupts.h>
#include <exec/semaphores.h>
#include <exec/execbase.h>
#define EXEC_BASE_NAME (*(struct ExecBase **)4UL)
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/errors.h>
#include <exec/resident.h>
#include <exec/initializers.h>

#include <devices/timer.h>
#include <devices/input.h>
#include <devices/newstyle.h>
#include <utility/utility.h>
#include <dos/dos.h>
#include <intuition/intuition.h>

#include <devices/usb.h>
#include <devices/usbhardware.h>
#include <libraries/usbclass.h>

#include <string.h>
#include <stddef.h>
#include <stdio.h>

#include <proto/alib.h>
#include <proto/dos.h>
#include <proto/commodities.h>
#include <proto/intuition.h>
#include <proto/neptune.h>
#include <proto/utility.h>
#include <proto/keymap.h>
#include <proto/layers.h>
#include <proto/input.h>
#include <proto/expansion.h>
#include <proto/exec.h>

#define NewList NEWLIST

#include <stdarg.h>

#define min(x,y) (((x) < (y)) ? (x) : (y))
#define max(x,y) (((x) > (y)) ? (x) : (y))

/* Tasks that feed input.device (hid, bootkeyboard, bootmouse) must sit above
 * DOS handlers (5) and the band dynamic schedulers manage (Executive: <= 5).
 * With IND_ADDEVENT, input.device (pri 20) auto-repeats a key until *this*
 * task delivers the key-up, and only this task re-arms the interrupt IN pipe;
 * a busy console handler at 5 could hold both back for seconds.
 * A floor, not an override: a higher pgc_SubTaskPri wins.
 */
#define INPUT_CLASS_TASK_PRI 10

static inline void nApplyInputTaskPriFloor(void)
{
    struct Task *task = FindTask(NULL);
    if(task->tc_Node.ln_Pri < INPUT_CLASS_TASK_PRI)
    {
        SetTaskPri(task, INPUT_CLASS_TASK_PRI);
    }
}

/* TRUE if the input.device behind ioreq (just opened, idle) implements
   IND_ADDEVENT, the V47 (AmigaOS 3.2) command. Asks the device itself with
   NSCMD_DEVICEQUERY instead of trusting lib_Version alone: the 68k
   input.device 50.x is V50 but does not answer the query, and its command
   24 (CMD_NONSTD+15) is a different function - fed key events with it, a key
   repeats for ever. The 3.2 input.device (47.1) answers and lists 24.
   Below V47 the query is not even sent. Leaves io_Command/io_Data/io_Length
   changed; every sender sets all three before its DoIO. */
static inline BOOL nInputHasAddEvent(struct IOStdReq *ioreq)
{
    struct NSDeviceQueryResult nsdq;
    UWORD *cmd;

    if(ioreq->io_Device->dd_Library.lib_Version < 47)
    {
        return(FALSE);
    }
    nsdq.nsdqr_DevQueryFormat = 0;
    nsdq.nsdqr_SizeAvailable = 0;
    nsdq.nsdqr_SupportedCommands = NULL;
    ioreq->io_Command = NSCMD_DEVICEQUERY;
    ioreq->io_Data = &nsdq;
    ioreq->io_Length = sizeof(nsdq);
    ioreq->io_Actual = 0;
    if(DoIO((struct IORequest *) ioreq) || (ioreq->io_Actual < sizeof(nsdq)) ||
       (nsdq.nsdqr_SizeAvailable < sizeof(nsdq)) || !nsdq.nsdqr_SupportedCommands)
    {
        return(FALSE);
    }
    for(cmd = (UWORD *) nsdq.nsdqr_SupportedCommands; *cmd; cmd++)
    {
        if(*cmd == IND_ADDEVENT)
        {
            return(TRUE);
        }
    }
    return(FALSE);
}

/* Log wordings shared by every class driver. */
#define NEP_BOUND_TXT     "Bound '%s' to %s unit %ld."   /* devname, unit name, unit no */
#define NEP_BOUND1_TXT    "Bound '%s'."                  /* devname */
#define NEP_RELEASED_TXT  "Released '%s'."               /* devname */
#define NEP_GIVEUP_TXT    "Too many consecutive transfer errors; shutting the device down."
