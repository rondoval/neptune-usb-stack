#ifndef USBAUDIO_CLASS_H
#define USBAUDIO_CLASS_H

/*
 *----------------------------------------------------------------------------
 *                       Includes for usbaudio class
 *----------------------------------------------------------------------------
 *                   By Chris Hodges <chrisly@platon42.de>
 */

#include "common.h"

#include <devices/usb_audio.h>
#include <libraries/neptune_class.h>

#include <devices/ahi.h>
#include <libraries/ahi_sub.h>

#include <string.h>
#include <stddef.h>
#include <stdio.h>

#include "usbaudio.h"


/* Protos */

struct NepClassAudio * usbAttemptInterfaceBinding(struct NepAudioBase *nh, struct PsdInterface *pif);
struct NepClassAudio * usbForceInterfaceBinding(struct NepAudioBase *nh, struct PsdInterface *pif);
void usbReleaseInterfaceBinding(struct NepAudioBase *nh, struct NepClassAudio *nch);

struct NepAudioSubLibBase * subLibInit(struct NepAudioSubLibBase * nas asm("d0"), BPTR seglist asm("a0"), struct ExecBase * SysBase asm("a6"));

struct NepAudioSubLibBase * subLibOpen(ULONG version asm("d0"), struct NepAudioSubLibBase * nas asm("a6"));
         
BPTR subLibClose(struct NepAudioSubLibBase * nas asm("a6"));
         
BPTR subLibExpunge(struct NepAudioSubLibBase * extralh asm("d0"), struct NepAudioSubLibBase * nas asm("a6"));
         
struct NepAudioSubLibBase * subLibReserved(struct NepAudioSubLibBase * nas asm("a6"));

ULONG subLibAllocAudio(struct TagItem * tags asm("a1"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));    

void subLibFreeAudio(struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));

void subLibDisable(struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));

void subLibEnable(struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));

ULONG subLibStart(ULONG flags asm("d0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));

ULONG subLibUpdate(ULONG flags asm("d0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));

ULONG subLibStop(ULONG flags asm("d0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));
         
ULONG subLibSetVol(UWORD channel asm("d0"), Fixed volume asm("d1"), sposition pan asm("d2"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), ULONG flags asm("d3"), struct NepAudioSubLibBase * nas asm("a6"));

ULONG subLibSetFreq(UWORD channel asm("d0"), ULONG freq asm("d1"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), ULONG flags asm("d2"), struct NepAudioSubLibBase * nas asm("a6"));

ULONG subLibSetSound(UWORD channel asm("d0"), UWORD sound asm("d1"), ULONG offset asm("d2"), LONG length asm("d3"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), ULONG flags asm("d4"), struct NepAudioSubLibBase * nas asm("a6"));

ULONG subLibSetEffect(ULONG * effect asm("a0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));

ULONG subLibLoadSound(UWORD sound asm("d0"), ULONG type asm("d1"), APTR info asm("a0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));

ULONG subLibUnloadSound(UWORD sound asm("d0"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));

IPTR subLibGetAttr(ULONG attr asm("d0"), LONG arg asm("d1"), LONG defvalue asm("d2"), struct TagItem * tags asm("a1"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));

ULONG subLibHardwareControl(ULONG attr asm("d0"), LONG arg asm("d1"), struct AHIAudioCtrlDrv * audioctrl asm("a2"), struct NepAudioSubLibBase * nas asm("a6"));

ULONG subLibPlayerIntV4(struct NepAudioMode * nam asm("a1"));
          
ULONG subLibPlayerIntV6(struct NepAudioMode * nam asm("a1"));

ULONG subLibPlayerIntDummy(struct NepAudioMode * nam asm("a1"));

struct NepClassAudio * nAllocAudio(void);
void nFreeAudio(struct NepClassAudio *nch);

BOOL nLoadClassConfig(struct NepAudioBase *nh);
LONG nOpenCfgWindow(struct NepAudioBase *nh);

void nGUITaskCleanup(struct NepAudioBase *nh);

void nAudioTask();
void nGUITask();

void nOutReqHook(struct Hook * hook asm("a0"), struct IOUsbHWRTIso * urti asm("a2"), struct IOUsbHWBufferReq * ubr asm("a1"));

void nInReqHook(struct Hook * hook asm("a0"), struct IOUsbHWRTIso * urti asm("a2"), struct IOUsbHWBufferReq * ubr asm("a1"));

void nInDoneHook(struct Hook * hook asm("a0"), struct IOUsbHWRTIso * urti asm("a2"), struct IOUsbHWBufferReq * ubr asm("a1"));

void nReleaseHook(struct Hook * hook asm("a0"), APTR prt asm("a2"), APTR unused asm("a1"));

#endif /* USBAUDIO_CLASS_H */
