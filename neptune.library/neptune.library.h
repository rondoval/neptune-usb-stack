#ifndef NEPTUNE_LIBRARY_H
#define NEPTUNE_LIBRARY_H

/*
 *----------------------------------------------------------------------------
 *                         Includes for neptune.library
 *----------------------------------------------------------------------------
 * Based on Poseidon's poseidon.library.h by Chris Hodges <chrisly@platon42.de>.
 */

/* Release date stamp, exposed as PA_ReleaseVersion. NeptuneUSB compares it against the
 * GCA_PrefsVersion stored in the user's config and forces a class DirScan when this is
 * newer - that is how an upgrade picks up new classes. Bump it once per release, by
 * hand: it must stay stable across rebuilds, or every rebuild would force a rescan. */
#define RELEASEVERSION 0x20260729

/* genmodule LC_LIBDEFS_FILE + <aros/libcall.h>/asmcall.h/symbolsets.h removed:
   their calling-convention macros are no longer used (de-AROS'd to plain C). */

#include <sys/time.h>

#include <exec/types.h>
#include <exec/lists.h>
#include <exec/alerts.h>
#include <exec/memory.h>
#include <exec/libraries.h>
#include <exec/interrupts.h>
#include <exec/semaphores.h>
#include <exec/execbase.h>
#include <exec/devices.h>
#include <exec/io.h>
#include <exec/ports.h>
#include <exec/errors.h>
#include <exec/resident.h>
#include <exec/initializers.h>

#include <devices/timer.h>
#include <devices/newstyle.h>
#include <utility/utility.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <intuition/intuition.h>

#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#include "neptune_intern.h"

#include <devices/usb.h>
#include <devices/usb_hub.h>
#include <devices/usb_hid.h>
#include <devices/usb_massstorage.h>
#include <devices/usbhardware.h>
#include <devices/usbhcd_context.h>    /* the context HCD ABI (lifecycle ops) */
#include <libraries/neptune_class.h>

/* Internal calls to our own LVO functions go through the inline stubs with the
   libbase = the in-scope `ps` parameter (every LVO function has it in a6). The
   LVO function DEFINITIONS are parenthesised so these macros don't expand there. */
#define NEPTUNE_BASE_NAME ps
/* Use the inline stubs (call macros) only - NOT <proto/neptune.h>, whose plain
   clib prototypes would conflict with our register-arg LVO definitions. */
#include <inline/neptune.h>

struct PsdRawDoFmt
{
    ULONG rdf_Len;
    STRPTR rdf_Buf;
};

/* Protos */

void pFreeEndpoint(struct PsdEndpoint *pep);
struct PsdEndpoint * pAllocEndpoint(struct PsdInterface *pif);

void pFreeInterface(struct PsdInterface *pif);
struct PsdInterface * pAllocInterface(struct PsdConfig *pc);

void pFreeConfig(struct PsdConfig *pc);
struct PsdConfig * pAllocConfig(struct PsdDevice *pd);

void pCheckForDeadlock(struct PsdBase *ps, struct PsdLockSem *pls, BOOL excl);
void pInitSem(struct PsdBase *ps, struct PsdLockSem *pls, STRPTR name);
void pDeleteSem(struct PsdBase *ps, struct PsdLockSem *pls);
void pLockSemExcl(struct PsdBase *ps, struct PsdLockSem *pls);
void pLockSemShared(struct PsdBase *ps, struct PsdLockSem *pls);
void pUnlockSem(struct PsdBase *ps, struct PsdLockSem *pls);

BOOL pOpenDOS(struct PsdBase *ps);
BOOL pHaveDOS(struct PsdBase *ps);

UWORD pAllocDevAddr(struct PsdDevice *pd);

BOOL pFixBrokenConfig(struct PsdPipe *pp);
BOOL pGetDevConfig(struct PsdPipe *pp);

ULONG pGetFormLength(struct PsdIFFContext *pic);

struct PsdIFFContext * pAllocForm(struct PsdBase *ps, struct PsdIFFContext *parent, ULONG formid);
void pFreeForm(struct PsdBase *ps, struct PsdIFFContext *pic);
ULONG * pInternalWriteForm(struct PsdIFFContext *pic, ULONG *buf);
struct PsdIFFContext * pAddCfgChunk(struct PsdBase *ps, struct PsdIFFContext *pic, APTR chunk);
STRPTR pGetStringChunk(struct PsdBase *ps, struct PsdIFFContext *pic, ULONG chunkid);
BOOL pMatchStringChunk(struct PsdBase *ps, struct PsdIFFContext *pic, ULONG chunkid, CONST_STRPTR str);

BOOL pRemCfgChunk(struct PsdBase *ps, struct PsdIFFContext *pic, ULONG chnkid);
BOOL pAddStringChunk(struct PsdBase *ps, struct PsdIFFContext *pic, ULONG chunkid, CONST_STRPTR str);
void pUpdateGlobalCfg(struct PsdBase *ps, struct PsdIFFContext *pic);
APTR pFindCfgChunk(struct PsdBase *ps, struct PsdIFFContext *pic, ULONG chnkid);

BOOL pGetDevConfig(struct PsdPipe *pp);

void pClassScan(struct PsdBase *ps);
void pReleaseBinding(struct PsdBase *ps, struct PsdDevice *pd, struct PsdInterface *pif);
void pReleaseDevBinding(struct PsdBase *ps, struct PsdDevice *pd);
void pReleaseIfBinding(struct PsdBase *ps, struct PsdInterface *pif);

BOOL pIsDeviceStillValid(struct PsdBase *ps, struct PsdDevice *pd);
BOOL pIsClassStillValid(struct PsdBase *ps, struct Library *ucb);

void pGarbageCollectEvents(struct PsdBase *ps);
BOOL pStartEventHandler(struct PsdBase *ps);

BOOL pCheckCfgChanged(struct PsdBase *ps);

ULONG pPowerRecurseDrain(struct PsdBase *ps, struct PsdDevice *pd);
void pPowerRecurseSupply(struct PsdBase *ps, struct PsdDevice *pd);

void pStripString(struct PsdBase *ps, STRPTR str);
struct Node * pFindName(struct PsdBase *ps, struct List *list, STRPTR name);

struct PsdDevice * pFindTTHub(struct PsdDevice *pd, UWORD *ttPort);
void pGetTTInfo(struct PsdDevice *pd, UWORD *ttHubAddr, UWORD *ttHubPort, UWORD *thinkTime, BOOL *isMultiTT);

#define psdAddErrorMsg0(level, origin, fmtstr) psdAddErrorMsgA(level, origin, fmtstr, NULL)

void pDeviceTask();
void pNotifyGUITask();
void pEventHandlerTask();

void pPutChar(char ch asm("d0"), struct PsdRawDoFmt * rdf asm("a3"));

void pRawFmtLength(char ch asm("d0"), ULONG * len asm("a3"));

#endif /* NEPTUNE_LIBRARY_H */
