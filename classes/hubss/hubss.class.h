/*
    Copyright (C) 2014-2025, The AROS Development Team. All rights reserved.

    Desc: SuperSpeed USB3.0 hub class for Neptune, based on the Poseidon hubss class
          (itself based upon hub.h by Chris Hodges <chrisly@platon42.de>)
*/

#ifndef HUBSS_CLASS_H
#define HUBSS_CLASS_H

#include <exec/exec.h>
#include <libraries/neptune.h>

#include <devices/usb_hub.h>


struct NepHubSSBase {
    struct Library          nh_Library;       /* standard */
    UWORD                   nh_Flags;         /* various flags */
    BPTR                    nh_SegList;       /* load seglist (stored by the class skeleton) */

    struct Library          *nh_UtilityBase;   /* utility base */
    struct List             nh_Bindings;
};

struct NepClassHubSS {
    struct Node             nch_Node;         /* Node linkage */
    struct NepHubSSBase     *nch_HubBase;      /* hubss.usbclass base */
    struct Library          *nch_Base;         /* neptune.library base */
    struct PsdHardware      *nch_Hardware;     /* Up linkage */
    struct PsdDevice        *nch_Device;       /* Up linkage */
    struct PsdConfig        *nch_Config;       /* Up linkage */
    struct PsdInterface     *nch_Interface;   /* Up linkage */

    struct PsdEndpoint      *nch_EP1;          /* Endpoint 1 */
    struct PsdPipe          *nch_EP0Pipe;      /* Endpoint 0 pipe */
    struct PsdPipe          *nch_EP1Pipe;      /* Endpoint 1 pipe */

    BOOL                    nch_IOStarted;    /* IO Running */
    BOOL                    nch_Running;      /* Not suspended */
    struct Task             *nch_ReadySigTask; /* Task to send ready signal to */
    LONG                    nch_ReadySignal;  /* Signal to send when ready */
    struct Task             *nch_Task;         /* Subtask */
    struct MsgPort          *nch_TaskMsgPort;  /* Message Port of Subtask */
    struct MsgPort          *nch_CtrlMsgPort;  /* Message Port for control messages */

    UWORD                   nch_NumPorts;     /* Number of ports at this hub */
    UWORD                   nch_HubAttr;      /* Hub Characteristics (see UHCF flags) */
    UWORD                   nch_PwrGoodTime;  /* Time in ms for power to become good */
    UWORD                   nch_HubCurrent;   /* Max hub current in mA */
    UWORD                   nch_HubHdrDecLat;
    UWORD                   nch_HubDelay;
    UWORD                   nch_Removable;    /* Bitmask for device removable */

    ULONG                   nch_PowerCycle;   /* Bitmask of devices to powercycle */
    ULONG                   nch_DisablePort;  /* Bitmask of devices to disable */
    BOOL                    nch_IsSSHalf;     /* Own hub is the USB3/SS half of a physical hub (always TRUE here) */
    UBYTE                   *nch_ContainerId;  /* Own BOS Container ID (16 bytes, library-owned) or NULL */

    BOOL                    nch_ClassScan;    /* Flag to cause class scan */
    BOOL                    nch_Settling;     /* Port pass not complete: the task is not idle in Wait() */
    BOOL                    nch_IsRootHub;    /* Is this a Root Hub? */

    UBYTE                   nch_PortChanges[4]; /* Buffer for port changes */
    struct PsdDevice        **nch_Downstream;   /* Pointer to array of down stream device pointers */
};

struct NepHubSSMsg {
    struct Message          nhm_Msg;          /* Message body */
    ULONG                   nhm_MethodID;     /* The method ID (see neptune_class.h) */
    IPTR                    *nhm_Params;       /* Pointer to parameters of the method (all of them!) */
    IPTR                    nhm_Result;       /* Result of call */
};

#endif /* HUBSS_CLASS_H */
