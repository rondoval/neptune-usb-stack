#ifndef DEVICES_USBVIDEODEVICE_H
#define DEVICES_USBVIDEODEVICE_H
/*
**	usbvideodevice.h - usbvideo.device, the application interface of usbvideo.usbclass
**
**	One unit is one capture stream of one camera. Units are numbered from 0
**	and opened exclusively. A camera that is unplugged and comes back gets
**	its unit number back, unless another camera was given the number while
**	it was away - a unit that is held open is never given away, so a
**	program that keeps its unit open while its camera is away finds it
**	there again. The class moves frames and does not change them:
**	what CMD_READ returns is the camera's sample, byte for byte - for MJPEG
**	one JPEG picture, which may lack Huffman tables (the standard ones then
**	apply); for an uncompressed format the picture's pixels in the layout
**	its FourCC stands for; for H.264 one picture's worth of the stream, in
**	the byte stream form of the standard's Annex B.
**
**	The loop an application writes:
**	    OpenDevice("usbvideo.device", unit, ioreq, 0)
**	    NSCMD_UV_SETMODE               - choose a mode, learn the buffer size
**	    SendIO() a few CMD_READs       - each with a buffer of that size
**	    CMD_START
**	    for every reply: use the frame, SendIO() the request again
**	    CMD_STOP, CloseDevice()
**	Reading slower than the camera produces just skips frames.
**
**	The structures filled by a command may gain fields at their end in a
**	later version: pass the size of the structure you compiled with, and
**	trust io_Actual for how much of it was filled.
*/

#include <exec/types.h>
#include <exec/io.h>

#define USBVIDEO_NAME "usbvideo.device"

/* Flags for OpenDevice() */
#define UVOF_DAMAGED     0x00000001 /* deliver damaged frames too, flagged IVRF_DAMAGED
                                       (default: they are dropped). Only a request as
                                       long as struct IOVideoReq can see that flag. */

/*
** Commands. usbvideo.device is a new-style device: NSCMD_DEVICEQUERY lists
** what it understands. Its own commands are a block in the third-party
** command area; of the standard ones it knows CMD_START, CMD_STOP, CMD_READ,
** CMD_FLUSH (aborts every queued read), and AbortIO() works on a read and on
** a waiting NSCMD_UV_WAITEVENT.
*/
#define NSCMD_USBVIDEO_BASE  0x8A00
#define NSCMD_UV_GETINFO     (NSCMD_USBVIDEO_BASE + 0x00) /* io_Data: struct UsbVideoInfo, io_Length: its size */
#define NSCMD_UV_GETMODES    (NSCMD_USBVIDEO_BASE + 0x01) /* io_Data: buffer for the mode table, io_Length: its size;
                                                             io_Actual: bytes written - the first records that fit */
#define NSCMD_UV_SETMODE     (NSCMD_USBVIDEO_BASE + 0x02) /* io_Data: struct UsbVideoModeReq, read and written */
#define NSCMD_UV_GETSTATS    (NSCMD_USBVIDEO_BASE + 0x03) /* io_Data: struct UsbVideoStats, io_Length: its size */
#define NSCMD_UV_REQUEST     (NSCMD_USBVIDEO_BASE + 0x04) /* io_Data: struct UsbVideoRequest, read and written */
#define NSCMD_UV_GETTOPOLOGY (NSCMD_USBVIDEO_BASE + 0x05) /* io_Data: buffer for the terminals and units, io_Length:
                                                             its size; io_Actual: bytes written - the first
                                                             records that fit */
#define NSCMD_UV_WAITEVENT   (NSCMD_USBVIDEO_BASE + 0x06) /* io_Data: buffer for one status packet, io_Length: its
                                                             size; completes when the camera sends one */
#define NSCMD_UV_GETCONTROL  (NSCMD_USBVIDEO_BASE + 0x07) /* io_Data: struct UsbVideoControl, read and written */
#define NSCMD_UV_NEXTCONTROL (NSCMD_USBVIDEO_BASE + 0x08) /* the same: the camera's next control after this one */
#define NSCMD_UV_SETCONTROL  (NSCMD_USBVIDEO_BASE + 0x09) /* the same: uvc_Value goes to the camera */
/* the block is NSCMD_USBVIDEO_BASE + 0x00..0x1f */

/*
** CMD_READ: io_Data / io_Length is the buffer for one frame; on return
** io_Actual is the frame's length - of an uncompressed format always
** uvq_MaxFrameSize, the size of the picture: a frame of any other length
** counts as damaged. A frame goes to a read that was waiting
** when the frame began. A read may be queued before CMD_START and stays
** queued across CMD_STOP; it ends early with IOERR_ABORTED on CMD_FLUSH,
** AbortIO() and CloseDevice(), and with UVERR_GONE when the camera is
** unplugged.
**
** A request as long as struct IOVideoReq (mn_Length says so) also gets the
** fields below; a plain struct IOStdReq works for the simplest client.
*/
struct IOVideoReq
{
    struct IOStdReq ivr_Std;
    ULONG           ivr_Sequence;   /* counts the frames the camera started since CMD_START,
                                       the first is 1: a gap is a frame that was dropped or
                                       that no read was waiting for */
    ULONG           ivr_Flags;      /* IVRF_ */
    ULONG           ivr_PTS;        /* the camera's presentation time stamp, in ticks of
                                       uvi_ClockFrequency; valid if IVRF_PTS */
    UWORD           ivr_BusFrame;   /* USB frame number in which the frame began, as the
                                       host counts them; 0 on a bulk stream (UVIF_BULK),
                                       whose transfers are not tied to frames */
    UWORD           ivr_SCRFrame;   /* the camera's own count of USB frames, 11 bits, and... */
    ULONG           ivr_SCRClock;   /* ...what its clock read at that moment, in the ticks of
                                       ivr_PTS; both valid if IVRF_SCR */
};

#define IVRF_DAMAGED     0x00000001 /* lost or flagged data inside this frame (UVOF_DAMAGED only) */
#define IVRF_PTS         0x00000002 /* ivr_PTS is valid */
#define IVRF_SCR         0x00000004 /* ivr_SCRFrame and ivr_SCRClock are valid */

/*
** Time. ivr_PTS says when the camera took the picture, by its own clock.
** The pair ivr_SCRClock / ivr_SCRFrame, taken when the frame's first bytes
** went onto the bus, ties that clock to the USB frames, which tick once a
** millisecond for the host as well: two pairs some time apart give the true
** rate of the camera's clock, and with it the place of every ivr_PTS in bus
** time. That is what lining pictures up with something else on the bus -
** the camera's own microphone - needs. The class passes the numbers on and
** computes nothing.
*/

/* io_Error, beyond the IOERR_ values of exec/errors.h */
#define UVERR_NOMODE      1 /* CMD_START before a successful NSCMD_UV_SETMODE */
#define UVERR_BADMODE     2 /* NSCMD_UV_SETMODE: the camera has no such mode */
#define UVERR_REFUSED     3 /* the camera refused: the negotiation (NSCMD_UV_SETMODE), a
                               value (NSCMD_UV_SETCONTROL - uvc_Reason has its reason), a
                               request (NSCMD_UV_REQUEST - uvr_ErrorCode has it) */
#define UVERR_NOBANDWIDTH 4 /* the mode does not fit the bus */
#define UVERR_BUSY        5 /* NSCMD_UV_SETMODE while streaming, CMD_START twice */
#define UVERR_OVERFLOW    6 /* CMD_READ: the frame was longer than the buffer; io_Actual
                               bytes of it are there */
#define UVERR_GONE        7 /* the camera was unplugged */
#define UVERR_USB         8 /* a USB transfer failed */
#define UVERR_NOSTATUS    9 /* NSCMD_UV_WAITEVENT: the camera has no status endpoint */
#define UVERR_NOCONTROL  10 /* the camera has no such control; NSCMD_UV_NEXTCONTROL: no
                               further one */

/* NSCMD_UV_GETINFO */
struct UsbVideoInfo
{
    UWORD uvi_VendorID;
    UWORD uvi_ProductID;
    UWORD uvi_Version;        /* UVC revision of the camera, BCD: 0x0100, 0x0110, 0x0150 */
    UWORD uvi_Flags;          /* UVIF_ */
    ULONG uvi_NumModes;       /* records NSCMD_UV_GETMODES returns... */
    ULONG uvi_ModeTableSize;  /* ...and the buffer they need, in bytes */
    /* the mode in force: all zero until NSCMD_UV_SETMODE succeeded */
    ULONG uvi_FourCC;
    UWORD uvi_Width;
    UWORD uvi_Height;
    ULONG uvi_Interval;       /* frame interval, 100 ns units */
    ULONG uvi_MaxFrameSize;   /* no frame of this mode is longer: the size for read buffers */
    ULONG uvi_ClockFrequency; /* ticks per second of the clock ivr_PTS counts in;
                                 0 = the camera does not say */
    UBYTE uvi_ProductName[80];
    UBYTE uvi_SerialNumber[44]; /* tells two cameras of one kind apart; "n/a" if it has none */
    /* a request shorter than this gets the fields above only */
    ULONG uvi_NumEntities;    /* records NSCMD_UV_GETTOPOLOGY returns... */
    ULONG uvi_TopologySize;   /* ...and the buffer they need, in bytes */
    /* and one shorter than this the fields above */
    UBYTE uvi_Manufacturer[48]; /* who made the camera: the name the camera gives, else
                                   the one the USB stack has for its vendor ID; "n/a" if
                                   neither */
};

#define UVIF_STREAMING   0x0001 /* between CMD_START and CMD_STOP */
#define UVIF_BULK        0x0002 /* bulk transport (else isochronous): the stream takes no
                                   bus bandwidth and none is kept for it */
#define UVIF_EVENTS      0x0004 /* the camera has a status endpoint: NSCMD_UV_WAITEVENT works */

/*
** NSCMD_UV_GETMODES: the table is a row of records of varying length, one per
** format and picture size; uvm_Size leads from one to the next. Only modes
** the class can stream are listed, so a later version may list more: skip
** the FourCCs you do not know.
*/
struct UsbVideoMode
{
    UWORD uvm_Size;           /* bytes from this record to the next */
    UWORD uvm_Flags;          /* UVMF_ */
    ULONG uvm_FourCC;         /* UVFCC_ */
    UWORD uvm_Width;
    UWORD uvm_Height;
    ULONG uvm_DefInterval;    /* the camera's default frame interval, 100 ns units */
    ULONG uvm_NumIntervals;   /* entries in uvm_Intervals */
    ULONG uvm_Intervals[1];   /* supported intervals, shortest (fastest) first; with
                                 UVMF_CONTINUOUS exactly three: shortest, longest, step */
};

#define UVMF_DEFAULT     0x0001 /* the default mode, the one a request without a size gets:
                                   the user's choice in the camera's settings window if
                                   there is one, else what the camera suggests */
#define UVMF_CONTINUOUS  0x0002 /* uvm_Intervals is a range, not a list */

/* A FourCC is its four characters in reading order. An uncompressed format
   has the one its camera names it by; these two are the usual ones. */
#define UVFCC_MJPG       0x4D4A5047 /* 'MJPG': one JPEG picture per frame */
#define UVFCC_YUY2       0x59555932 /* 'YUY2': packed 4:2:2, two bytes a pixel: Y0 U Y1 V */
#define UVFCC_NV12       0x4E563132 /* 'NV12': 4:2:0, the Y plane, then U and V interleaved */
#define UVFCC_H264       0x48323634 /* 'H264': one picture of an H.264 stream per frame */
#define UVFCC_VP80       0x56503830 /* 'VP80': one VP8 frame per frame */

/*
** H.264 and VP8 are streams: a frame builds on the ones before it. Keep
** reads waiting - a frame nobody takes is a hole - and watch ivr_Sequence:
** after a gap a decoder shows rubbish until the next key frame comes.
**
** A camera from before the H.264 format may have an H.264 encoder all the
** same, set up through an extension unit (<devices/usb_video.h>). Its
** H.264 is listed as H264 modes like any other. NSCMD_UV_SETMODE takes the
** bit rate and the time between key frames for such a mode (uvq_BitRate,
** uvq_KeyInterval); what a request leaves open, and all else about the
** encoder, is as that unit has it. The unit's other controls, which are
** for use while the stream runs, are reached with NSCMD_UV_REQUEST.
*/

/*
** NSCMD_UV_SETMODE: fill in what matters and leave the rest zero - no FourCC
** means MJPEG if the camera has it, else its first format; no size (width
** and height both zero) the default size; no interval the default interval
** of that size (the defaults are the mode table's: UVMF_DEFAULT,
** uvm_DefInterval); an interval the mode does not list is replaced by the
** nearest it has. On
** success the structure holds the mode the camera settled on - look at it,
** the interval in particular may differ from the one asked for - and the
** mode is in force until the next NSCMD_UV_SETMODE or CloseDevice(). A
** failed request changes nothing. Not while streaming.
**
** The last two fields are for a mode with an encoder behind it that takes
** such wishes: today the H264 modes of a camera with the H.264 unit above.
** For any other mode they are not looked at and come back as 0, which is
** how a program sees that its wish was not for this mode.
*/
struct UsbVideoModeReq
{
    ULONG uvq_FourCC;
    UWORD uvq_Width;
    UWORD uvq_Height;
    ULONG uvq_Interval;       /* 100 ns units: 333333 = 30 frames a second */
    ULONG uvq_MaxFrameSize;   /* written only: the size for read buffers */
    /* a request shorter than this ends above and asks for none of the following */
    ULONG uvq_BitRate;        /* bits a second, on average; 0 = as the camera has it.
                                 Written: what the camera settled on */
    ULONG uvq_KeyInterval;    /* milliseconds from one key frame to the next; 0 = as the
                                 camera has it. Written: what the camera settled on */
};

/*
** NSCMD_UV_GETSTATS: what became of the stream since the last CMD_START.
** Every frame the camera started ends up in one of the four counts after
** uvs_Frames (the frame in progress is in none yet).
*/
struct UsbVideoStats
{
    ULONG uvs_Frames;         /* frames the camera started */
    ULONG uvs_Delivered;      /* handed to a read, whole */
    ULONG uvs_Skipped;        /* no read was waiting when the frame began */
    ULONG uvs_Damaged;        /* damaged: dropped, or delivered flagged (UVOF_DAMAGED) */
    ULONG uvs_Overflowed;     /* longer than the read's buffer (UVERR_OVERFLOW) */
    /* why frames were damaged */
    ULONG uvs_LostIntervals;  /* transfers the USB controller reported as failed or missed */
    ULONG uvs_BadPayloads;    /* transfers that did not begin with a valid payload header */
    ULONG uvs_CameraErrors;   /* transfers the camera itself flagged as faulty */
};

/*
** Camera controls: brightness, exposure, focus and so on.
**
** Controls belong to the camera, not to a video stream. You can read and set
** them at any time while the device is open, whether the camera is streaming
** or not. If a camera has several streams, each of its device units reaches
** the same controls.
**
** There are two sets of commands for controls:
**
** - The control commands described here. They cover the standard controls
**   of the USB video class, listed below as UVCID_. Values are ordinary
**   numbers. Use these commands whenever they cover what you need.
**
** - The raw commands NSCMD_UV_GETTOPOLOGY and NSCMD_UV_REQUEST, described
**   further down. They pass any class request to the camera unchanged, so
**   they also reach a manufacturer's own controls and the standard controls
**   that have no UVCID_. You have to know the selectors and data layouts
**   yourself (<devices/usb_video.h>), and the data is little-endian.
**
** The control commands are NSCMD_UV_GETCONTROL, NSCMD_UV_NEXTCONTROL and
** NSCMD_UV_SETCONTROL. All three take the same structure:
**     io_Data   = pointer to a struct UsbVideoControl
**     io_Length = sizeof(struct UsbVideoControl)
** uvc_ID says which control you mean. Leave uvc_Entity 0 unless you have a
** reason not to; the reason is explained at the field.
**
** NSCMD_UV_GETCONTROL - read one control
**     You set:  uvc_ID, uvc_Entity
**     You get:  every other field: value, range, default, flags, names
**     Errors:   UVERR_NOCONTROL - the camera has no such control
**     To fill in the structure the device sends the camera up to six
**     requests, so do not call this more often than you need to.
**
** NSCMD_UV_NEXTCONTROL - list the controls the camera has
**     Works like NSCMD_UV_GETCONTROL, but returns the control that follows
**     the one in the structure. Start with a structure that is all zero and
**     repeat the command with the same structure until it fails:
**
**         struct UsbVideoControl ctl = { 0 };
**
**         ioreq->io_Command = NSCMD_UV_NEXTCONTROL;
**         ioreq->io_Data    = &ctl;
**         ioreq->io_Length  = sizeof(ctl);
**         while(DoIO((struct IORequest *) ioreq) == 0)
**         {
**             Printf("%s\n", ctl.uvc_Name);
**         }
**
**     After the last control the error is UVERR_NOCONTROL. A control that
**     switches an automatic mode on and off is returned before the controls
**     that mode takes over, which is the order a settings window wants.
**
** NSCMD_UV_SETCONTROL - set one control
**     You set:  uvc_ID, uvc_Entity, uvc_Value
**     You get:  uvc_Reason; no other field is changed
**     Errors:   UVERR_NOCONTROL - the camera has no such control
**               UVERR_REFUSED   - the camera rejected the value; uvc_Reason
**                                 says why
**     The value is sent to the camera exactly as given: the device does not
**     limit it to uvc_Min..uvc_Max or round it to uvc_Step. A camera
**     normally rejects a value it cannot use, and any value while the
**     control is UVCF_INACTIVE. To see what the camera made of a value, read
**     the control again with NSCMD_UV_GETCONTROL.
**
** Like every command that talks to the camera, these can also fail with
** UVERR_GONE (unplugged) or UVERR_USB (a transfer failed).
*/
struct UsbVideoControl
{
    UWORD uvc_ID;             /* in:  which control, UVCID_ */
    UBYTE uvc_Entity;         /* in:  0 = wherever the camera has this control. A camera is
                                      made of terminals and units, each with an ID, and a
                                      control sits in one of them. Give an ID only to pick
                                      one of several that have the same control.
                                 out: the ID of the terminal or unit that has it. Set it
                                      back to 0 before you reuse the structure for another
                                      uvc_ID */
    UBYTE uvc_Selector;       /* out: the control's number inside that terminal or unit.
                                      uvc_Entity and uvc_Selector together are how an event
                                      (NSCMD_UV_WAITEVENT) and NSCMD_UV_REQUEST name this
                                      control */
    UBYTE uvc_Kind;           /* out: what kind of value it has, UVCK_ */
    UBYTE uvc_Reason;         /* out: the camera's error code, UVREC_ in
                                      <devices/usb_video.h>. Set when the camera rejected a
                                      value (UVERR_REFUSED). Also set by a read if the
                                      camera did not answer for a control it claims to
                                      have: UVCF_GET is then clear and this is not
                                      UVREC_NO_ERROR */
    UWORD uvc_Flags;          /* out: what can be done with the control, UVCF_ */
    LONG  uvc_Value;          /* out: the current value; valid only if UVCF_GET is set
                                 in:  for NSCMD_UV_SETCONTROL, the new value */
    LONG  uvc_Min;            /* out: the smallest value, */
    LONG  uvc_Max;            /*      the largest value, */
    LONG  uvc_Step;           /*      and the distance from one valid value to the next;
                                      all three are valid only if UVCF_RANGE is set */
    LONG  uvc_Default;        /* out: the camera's default value; valid only if
                                      UVCF_DEFAULT is set */
    ULONG uvc_Choices;        /* out: UVCK_CHOICE only: bit n is set if the camera offers
                                      choice n */
    CONST_STRPTR uvc_Name;    /* out: the control's name in English, for display */
    const CONST_STRPTR *uvc_ChoiceNames; /* out: UVCK_CHOICE only: the names of the
                                                 choices, uvc_ChoiceNames[n] for choice n,
                                                 with NULL after the last */
    /* The names are strings inside the device: do not change or free them. They stay
       valid until CloseDevice(). */
};

#define UVCK_RANGE       0 /* a number */
#define UVCK_SWITCH      1 /* 0 = off, 1 = on */
#define UVCK_CHOICE      2 /* the number of one of its choices, counted from 0 */

/* the first five are what the camera says of the control right now */
#define UVCF_GET         0x0001 /* can be read */
#define UVCF_SET         0x0002 /* can be set... */
#define UVCF_INACTIVE    0x0004 /* ...but not now: an automatic mode governs it, or the
                                   mode in force rules it out */
#define UVCF_VOLATILE    0x0008 /* changes by itself; the camera reports that as an event */
#define UVCF_SLOW        0x0010 /* a write is taken at once and its outcome comes later, as
                                   an event; until then the control refuses with
                                   UVREC_NOT_READY */
#define UVCF_RANGE       0x0100 /* uvc_Min, uvc_Max and uvc_Step are valid: always for a
                                   switch and a choice; for a number if the camera names
                                   its range */
#define UVCF_DEFAULT     0x0200 /* uvc_Default is valid */
#define UVCF_GOVERNS     0x0400 /* an automatic mode: whether other controls can be set
                                   depends on it. Write it before them, and ask about them
                                   again when it was written */

/*
** The controls. A value has the unit the USB video class gives it; where
** that is not a plain number of the camera's own, it is noted. The high
** byte of an ID is its group: a settings window makes a page of each.
*/
#define UVCID_GROUP(id)  ((id) >> 8)
#define UVCG_PICTURE     0x01
#define UVCG_CAMERA      0x02

/* the picture */
#define UVCID_BRIGHTNESS          0x0101
#define UVCID_CONTRAST_AUTO       0x0102 /* switch */
#define UVCID_CONTRAST            0x0103
#define UVCID_SATURATION          0x0104
#define UVCID_HUE_AUTO            0x0105 /* switch */
#define UVCID_HUE                 0x0106
#define UVCID_SHARPNESS           0x0107
#define UVCID_GAMMA               0x0108
#define UVCID_GAIN                0x0109
#define UVCID_BACKLIGHT           0x010a /* backlight compensation */
#define UVCID_WB_AUTO             0x010b /* switch: white balance by temperature, automatic */
#define UVCID_WB                  0x010c /* white balance: colour temperature, kelvin */
#define UVCID_WB_COMPONENT_AUTO   0x010d /* switch: white balance by components, automatic */
#define UVCID_WB_BLUE             0x010e /* white balance: the blue component... */
#define UVCID_WB_RED              0x010f /* ...and the red one */
#define UVCID_POWER_LINE          0x0110 /* choice, UVPLF_: the mains frequency a lamp flickers at */
#define UVCID_DIGITAL_ZOOM        0x0111
#define UVCID_DIGITAL_ZOOM_LIMIT  0x0112

/* the camera */
#define UVCID_EXPOSURE_MODE       0x0201 /* choice, UVEXP_ */
#define UVCID_EXPOSURE_PRIORITY   0x0202 /* switch: the exposure may lower the frame rate */
#define UVCID_EXPOSURE_TIME       0x0203 /* 0.1 ms */
#define UVCID_IRIS                0x0204 /* f-number x 100 */
#define UVCID_FOCUS_AUTO          0x0205 /* switch */
#define UVCID_FOCUS               0x0206
#define UVCID_FOCUS_RANGE         0x0207 /* choice, UVFOC_ */
#define UVCID_ZOOM                0x0208
#define UVCID_PAN                 0x0209 /* arc seconds */
#define UVCID_TILT                0x020a /* arc seconds */
#define UVCID_ROLL                0x020b /* degrees */
#define UVCID_PRIVACY             0x020c /* switch: the privacy shutter */
#define UVCID_SCANNING_MODE       0x020d /* switch: progressive scan */

/* the choices of UVCID_EXPOSURE_MODE... */
#define UVEXP_MANUAL     0
#define UVEXP_AUTO       1
#define UVEXP_SHUTTER    2 /* shutter priority */
#define UVEXP_APERTURE   3 /* aperture priority */
/* ...of UVCID_POWER_LINE... */
#define UVPLF_OFF        0
#define UVPLF_50HZ       1
#define UVPLF_60HZ       2
#define UVPLF_AUTO       3
/* ...and of UVCID_FOCUS_RANGE */
#define UVFOC_FULL       0
#define UVFOC_MACRO      1
#define UVFOC_PEOPLE     2
#define UVFOC_SCENE      3

/*
** NSCMD_UV_REQUEST: one class request to the camera's control interface or
** to one of its terminals and units. io_Data is this structure, io_Length
** its size.
*/
struct UsbVideoRequest
{
    UBYTE uvr_Request;        /* UVUDR_: SET_CUR, GET_CUR, GET_MIN, GET_INFO ... */
    UBYTE uvr_Entity;         /* ID of the terminal or unit; 0 = the control interface itself */
    UBYTE uvr_Selector;       /* which of its controls: UVCTCS_, UVPUCS_ ... */
    UBYTE uvr_ErrorCode;      /* written with UVERR_REFUSED: the camera's reason, UVREC_ */
    UWORD uvr_Length;         /* bytes at uvr_Data */
    UWORD uvr_Actual;         /* written: bytes transferred - a camera may answer a GET
                                 request with fewer than asked for, the rest is zero then */
    APTR  uvr_Data;
};

/*
** NSCMD_UV_GETTOPOLOGY: the camera's terminals and units, one record each in
** the order of its descriptors; uve_Size leads from one to the next. A
** terminal or unit takes its input from its sources, so the records are the
** camera's block diagram.
*/
struct UsbVideoEntity
{
    UWORD uve_Size;           /* bytes from this record to the next */
    UBYTE uve_ID;             /* what uvr_Entity names */
    UBYTE uve_Type;           /* UDST_VIDEO_CTRL_: input or output terminal, selector,
                                 processing, extension or encoding unit */
    UWORD uve_TermType;       /* terminals: wTerminalType (UVITT_CAMERA is a camera) */
    UBYTE uve_NumSources;     /* IDs it takes its input from... */
    UBYTE uve_ControlSize;    /* ...and bytes of control bitmap, both in uve_Data */
    UBYTE uve_GUID[16];       /* extension units: guidExtensionCode in wire order, else zero */
    UBYTE uve_Data[2];        /* the source IDs, then the control bitmap as the camera
                                 declares it (bits UVCTB_, UVPUB_ ...) */
};

/*
** NSCMD_UV_WAITEVENT: waits until the camera reports something by itself - a
** control changed its value or its state, the button on the camera was
** pressed - and returns that status packet as the camera sent it (UVSP_ in
** <devices/usb_video.h>), io_Actual its length; a buffer of 32 bytes takes
** any. Keep a few waiting, like reads: each event completes one of them, and
** an event nobody waits for is lost. A waiting request ends early with
** IOERR_ABORTED on AbortIO() and CloseDevice() and with UVERR_GONE when the
** camera is unplugged; a camera without status endpoint (UVIF_EVENTS not
** set) answers UVERR_NOSTATUS at once.
*/

#endif /* DEVICES_USBVIDEODEVICE_H */
