#ifndef BOOTKEYBOARD_H
#define BOOTKEYBOARD_H

#include <devices/keyboard.h>
#include <libraries/keymap.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <libraries/mui.h>
#include <libraries/gadtools.h>

#define ID_ABOUT        0x55555555
#define ID_STORE_CONFIG 0xaaaaaaaa
#define ID_RESTORE_DEF  0x12345678
#define ID_LOAD_LAST    0x56789abc

struct ClsGlobalCfg
{
    ULONG cgc_ChunkID;
    ULONG cgc_Length;
    IPTR  cgc_RHEnable;
    IPTR  cgc_ResetDelay;
    IPTR  cgc_CapsLock;
    IPTR  cgc_ISAMap;
    IPTR  cgc_ExtraEmulDisable;
};

struct NepHidBase
{
    struct Library      nh_Library;       /* standard */
    BPTR                nh_SegList;       /* load seglist */
    UWORD               nh_Flags;         /* various flags */
    struct Library     *nh_UtilityBase;   /* utility base */

    struct Library     *nh_MUIBase;       /* MUI master base */
    struct Library     *nh_PsdBase;       /* neptune.library base */
    struct Library     *nh_IntBase;       /* Intuition base */
    struct Task        *nh_GUITask;       /* GUI Task */

    struct ClsGlobalCfg nh_CurrentCGC;

    BOOL                nh_UsingDefaultCfg;
    Object             *nh_App;
    Object             *nh_MainWindow;
    Object             *nh_RHEnableObj;
    Object             *nh_ResetDelayObj;
    Object             *nh_CapsLockObj;
    Object             *nh_ISAMapObj;
    Object             *nh_ExtraEmulObj;
    Object             *nh_UseObj;
    Object             *nh_CloseObj;

    Object             *nh_AboutMI;
    Object             *nh_UseMI;
    Object             *nh_RestoreDefMI;
    Object             *nh_LoadLastMI;
    Object             *nh_MUIPrefsMI;
};

struct NepClassHid
{
    struct Node         nch_Node;         /* Node linkage */
    struct NepHidBase  *nch_ClsBase;      /* Up linkage */
    struct Library     *nch_Base;         /* neptune.library base */
    struct PsdDevice   *nch_Device;       /* Up linkage */
    struct PsdConfig   *nch_Config;       /* Up linkage */
    struct PsdInterface *nch_Interface;   /* Up linkage */
    struct PsdPipe     *nch_EP0Pipe;      /* Endpoint 0 pipe */
    struct PsdEndpoint *nch_EP1;          /* Endpoint 1 */
    struct PsdPipe     *nch_EP1Pipe;      /* Endpoint 1 pipe */
    IPTR                nch_EP1PktSize;   /* Size of EP1 packets */
    UBYTE              *nch_EP1Buf;       /* Packet buffer for EP1 */
    struct Task        *nch_ReadySigTask; /* Task to send ready signal to */
    LONG                nch_ReadySignal;  /* Signal to send when ready */
    struct Task        *nch_Task;         /* Subtask */
    struct MsgPort     *nch_TaskMsgPort;  /* Message Port of Subtask */
    struct MsgPort     *nch_InpMsgPort;   /* input.device MsgPort */
    struct IOStdReq    *nch_InpIOReq;     /* input.device IORequest */
    struct InputEvent   nch_FakeEvent;    /* Input Event */
    IPTR                nch_IfNum;        /* Interface Number */
    UWORD               nch_InpCmd;       /* IND_ADDEVENT if listed, else IND_WRITEEVENT */
    BOOL                nch_IdleArmed;    /* Finite idle rate set at bind, first report still due */
    BOOL                nch_CapsLock;     /* Caps Lock pressed */
    UBYTE               nch_OldKeyArray[8]; /* Last keys pressed */
    ULONG               nch_OldQualifier; /* Previous qualifiers */
};

/* ROM-safe MUI base: the GUI subtask (bootkbd_GUITask) carries the libbase `nh` in
   tc_UserData; the shared accessor reaches nh_MUIBase through it. See classes/mui_base.h. */
#define MUI_BASE_USERDATA struct NepHidBase
#define MUI_BASE_FIELD    nh_MUIBase
#include "mui_base.h"

#endif /* BOOTKEYBOARD_H */
