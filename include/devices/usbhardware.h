#ifndef DEVICES_USBHARDWARE_H
#define DEVICES_USBHARDWARE_H
/*
**	$VER: usbhardware.h 3.5 (07.07.2026)
**
**	standard usb hardware device include file
**  USB 2.0 only, 3.0 support removed
**  Classic V1/V2 IOUsbHWReq, as used by Neptune; Poseidon's V3 interface
**  is not defined here.
**
**	(C) Copyright 2002-2007 Chris Hodges
**	(C) Copyright 2007-2026 AROS Development Team
**	    All Rights Reserved
*/

#ifndef EXEC_IO_H
#include "exec/io.h"
#endif

#ifndef EXEC_ERRORS_H
#include <exec/errors.h>
#endif

#ifndef DEVICES_USB_H
#include "devices/usb.h"
#endif

/* Shared value pool (error codes, capability tag/bits, iso buffer flags) -
 * common to this legacy ABI and the context ABI (usbhcd_context.h). */
#ifndef DEVICES_USBHCD_COMMON_H
#include "devices/usbhcd_common.h"
#endif

/* Common base (V1) fields */
#define IOUSBHWREQ_V1_FIELDS                                                                                                                         \
    struct IORequest    iouh_Req;               /* basic IOReq                                                                                      */\
    UWORD               iouh_Flags;             /* Transfer flags                                                                                   */\
    UWORD               iouh_State;             /* USB State Flags                                                                                  */\
    UWORD               iouh_Dir;               /* Direction of transfer                                                                            */\
    UWORD               iouh_DevAddr;           /* USB Device Address (0-127)                                                                       */\
    UWORD               iouh_Endpoint;          /* USB Device Endpoint (0-15)                                                                       */\
    UWORD               iouh_MaxPktSize;        /* Maximum packet size                                                                              */\
    ULONG               iouh_Actual;            /* Actual bytes transferred                                                                         */\
    ULONG               iouh_Length;            /* Size of buffer                                                                                   */\
    APTR                iouh_Data;              /* Pointer to in/out buffer                                                                         */\
    UWORD               iouh_Interval;          /* Interrupt Interval (ms or 125us units)                                                           */\
    ULONG               iouh_NakTimeout;        /* Timeout in ms before request retired                                                             */\
    struct UsbSetupData iouh_SetupData;         /* Setup fields for ctrl transfers                                                                  */\
    APTR                iouh_UserData;          /* private data, stack-owned                                                                        */\
    UWORD               iouh_ExtError           /* Extended error code                                                                              */

/* V2 extension fields (added after V1) */
#define IOUSBHWREQ_V2_FIELDS                                                                                                                          \
    UWORD               iouh_Frame;             /* current USB-Frame / ISO start frame                                                              */\
    UWORD               iouh_SplitHubAddr;      /* Split-Transaction HUB address                                                                    */\
    UWORD               iouh_SplitHubPort;      /* Split-Transaction HUB downstream port                                                            */\
    APTR                iouh_DriverPrivate1;    /* private data for internal driver use                                                             */\
    APTR                iouh_DriverPrivate2     /* private data for internal driver use                                                             */

/* IO Request structure */
/* Original V1 layout (kept for ABI/compat) */
struct IOUsbHWReqV1
{
    IOUSBHWREQ_V1_FIELDS;
};
#define IOUsbHWReqObsolete  IOUsbHWReqV1

/* Current version = V1 + V2 extension */
struct IOUsbHWReq
{
    IOUSBHWREQ_V1_FIELDS;
    IOUSBHWREQ_V2_FIELDS;
};
#define IOUsbHWReqV2 IOUsbHWReq

/* Realtime ISO transfer structure as given in iouh_Data */
struct IOUsbHWRTIso
{
    struct Node         *urti_Node;             /* Driver's linkage (private)                                                                       */
    struct Hook         *urti_InReqHook;        /* Called with struct IOUsbHWBufferReq whenever input data has arrived and is ready to be copied    */
    struct Hook         *urti_OutReqHook;       /* Called with struct IOUsbHWBufferReq to prepare output buffer copying                             */
    struct Hook         *urti_InDoneHook;       /* Called with struct IOUsbHWBufferReq when input buffer has been copied                            */
    struct Hook         *urti_OutDoneHook;      /* Called with struct IOUsbHWBufferReq when output buffer has been sent                             */
    ULONG               urti_OutPrefetch;       /* Maximum prefetch in bytes allowed for output                                                     */
    APTR                urti_DriverPrivate1;    /* private data for internal driver use                                                             */
    APTR                urti_DriverPrivate2;    /* private data for internal driver use                                                             */
};

struct IOUsbHWBufferReq
{
    UBYTE               *ubr_Buffer;            /* Pointer to buffer, filled by called function                                                     */
    ULONG               ubr_Length;             /* Length of input received or output to be sent (may be adjusted by hook to force a partial copy)  */
    UWORD               ubr_Frame;              /* Frame number, filled by caller (may be adjusted by output hook)                                  */
    UWORD               ubr_Flags;              /* Flags, may be inspected and changed by hooks                                                     */
};

/* ubr_Flags bits (UBFB_CONTBUFFER, UHCD_UBB_XFER_ERROR) live in usbhcd_common.h */

/* non-standard commands */
/* UHCMD_QUERYDEVICE/USBRESET (== CMD_NONSTD+0..1) live in usbhcd_common.h */
#define UHCMD_USBSUSPEND        CMD_STOP
#define UHCMD_USBOPER           CMD_START
#define UHCMD_USBRESUME         (CMD_NONSTD + 2)
#define UHCMD_CONTROLXFER       (CMD_NONSTD + 3)
#define UHCMD_ISOXFER           (CMD_NONSTD + 4)
#define UHCMD_INTXFER           (CMD_NONSTD + 5)
#define UHCMD_BULKXFER          (CMD_NONSTD + 6)
#define UHCMD_ADDISOHANDLER     (CMD_NONSTD + 7)
#define UHCMD_REMISOHANDLER     (CMD_NONSTD + 8)
#define UHCMD_STARTRTISO        (CMD_NONSTD + 9)
#define UHCMD_STOPRTISO         (CMD_NONSTD + 10)

/* Error codes for io_Error field (UHIOERR_*) live in usbhcd_common.h */

/* Values for iouh_Dir */
#define UHDIR_SETUP             0               /* This is a setup transfer (UHCMD_CTRLXFER)                                                        */
#define UHDIR_OUT               1               /* This is a host to device transfer                                                                */
#define UHDIR_IN                2               /* This is a device to host transfer                                                                */

/* Definitions for iouh_Flags */
#define UHFB_LOWSPEED           0               /* Device operates at low speed                                                                     */
#define UHFB_HIGHSPEED          1               /* Device operates at high speed (USB 2.0)                                                          */
#define UHFB_NOSHORTPKT         2               /* Inhibit sending of a short packet at the end of a transfer (if possible)                         */
#define UHFB_NAKTIMEOUT         3               /* Allow the request to time-out after the given timeout value                                      */
#define UHFB_ALLOWRUNTPKTS      4               /* Receiving less data than expected will not cause an UHIOERR_RUNTPACKET                           */
#define UHFB_SPLITTRANS         5               /* new for V2.0: Split transaction for Lowspeed/Fullspeed devices at USB2.0 hubs                    */
#define UHFB_MULTI_1            6               /* new for V2.1: Number of transactions per microframe bit 0                                        */
#define UHFB_MULTI_2            7               /* new for V2.1: Number of transactions per microframe bit 1                                        */
#define UHFS_THINKTIME          8               /* new for V2.2: Bit times required at most for intertransaction gap on LS/FS                       */

#define UHTT_8                  (0)
#define UHTT_16                 (1)
#define UHTT_24                 (2)
#define UHTT_32                 (3)

#define UHFF_LOWSPEED           (1 << UHFB_LOWSPEED)
#define UHFF_HIGHSPEED          (1 << UHFB_HIGHSPEED)
#define UHFF_NOSHORTPKT         (1 << UHFB_NOSHORTPKT)
#define UHFF_NAKTIMEOUT         (1 << UHFB_NAKTIMEOUT)
#define UHFF_ALLOWRUNTPKTS      (1 << UHFB_ALLOWRUNTPKTS)
#define UHFF_SPLITTRANS         (1 << UHFB_SPLITTRANS)
#define UHFF_MULTI_1            (1 << UHFB_MULTI_1)
#define UHFF_MULTI_2            (1 << UHFB_MULTI_2)
#define UHFF_MULTI_3            (UHFF_MULTI_1|UHFF_MULTI_2)
#define UHFF_THINKTIME_8        (UHTT_8  << UHFS_THINKTIME)
#define UHFF_THINKTIME_16       (UHTT_16 << UHFS_THINKTIME)
#define UHFF_THINKTIME_24       (UHTT_24 << UHFS_THINKTIME)
#define UHFF_THINKTIME_32       (UHTT_32 << UHFS_THINKTIME)

/* Definitions for the legacy iouh_State field - also the values returned by the
 * legacy-only UHA_State query (whose tag ID now lives in usbhcd_common.h) */

#define UHSB_OPERATIONAL        0               /* USB can be used for transfers                                                                    */
#define UHSB_RESUMING           1               /* USB is currently resuming                                                                        */
#define UHSB_SUSPENDED          2               /* USB is in suspended state                                                                        */
#define UHSB_RESET              3               /* USB is just inside a reset phase                                                                 */

#define UHSF_OPERATIONAL        (1 << UHSB_OPERATIONAL)
#define UHSF_RESUMING           (1 << UHSB_RESUMING)
#define UHSF_SUSPENDED          (1 << UHSB_SUSPENDED)
#define UHSF_RESET              (1 << UHSB_RESET)

#endif	/* DEVICES_USBHARDWARE_H */
