#ifndef MASSSTORAGE_CLASS_H
#define MASSSTORAGE_CLASS_H

/*
 *----------------------------------------------------------------------------
 *                         Includes for MS class
 *----------------------------------------------------------------------------
 *                   By Chris Hodges <chrisly@platon42.de>
 */

#include "common.h"

#include <libraries/expansion.h>
#include <libraries/expansionbase.h>
#include <libraries/configregs.h>
#include <libraries/configvars.h>
#include <libraries/asl.h>

#include <dos/dostags.h>
#include <scsi/commands.h>
#include <scsi/values.h>
#include <dos/dosextens.h>
#include <dos/filehandler.h>
#include <resources/filesysres.h>

#include <devices/usb.h>
#include <devices/usbhardware.h>
#include <devices/usb_massstorage.h>
#include <libraries/neptune_class.h>

#include <string.h>
#include <stddef.h>
#include <stdio.h>

#include "massstorage.h"
#include "dev.h"

#ifndef TD_READ64
#define TD_READ64     24
#define TD_WRITE64    25
#define TD_SEEK64     26
#define TD_FORMAT64   27
#endif

/* The "%s (%ld)" tail every transport uses to report an ioerr. A format
   FRAGMENT plus its two arguments rather than a helper function: string
   literal concatenation happens at translation time, so the emitted format
   string - and therefore the message the user reads in NeptuneUSB - is
   byte-identical to the hand-written form at every call site, while the
   psdNumToStr() boilerplate is spelled once.
   psdAddErrorMsg() carries no format attribute (the sfd cannot express one),
   so this is also the only thing keeping the specifier and its two arguments
   in lock-step. Both need an in-scope `ps`, like psdAddErrorMsg() itself.
   MS_IOERR_ARGS names its argument twice: pass a plain lvalue, never a call.

     psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                    "CLEAR_ENDPOINT_HALT %ld failed: " MS_IOERR_FMT,
                    (ULONG) epnum, MS_IOERR_ARGS(ioerr)); */
#define MS_IOERR_FMT        "%s (%ld)"
#define MS_IOERR_ARGS(err)  psdNumToStr(NTS_IOERR, (err), "unknown"), (err)

/* Protos */

struct NepClassMS * usbAttemptInterfaceBinding(struct NepMSBase *nh, struct PsdInterface *pif);
struct NepClassMS * usbForceInterfaceBinding(struct NepMSBase *nh, struct PsdInterface *pif);
void usbReleaseInterfaceBinding(struct NepMSBase *nh, struct NepClassMS *ncm);

struct NepClassMS * nAllocMS(void);
void nFreeMS(struct NepClassMS *ncm);

BOOL nLoadClassConfig(struct NepMSBase *nh);
BOOL nLoadBindingConfig(struct NepClassMS *ncm, BOOL announce);
void nReapplyConfig(struct NepMSBase *nh);
void nWakeUnitTask(struct NepClassMS *ncm);
void nWakeTransport(struct NepClassMS *ncm);
LONG nOpenBindingCfgWindow(struct NepMSBase *nh, struct NepClassMS *ncm);

void nGUITaskCleanup(struct NepClassMS *ncm);
BOOL nStoreConfig(struct Library *ps, struct NepClassMS *ncm);
BOOL nTryFallback(struct Library *ps, struct NepClassMS *ncm, ULONG flag,
                  CONST_STRPTR name, BOOL enable, LONG rc);
BOOL nApplyFallback(struct NepClassMS *ncm, ULONG flag, CONST_STRPTR name,
                    BOOL enable, LONG rc);

void nHexString(UBYTE *src, ULONG len, UBYTE *buf);

LONG nScsiDirect(struct NepClassMS *ncm, struct SCSICmd *scsicmd);
LONG nScsiDirectBulk(struct NepClassMS *ncm, struct SCSICmd *scsicmd);
LONG nScsiDirectCBI(struct NepClassMS *ncm, struct SCSICmd *scsicmd);
LONG nScsiDirectUAS(struct NepClassMS *ncm, struct SCSICmd *scsicmd);

/* UAS multi-tag engine (massstorage_uas.c) */
BOOL nUasCollectEndpoints(struct NepClassMS *ncm);
BOOL nUasInitTags(struct NepClassMS *ncm);
void nUasDisableTags(struct NepClassMS *ncm);
BOOL nUasEligible(struct NepClassMS *ncm, struct IOStdReq *ioreq);
struct UasTag * nUasFreeTag(struct NepClassMS *ncm);
void nUasSubmitTag(struct NepClassMS *ncm, struct UasTag *ut, struct IOStdReq *ioreq);
void nUasProcessAborts(struct NepClassMS *ncm);
void nUasReapTags(struct NepClassMS *ncm);
/* ABORT TASK ladder for tags the device still owns after a host-side kill */
void nUasProcessQuarantine(struct NepClassMS *ncm);
void nUasDrainTags(struct NepClassMS *ncm);
BOOL nUasTagsIdle(struct NepClassMS *ncm); /* TRUE = no tag in flight (non-UAS: always) */
LONG nBulkReset(struct NepClassMS *ncm);
LONG nBulkClear(struct NepClassMS *ncm);
LONG nClearEndpointHalt(struct NepClassMS *ncm, UWORD epnum, BOOL is_in);
LONG nClearEndpointHaltMsg(struct NepClassMS *ncm, UWORD epnum, BOOL is_in);
void nLockXFer(struct NepClassMS *ncm);
void nUnlockXFer(struct NepClassMS *ncm);
void nSetNakTimeout(struct NepClassMS *ncm, struct PsdPipe *pp, ULONG timeout_ms);
ULONG nNakTimeoutMs(struct NepClassMS *ncm);
void nApplyNakTimeout(struct NepClassMS *ncm, ULONG timeout_ms);
UWORD nBuildRWCdb(UBYTE *cdb, BOOL iswrite, ULONG startblockhigh, ULONG startblock,
                  ULONG datalen, UWORD blockshift);
UWORD nBuildSenseCdb(UBYTE *cdb, ULONG senselen);
LONG nRead64(struct NepClassMS *ncm, struct IOStdReq *ioreq);
LONG nWrite64(struct NepClassMS *ncm, struct IOStdReq *ioreq);
LONG nFormat64(struct NepClassMS *ncm, struct IOStdReq *ioreq);
LONG nSeek64(struct NepClassMS *ncm, struct IOStdReq *ioreq);
LONG nGetGeometry(struct NepClassMS *ncm, struct IOStdReq *ioreq);
LONG nGetWriteProtect(struct NepClassMS *ncm);
LONG nStartStop(struct NepClassMS *ncm, struct IOStdReq *ioreq);

/* Removable task (massstorage_removable.c) */
BOOL nStartRemovableTask(struct Library *ps, struct NepMSBase *nh);
struct NepMSBase * nAllocRT(void);
void nFreeRT(struct NepMSBase *nh);
/* DOS access and node teardown (massstorage.class.c; shared with safe-eject) */
BOOL nOpenDOSLib(struct NepMSBase *nh);
BOOL nOpenDOS(struct NepMSBase *nh);
void nUnmountPartition(struct NepClassMS *ncm);
void nRemoveDosNode(struct NepClassMS *ncm, struct DeviceNode *node);
/* Mount recipe dispatch (massstorage.class.c: its helpers are shared with the
   config GUI, so only the entry point is exported) */
struct MountResult;
BOOL nMountDrive(struct NepClassMS *ncm, struct MountResult *stats);
LONG nIOCmdTunnel(struct NepClassMS *ncm, struct IOStdReq *ioreq);
LONG nScsiDirectTunnel(struct NepClassMS *ncm, struct SCSICmd *scsicmd);

BOOL nDosEntryMatches(struct NepClassMS *ncm, struct DosList *list);
struct DeviceNode * FindMatchingDevice(struct NepClassMS *ncm);
void nBusyName(struct NepClassMS *ncm, struct DeviceNode *node, STRPTR buf, ULONG len);
IPTR nSafeEjectDevice(struct NepMSBase *nh, struct NepClassMS *ncm, STRPTR busybuf, ULONG busybuflen);

void AutoDetectMaxTransfer(struct NepClassMS *ncm);

void nMSTask();
void nRemovableTask();
void nGUITask();

LONG LUNListDisplayHook(struct Hook * hook asm("a0"), char ** strarr asm("a2"), struct NepClassMS * ncm asm("a1"));

#endif /* MASSSTORAGE_CLASS_H */
