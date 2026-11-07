#ifndef DEVICES_USB_VIDEO_H
#define DEVICES_USB_VIDEO_H
/*
**	$VER: usb_video.h 3.0 (05.10.2026)
**
**	usb definitions include file - USB Video Class 1.0 / 1.1 / 1.5
**
**	(C) Copyright 2008 Chris Hodges
**	    All Rights Reserved
**
**	3.0: extended to UVC 1.5. The wire layouts are packed structures
**	     with the field names of the specification, laid out anew from
**	     it (several of the structures of 2.0 were wrong). Every field of
**	     more than one byte is little-endian on the wire, and few are
**	     aligned. A structure names the fixed part of a layout; what
**	     follows an array of variable length is said in a comment.
**	     Last in the file: the H.264 extension unit of UVC 1.1 cameras.
*/

#include <exec/types.h>
#include <stddef.h>

#if defined(__GNUC__)
# pragma pack(1)
#endif

/* Usb Video Requests */
#define UVUDR_SET_CUR             0x01
#define UVUDR_GET_CUR             0x81
#define UVUDR_SET_MIN             0x02
#define UVUDR_GET_MIN             0x82
#define UVUDR_SET_MAX             0x03
#define UVUDR_GET_MAX             0x83
#define UVUDR_SET_RES             0x04
#define UVUDR_GET_RES             0x84
#define UVUDR_GET_LEN             0x85
#define UVUDR_GET_INFO            0x86
#define UVUDR_GET_DEF             0x87
#define UVUDR_SET_CUR_ALL         0x11 /* 1.5: all controls of an entity at once */
#define UVUDR_GET_CUR_ALL         0x91
#define UVUDR_GET_MIN_ALL         0x92
#define UVUDR_GET_MAX_ALL         0x93
#define UVUDR_GET_RES_ALL         0x94
#define UVUDR_GET_DEF_ALL         0x97

/* GET_INFO capability / state bits */
#define UVGIF_GET                 0x01 /* GET requests supported */
#define UVGIF_SET                 0x02 /* SET requests supported */
#define UVGIF_DISABLED_AUTO       0x04 /* Disabled by an automatic mode (state) */
#define UVGIF_AUTOUPDATE          0x08 /* Changes on its own, announced by status interrupt */
#define UVGIF_ASYNCHRONOUS        0x10 /* SET_CUR outcome arrives by status interrupt */
#define UVGIF_DISABLED_COMMIT     0x20 /* 1.5: disabled by the current Commit state */

/* Request Error Code Control values (the reason for a protocol STALL) */
#define UVREC_NO_ERROR            0x00
#define UVREC_NOT_READY           0x01
#define UVREC_WRONG_STATE         0x02
#define UVREC_POWER               0x03
#define UVREC_OUT_OF_RANGE        0x04
#define UVREC_INVALID_UNIT        0x05
#define UVREC_INVALID_CONTROL     0x06
#define UVREC_INVALID_REQUEST     0x07
#define UVREC_INVALID_VALUE       0x08 /* Invalid value within range */
#define UVREC_UNKNOWN             0xff

/* Video Ctrl class specific interface descriptor subtypes */
#define UDST_VIDEO_CTRL_HEADER          0x01
#define UDST_VIDEO_CTRL_INPUT_TERMINAL  0x02
#define UDST_VIDEO_CTRL_OUTPUT_TERMINAL 0x03
#define UDST_VIDEO_CTRL_SELECTOR_UNIT   0x04
#define UDST_VIDEO_CTRL_PROCESSING_UNIT 0x05
#define UDST_VIDEO_CTRL_EXTENSION_UNIT  0x06
#define UDST_VIDEO_CTRL_ENCODING_UNIT   0x07 /* 1.5 */

/* Video Streaming class specific interface descriptor subtypes */
#define UDST_VIDEO_STREAM_INPUT_HEADER        0x01
#define UDST_VIDEO_STREAM_OUTPUT_HEADER       0x02
#define UDST_VIDEO_STREAM_STILL_IMAGE_FRAME   0x03
#define UDST_VIDEO_STREAM_FORMAT_UNCOMPRESSED 0x04
#define UDST_VIDEO_STREAM_FRAME_UNCOMPRESSED  0x05
#define UDST_VIDEO_STREAM_FORMAT_MJPEG        0x06
#define UDST_VIDEO_STREAM_FRAME_MJPEG         0x07
#define UDST_VIDEO_STREAM_FORMAT_MPEG2TS      0x0a
#define UDST_VIDEO_STREAM_FORMAT_DV           0x0c
#define UDST_VIDEO_STREAM_COLORFORMAT         0x0d
#define UDST_VIDEO_STREAM_FORMAT_FRAME_BASED  0x10
#define UDST_VIDEO_STREAM_FRAME_FRAME_BASED   0x11
#define UDST_VIDEO_STREAM_FORMAT_STREAM_BASED 0x12
#define UDST_VIDEO_STREAM_FORMAT_H264           0x13 /* 1.5 */
#define UDST_VIDEO_STREAM_FRAME_H264            0x14
#define UDST_VIDEO_STREAM_FORMAT_H264_SIMULCAST 0x15
#define UDST_VIDEO_STREAM_FORMAT_VP8            0x16
#define UDST_VIDEO_STREAM_FRAME_VP8             0x17
#define UDST_VIDEO_STREAM_FORMAT_VP8_SIMULCAST  0x18

/* Videoclass specific endpoint descriptors subtypes */
#define UDST_VIDEO_EP_GENERAL    0x01
#define UDST_VIDEO_EP_ENDPOINT   0x02
#define UDST_VIDEO_EP_INTERRUPT  0x03

/* Video classes */
#define VIDEO_NO_SUBCLASS     0x00
#define VIDEO_CTRL_SUBCLASS   0x01
#define VIDEO_STREAM_SUBCLASS 0x02
#define VIDEO_IFCOLL_SUBCLASS 0x03

/* Video interface protocols */
#define VIDEO_PROTOCOL_UNDEFINED 0x00 /* UVC 1.0 / 1.1 */
#define VIDEO_PROTOCOL_15        0x01 /* UVC 1.5 */

/* bcdUVC of the Video Control header - the authoritative revision */
#define UVC_VERSION_10 0x0100
#define UVC_VERSION_11 0x0110
#define UVC_VERSION_15 0x0150

/* USB Video specific stuff */

/* USB Video USB Terminal types */
#define UVUTT_VENDOR           0x0100 /* USB vendor specific */
#define UVUTT_STREAMING        0x0101 /* USB streaming */

/* USB Video Input Terminal types */
#define UVITT_VENDOR           0x0200 /* Input Vendor specific */
#define UVITT_CAMERA           0x0201 /* Camera sensor */
#define UVITT_MEDIA_TRANSPORT  0x0202 /* Sequential media */

/* USB Video Output Terminal types */
#define UVOTT_VENDOR           0x0300 /* Output Vendor specific */
#define UVOTT_DISPLAY          0x0301 /* Generic display */
#define UVOTT_MEDIA_TRANSPORT  0x0302 /* Sequential media */

/* USB Video External Terminal types */
#define UVETT_VENDOR           0x0400 /* External Vendor specific */
#define UVETT_COMPOSITE_CONNECTOR 0x0401 /* Composite video connector */
#define UVETT_SVIDEO_CONNECTOR 0x0402 /* S-video connector */
#define UVETT_COMPONENT_CONNECTOR 0x0403 /* Component video connector */

/* Controls. A control has two numbers: its selector names it in a request
   (the high byte of wValue), and its bit in the bitmap of its unit's,
   terminal's or format's descriptor says the device has it. The two
   orders differ, so each has its table; an Encoding Unit's bit is its
   selector - 1. */

/* VideoControl Interface Control Selectors */
#define UVVCCS_VIDEO_POWER_MODE_CONTROL   0x01
#define UVVCCS_REQUEST_ERROR_CODE_CONTROL 0x02

/* Selector Unit Control Selectors */
#define UVSUCS_INPUT_SELECT_CONTROL       0x01

/* Camera Terminal Control Selectors */
#define UVCTCS_SCANNING_MODE_CONTROL      0x01
#define UVCTCS_AE_MODE_CONTROL            0x02
#define UVCTCS_AE_PRIORITY_CONTROL        0x03
#define UVCTCS_EXPOSURE_TIME_ABS_CONTROL  0x04
#define UVCTCS_EXPOSURE_TIME_REL_CONTROL  0x05
#define UVCTCS_FOCUS_ABS_CONTROL          0x06
#define UVCTCS_FOCUS_REL_CONTROL          0x07
#define UVCTCS_FOCUS_AUTO_CONTROL         0x08
#define UVCTCS_IRIS_ABS_CONTROL           0x09
#define UVCTCS_IRIS_REL_CONTROL           0x0a
#define UVCTCS_ZOOM_ABS_CONTROL           0x0b
#define UVCTCS_ZOOM_REL_CONTROL           0x0c
#define UVCTCS_PANTILT_ABS_CONTROL        0x0d
#define UVCTCS_PANTILT_REL_CONTROL        0x0e
#define UVCTCS_ROLL_ABS_CONTROL           0x0f
#define UVCTCS_ROLL_REL_CONTROL           0x10
#define UVCTCS_PRIVACY_CONTROL            0x11
#define UVCTCS_FOCUS_SIMPLE_CONTROL       0x12 /* 1.5 */
#define UVCTCS_WINDOW_CONTROL             0x13 /* 1.5 */
#define UVCTCS_REGION_OF_INTEREST_CONTROL 0x14 /* 1.5 */

/* Camera Terminal control bitmap, bit numbers */
#define UVCTB_SCANNING_MODE       0
#define UVCTB_AE_MODE             1
#define UVCTB_AE_PRIORITY         2
#define UVCTB_EXPOSURE_TIME_ABS   3
#define UVCTB_EXPOSURE_TIME_REL   4
#define UVCTB_FOCUS_ABS           5
#define UVCTB_FOCUS_REL           6
#define UVCTB_IRIS_ABS            7
#define UVCTB_IRIS_REL            8
#define UVCTB_ZOOM_ABS            9
#define UVCTB_ZOOM_REL            10
#define UVCTB_PANTILT_ABS         11
#define UVCTB_PANTILT_REL         12
#define UVCTB_ROLL_ABS            13
#define UVCTB_ROLL_REL            14
#define UVCTB_FOCUS_AUTO          17
#define UVCTB_PRIVACY             18
#define UVCTB_FOCUS_SIMPLE        19 /* 1.5 */
#define UVCTB_WINDOW              20 /* 1.5 */
#define UVCTB_REGION_OF_INTEREST  21 /* 1.5 */

/* Processing Unit Control Selectors */
#define UVPUCS_BACKLIGHT_COMP_CONTROL     0x01
#define UVPUCS_BRIGHTNESS_CONTROL         0x02
#define UVPUCS_CONTRAST_CONTROL           0x03
#define UVPUCS_GAIN_CONTROL               0x04
#define UVPUCS_POWER_LINE_FREQ_CONTROL    0x05
#define UVPUCS_HUE_CONTROL                0x06
#define UVPUCS_SATURATION_CONTROL         0x07
#define UVPUCS_SHARPNESS_CONTROL          0x08
#define UVPUCS_GAMMA_CONTROL              0x09
#define UVPUCS_WB_TEMP_CONTROL            0x0a
#define UVPUCS_WB_TEMP_AUTO_CONTROL       0x0b
#define UVPUCS_WB_COMPONENT_CONTROL       0x0c
#define UVPUCS_WB_COMPONENT_AUTO_CONTROL  0x0d
#define UVPUCS_DIGITAL_MULT_CONTROL       0x0e
#define UVPUCS_DIGITAL_MULT_LIMIT_CONTROL 0x0f
#define UVPUCS_HUE_AUTO_CONTROL           0x10
#define UVPUCS_ANALOG_VIDEO_STD_CONTROL   0x11
#define UVPUCS_ANALOG_LOCK_STATUS_CONTROL 0x12
#define UVPUCS_CONTRAST_AUTO_CONTROL      0x13 /* 1.5 */

/* Processing Unit control bitmap, bit numbers */
#define UVPUB_BRIGHTNESS          0
#define UVPUB_CONTRAST            1
#define UVPUB_HUE                 2
#define UVPUB_SATURATION          3
#define UVPUB_SHARPNESS           4
#define UVPUB_GAMMA               5
#define UVPUB_WB_TEMP             6
#define UVPUB_WB_COMPONENT        7
#define UVPUB_BACKLIGHT_COMP      8
#define UVPUB_GAIN                9
#define UVPUB_POWER_LINE_FREQ     10
#define UVPUB_HUE_AUTO            11
#define UVPUB_WB_TEMP_AUTO        12
#define UVPUB_WB_COMPONENT_AUTO   13
#define UVPUB_DIGITAL_MULT        14
#define UVPUB_DIGITAL_MULT_LIMIT  15
#define UVPUB_ANALOG_VIDEO_STD    16
#define UVPUB_ANALOG_LOCK_STATUS  17
#define UVPUB_CONTRAST_AUTO       18 /* 1.5 */

/* Encoding Unit Control Selectors (1.5) */
#define UVEUCS_SELECT_LAYER_CONTROL        0x01
#define UVEUCS_PROFILE_TOOLSET_CONTROL     0x02
#define UVEUCS_VIDEO_RESOLUTION_CONTROL    0x03
#define UVEUCS_MIN_FRAME_INTERVAL_CONTROL  0x04
#define UVEUCS_SLICE_MODE_CONTROL          0x05
#define UVEUCS_RATE_CONTROL_MODE_CONTROL   0x06
#define UVEUCS_AVERAGE_BITRATE_CONTROL     0x07
#define UVEUCS_CPB_SIZE_CONTROL            0x08
#define UVEUCS_PEAK_BIT_RATE_CONTROL       0x09
#define UVEUCS_QUANTIZATION_PARAMS_CONTROL 0x0a
#define UVEUCS_SYNC_REF_FRAME_CONTROL      0x0b
#define UVEUCS_LTR_BUFFER_CONTROL          0x0c
#define UVEUCS_LTR_PICTURE_CONTROL         0x0d
#define UVEUCS_LTR_VALIDATION_CONTROL      0x0e
#define UVEUCS_LEVEL_IDC_LIMIT_CONTROL     0x0f
#define UVEUCS_SEI_PAYLOADTYPE_CONTROL     0x10
#define UVEUCS_QP_RANGE_CONTROL            0x11
#define UVEUCS_PRIORITY_CONTROL            0x12
#define UVEUCS_START_OR_STOP_LAYER_CONTROL 0x13
#define UVEUCS_ERROR_RESILIENCY_CONTROL    0x14

/* VideoStreaming Interface Control Selectors */
#define UVVSCS_PROBE_CONTROL              0x01
#define UVVSCS_COMMIT_CONTROL             0x02
#define UVVSCS_STILL_PROBE_CONTROL        0x03
#define UVVSCS_STILL_COMMIT_CONTROL       0x04
#define UVVSCS_IMAGE_TRIGGER_CONTROL      0x05
#define UVVSCS_STREAM_ERROR_CODE_CONTROL  0x06
#define UVVSCS_STREAM_ERROR_COE_CONTROL   0x06 /* the spelling of 2.0 */
#define UVVSCS_GENERATE_KEY_FRAME_CONTROL 0x07
#define UVVSCS_UPDATE_FRAME_SEG_CONTROL   0x08
#define UVVSCS_SYNCH_DELAY_CONTROL        0x09

/* Video Streaming input header bitmap (a format's row of bmaControls), bit
   numbers: what probe and commit take for the format (0..3), and the
   controls it has (4, 5) */
#define UVIHB_KEY_FRAME_RATE      0
#define UVIHB_P_FRAME_RATE        1
#define UVIHB_COMP_QUALITY        2
#define UVIHB_COMP_WINDOW_SIZE    3
#define UVIHB_GENERATE_KEY_FRAME  4 /* UVVSCS_GENERATE_KEY_FRAME_CONTROL */
#define UVIHB_UPDATE_FRAME_SEG    5 /* UVVSCS_UPDATE_FRAME_SEG_CONTROL */

/* Stream Error Code Control values (the reason for an ERR payload) */
#define UVSEC_NO_ERROR            0x00
#define UVSEC_PROTECTED_CONTENT   0x01
#define UVSEC_INPUT_UNDERRUN      0x02
#define UVSEC_DISCONTINUITY       0x03
#define UVSEC_OUTPUT_UNDERRUN     0x04
#define UVSEC_OUTPUT_OVERRUN      0x05
#define UVSEC_FORMAT_CHANGE       0x06 /* Device changed the format: read PROBE */
#define UVSEC_STILL_CAPTURE       0x07

/* Still Image Trigger Control values */
#define UVSIT_NORMAL              0x00
#define UVSIT_TRANSMIT            0x01 /* Method 2: still over the video pipe */
#define UVSIT_TRANSMIT_BULK       0x02 /* Method 3: still over the bulk still pipe */
#define UVSIT_ABORT               0x03


/*
** Class specific descriptors of the Video Control interface
*/

/* What all class specific descriptors begin with */
struct UsbVideoDesc
{
    UBYTE bLength;             /* Size of this descriptor */
    UBYTE bDescriptorType;     /* UDT_CS_INTERFACE (0x24), or 0x25 for an endpoint */
    UBYTE bDescriptorSubType;  /* UDST_VIDEO_ */
};

/* Video Control Header (subtype 0x01) */
struct UsbVideoHeaderDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UWORD bcdUVC;              /* The revision: UVC_VERSION_ */
    UWORD wTotalLength;        /* Of all class specific descriptors of the interface */
    ULONG dwClockFrequency;    /* Hz; deprecated */
    UBYTE bInCollection;       /* Number of streaming interfaces (n) */
    UBYTE baInterfaceNr[0];    /* n interface numbers */
};

/* Input Terminal (subtype 0x02) */
struct UsbVideoInputTermDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bTerminalID;
    UWORD wTerminalType;       /* UVUTT_ / UVITT_ / UVETT_ */
    UBYTE bAssocTerminal;
    UBYTE iTerminal;           /* String descriptor */
};

/* ...which a Camera Terminal (UVITT_CAMERA) continues */
struct UsbVideoCameraTermDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bTerminalID;
    UWORD wTerminalType;
    UBYTE bAssocTerminal;
    UBYTE iTerminal;
    UWORD wObjectiveFocalLengthMin;
    UWORD wObjectiveFocalLengthMax;
    UWORD wOcularFocalLength;
    UBYTE bControlSize;        /* Bytes of bitmap (n) */
    UBYTE bmControls[0];       /* n bytes, bits UVCTB_ */
};

/* Output Terminal (subtype 0x03) */
struct UsbVideoOutputTermDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bTerminalID;
    UWORD wTerminalType;       /* UVUTT_ / UVOTT_ / UVETT_ */
    UBYTE bAssocTerminal;
    UBYTE bSourceID;           /* The unit or terminal this one is fed by */
    UBYTE iTerminal;
};

/* Selector Unit (subtype 0x04) */
struct UsbVideoSelectorUnitDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bUnitID;
    UBYTE bNrInPins;           /* p */
    UBYTE baSourceID[0];       /* p source IDs; behind them: iSelector */
};

/* Processing Unit (subtype 0x05) */
struct UsbVideoProcessingUnitDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bUnitID;
    UBYTE bSourceID;
    UWORD wMaxMultiplier;
    UBYTE bControlSize;        /* Bytes of bitmap (n) */
    UBYTE bmControls[0];       /* n bytes, bits UVPUB_; behind them: iProcessing and,
                                  from UVC 1.1 on, bmVideoStandards */
};

/* Extension Unit (subtype 0x06) */
struct UsbVideoExtensionUnitDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bUnitID;
    UBYTE guidExtensionCode[16];
    UBYTE bNumControls;
    UBYTE bNrInPins;           /* p */
    UBYTE baSourceID[0];       /* p source IDs; behind them: bControlSize (n),
                                  n bytes of bmControls, iExtension */
};

/* Encoding Unit (subtype 0x07, 1.5) */
struct UsbVideoEncodingUnitDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bUnitID;
    UBYTE bSourceID;
    UBYTE iEncoding;
    UBYTE bControlSize;        /* Always 3 */
    UBYTE bmControls[3];       /* Selector s is bit s - 1 */
    UBYTE bmControlsRuntime[3]; /* Those that can be changed while streaming */
};

/* Class specific interrupt endpoint descriptor (UDST_VIDEO_EP_INTERRUPT) */
struct UsbVideoIntEPDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UWORD wMaxTransferSize;    /* The largest status packet */
};

/*
** Class specific descriptors of a Video Streaming interface
*/

/* Video Streaming Input Header (subtype 0x01) */
struct UsbVideoInputHeaderDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bNumFormats;         /* p */
    UWORD wTotalLength;
    UBYTE bEndpointAddress;    /* The video endpoint */
    UBYTE bmInfo;              /* Bit 0: dynamic format change supported */
    UBYTE bTerminalLink;       /* The Output Terminal this stream feeds */
    UBYTE bStillCaptureMethod; /* 0 none, 1..3 */
    UBYTE bTriggerSupport;
    UBYTE bTriggerUsage;       /* 0 start still capture, 1 general button */
    UBYTE bControlSize;        /* n */
    UBYTE bmaControls[0];      /* p rows of n bytes, one row per format: bits UVIHB_ */
};

/* Video Streaming Output Header (subtype 0x02). UVC 1.0 ends before
   bControlSize. */
struct UsbVideoOutputHeaderDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bNumFormats;
    UWORD wTotalLength;
    UBYTE bEndpointAddress;
    UBYTE bTerminalLink;
    UBYTE bControlSize;
    UBYTE bmaControls[0];
};

/* What every format descriptor begins with. The frame count is common to
   those that have frame descriptors. */
struct UsbVideoFormatDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFormatIndex;        /* One-based */
    UBYTE bNumFrameDescriptors;
};

/* MJPEG Format (subtype 0x06) */
struct UsbVideoMJPEGFormatDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFormatIndex;
    UBYTE bNumFrameDescriptors;
    UBYTE bmFlags;             /* Bit 0: fixed size samples */
    UBYTE bDefaultFrameIndex;
    UBYTE bAspectRatioX;
    UBYTE bAspectRatioY;
    UBYTE bmInterlaceFlags;
    UBYTE bCopyProtect;
};

/* Uncompressed Format (subtype 0x04) */
struct UsbVideoUncompressedFormatDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFormatIndex;
    UBYTE bNumFrameDescriptors;
    UBYTE guidFormat[16];      /* The first four bytes are the FourCC */
    UBYTE bBitsPerPixel;
    UBYTE bDefaultFrameIndex;
    UBYTE bAspectRatioX;
    UBYTE bAspectRatioY;
    UBYTE bmInterlaceFlags;
    UBYTE bCopyProtect;
};

/* Frame Based Format (subtype 0x10): the uncompressed one and one byte more */
struct UsbVideoFrameBasedFormatDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFormatIndex;
    UBYTE bNumFrameDescriptors;
    UBYTE guidFormat[16];
    UBYTE bBitsPerPixel;
    UBYTE bDefaultFrameIndex;
    UBYTE bAspectRatioX;
    UBYTE bAspectRatioY;
    UBYTE bmInterlaceFlags;
    UBYTE bCopyProtect;
    UBYTE bVariableSize;
};

/* Stream Based Format (subtype 0x12) - no frame descriptors */
struct UsbVideoStreamBasedFormatDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFormatIndex;
    UBYTE guidFormat[16];
    ULONG dwPacketLength;
};

/* MPEG-2 TS Format (subtype 0x0a) - no frame descriptors. From UVC 1.1 on
   16 bytes follow: guidStrideFormat. */
struct UsbVideoMPEG2TSFormatDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFormatIndex;
    UBYTE bDataOffset;
    UBYTE bPacketLength;
    UBYTE bStrideLength;
};

/* DV Format (subtype 0x0c) - no frame descriptors */
struct UsbVideoDVFormatDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFormatIndex;
    ULONG dwMaxVideoFrameBufferSize;
    UBYTE bFormatType;
};

/* H.264 Format (subtypes 0x13, 0x15). The macroblock rates come in fives
   by what is scaled, each for one to four resolutions at once. */
struct UsbVideoH264FormatDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFormatIndex;
    UBYTE bNumFrameDescriptors;
    UBYTE bDefaultFrameIndex;
    UBYTE bMaxCodecConfigDelay;
    UBYTE bmSupportedSliceModes;
    UBYTE bmSupportedSyncFrameTypes;
    UBYTE bResolutionScaling;
    UBYTE Reserved1;
    UBYTE bmSupportedRateControlModes;
    UWORD wMaxMBperSecNoScalability[4];
    UWORD wMaxMBperSecTemporalScalability[4];
    UWORD wMaxMBperSecTemporalQualityScalability[4];
    UWORD wMaxMBperSecTemporalSpatialScalability[4];
    UWORD wMaxMBperSecFullScalability[4];
};

/* VP8 Format (subtypes 0x16, 0x18) */
struct UsbVideoVP8FormatDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFormatIndex;
    UBYTE bNumFrameDescriptors;
    UBYTE bDefaultFrameIndex;
    UBYTE bMaxCodecConfigDelay;
    UBYTE bSupportedPartitionCount;
    UBYTE bmSupportedSyncFrameTypes;
    UBYTE bResolutionScaling;
    UBYTE bmSupportedRateControlModes;
    UWORD wMaxMBperSec;
};

/* What every frame descriptor begins with */
struct UsbVideoFrameHeadDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFrameIndex;         /* One-based, per format */
};

/* Uncompressed Frame (subtype 0x05) and MJPEG Frame (subtype 0x07).
   Intervals are in units of 100 ns. */
struct UsbVideoFrameDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFrameIndex;
    UBYTE bmCapabilities;      /* Bit 0: still image, bit 1: fixed rate */
    UWORD wWidth;
    UWORD wHeight;
    ULONG dwMinBitRate;
    ULONG dwMaxBitRate;
    ULONG dwMaxVideoFrameBufferSize; /* Deprecated */
    ULONG dwDefaultFrameInterval;
    UBYTE bFrameIntervalType;  /* 0: minimum, maximum, step follow; n: n intervals */
    ULONG dwFrameInterval[0];
};

/* Frame Based Frame (subtype 0x11) */
struct UsbVideoFrameBasedFrameDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFrameIndex;
    UBYTE bmCapabilities;
    UWORD wWidth;
    UWORD wHeight;
    ULONG dwMinBitRate;
    ULONG dwMaxBitRate;
    ULONG dwDefaultFrameInterval;
    UBYTE bFrameIntervalType;
    ULONG dwBytesPerLine;
    ULONG dwFrameInterval[0];
};

/* H.264 Frame (subtype 0x14) - the intervals are always a list */
struct UsbVideoH264FrameDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFrameIndex;
    UWORD wWidth;
    UWORD wHeight;
    UWORD wSARwidth;
    UWORD wSARheight;
    UWORD wProfile;
    UBYTE bLevelIDC;
    UWORD wConstrainedToolset;
    ULONG bmSupportedUsages;
    UWORD bmCapabilities;
    ULONG bmSVCCapabilities;
    ULONG bmMVCCapabilities;
    ULONG dwMinBitRate;
    ULONG dwMaxBitRate;
    ULONG dwDefaultFrameInterval;
    UBYTE bNumFrameIntervals;
    ULONG dwFrameInterval[0];
};

/* VP8 Frame (subtype 0x17) - the intervals are always a list */
struct UsbVideoVP8FrameDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bFrameIndex;
    UWORD wWidth;
    UWORD wHeight;
    ULONG bmSupportedUsages;
    UWORD bmCapabilities;
    ULONG bmScalabilityCapabilities;
    ULONG dwMinBitRate;
    ULONG dwMaxBitRate;
    ULONG dwDefaultFrameInterval;
    UBYTE bNumFrameIntervals;
    ULONG dwFrameInterval[0];
};

/* Still Image Frame (subtype 0x03) */
struct UsbVideoStillFrameDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bEndpointAddress;    /* Method 3: the bulk endpoint; else 0 */
    UBYTE bNumImageSizePatterns; /* n */
    UWORD wSizePatterns[0];    /* n pairs: width, height; behind them:
                                  bNumCompressionPattern (m) and m bytes */
};

/* Color Matching (subtype 0x0d) */
struct UsbVideoColorDesc
{
    UBYTE bLength;
    UBYTE bDescriptorType;
    UBYTE bDescriptorSubType;
    UBYTE bColorPrimaries;
    UBYTE bTransferCharacteristics;
    UBYTE bMatrixCoefficients;
};


/* Video Probe and Commit control block. The revision of the device decides
   how much of it travels: UVPC_SIZE_. */
struct UsbVideoProbeCommit
{
    UWORD bmHint;              /* UVPCHF_ */
    UBYTE bFormatIndex;
    UBYTE bFrameIndex;
    ULONG dwFrameInterval;     /* 100 ns units */
    UWORD wKeyFrameRate;
    UWORD wPFrameRate;
    UWORD wCompQuality;
    UWORD wCompWindowSize;
    UWORD wDelay;              /* ms */
    ULONG dwMaxVideoFrameSize;
    ULONG dwMaxPayloadTransferSize;
    /* from UVC 1.1 on */
    ULONG dwClockFrequency;    /* Hz: the unit of PTS and SCR */
    UBYTE bmFramingInfo;       /* UVPCFF_ */
    UBYTE bPreferedVersion;
    UBYTE bMinVersion;
    UBYTE bMaxVersion;
    /* from UVC 1.5 on */
    UBYTE bUsage;              /* UVPCU_ */
    UBYTE bBitDepthLuma;
    UBYTE bmSettings;
    UBYTE bMaxNumberOfRefFramesPlus1;
    UWORD bmRateControlModes;
    UBYTE bmLayoutPerStream[8];
};

#define UVPC_SIZE_10              offsetof(struct UsbVideoProbeCommit, dwClockFrequency) /* 26 */
#define UVPC_SIZE_11              offsetof(struct UsbVideoProbeCommit, bUsage)           /* 34 */
#define UVPC_SIZE_15              sizeof(struct UsbVideoProbeCommit)                     /* 48 */

/* bUsage of the H.264 and VP8 payloads: what the stream is for. 1 to 5 are
   the real time modes; this one every H.264 frame has to offer. */
#define UVPCU_FILE_STORAGE_IP     17 /* a file: key frames and predicted ones, in order */

/* bmHint: which fields the host wants kept fixed */
#define UVPCHF_FRAME_INTERVAL     0x0001
#define UVPCHF_KEY_FRAME_RATE     0x0002
#define UVPCHF_P_FRAME_RATE       0x0004
#define UVPCHF_COMP_QUALITY       0x0008
#define UVPCHF_COMP_WINDOW_SIZE   0x0010

/* bmFramingInfo */
#define UVPCFF_FID_REQUIRED       0x01
#define UVPCFF_EOF_PRESENT        0x02
#define UVPCFF_EOS_PRESENT        0x04

/* Still Probe and Commit control block */
struct UsbVideoStillProbeCommit
{
    UBYTE bFormatIndex;
    UBYTE bFrameIndex;         /* Index into the still size patterns */
    UBYTE bCompressionIndex;
    ULONG dwMaxVideoFrameSize;
    ULONG dwMaxPayloadTransferSize;
};

/* Generate Key Frame control block (UVVSCS_GENERATE_KEY_FRAME_CONTROL): for
   a format of temporal compression, while the stream runs */
struct UsbVideoGenerateKeyFrame
{
    UBYTE bGenerateKeyFrame;   /* UVGKF_ */
};

#define UVGKF_NORMAL              0
#define UVGKF_GENERATE            1 /* a key frame at the earliest opportunity */

/* Synchronization and Long-Term Reference Frame control block of an
   Encoding Unit (UVEUCS_SYNC_REF_FRAME_CONTROL, 1.5) */
struct UsbVideoSyncRefFrame
{
    UBYTE bSyncFrameType;      /* UVSFT_ */
    UWORD wSyncFrameInterval;  /* ms from one to the next; 0 = this one only */
    UBYTE bGradualDecoderRefresh; /* frames of a gradual refresh; 0 for any other type */
};

#define UVSFT_RESET               0 /* the encoder chooses */
#define UVSFT_KEY_FRAME           1 /* H.264: an IDR frame for every layer; VP8: an intra frame */


/* Payload header: starts every payload transfer. Optional fields follow the
   flags, of which VP8 has three bytes, in this order: the presentation time
   (a long), if UVPHF_PTS; the source clock reference, if UVPHF_SCR. */
struct UsbVideoPayloadHeader
{
    UBYTE bHeaderLength;       /* Including this byte */
    UBYTE bmHeaderInfo;        /* UVPHF_ */
};

struct UsbVideoSCR
{
    ULONG dwSourceClock;       /* The device's clock... */
    UWORD wSOFCounter;         /* ...at this count of USB frames: UVPH_SCR_FRAME_MASK */
};

#define UVPH_SCR_FRAME_MASK       0x07ff /* the count is 11 bits: it wraps like the bus's */

#define UVPHF_FID                 0x01 /* Toggles with every new frame */
#define UVPHF_EOF                 0x02 /* Last payload of the frame */
#define UVPHF_PTS                 0x04 /* PTS field present */
#define UVPHF_SCR                 0x08 /* SCR field present */
#define UVPHF_EOS                 0x10 /* Payload specific; H.264: end of slice */
#define UVPHF_STI                 0x20 /* Still image (temporal formats: intra frame) */
#define UVPHF_ERR                 0x40 /* Payload damaged, see UVSEC_ */
#define UVPHF_EOH                 0x80 /* End of header */

/* The names of 2.0 for the same bits */
#define UVMJHF_FRAME_ID      UVPHF_FID
#define UVMJHF_END_OF_FRAME  UVPHF_EOF
#define UVMJHF_HAS_PTS       UVPHF_PTS
#define UVMJHF_HAS_SCR       UVPHF_SCR
#define UVMJHF_STILL_IMAGE   UVPHF_STI
#define UVMJHF_ERROR         UVPHF_ERR
#define UVMJHF_END_OF_HEADER UVPHF_EOH


/* Status interrupt packet, as the Video Control interface sends it: event 0
   says that a control has changed */
struct UsbVideoStatusVC
{
    UBYTE bStatusType;         /* Low nibble: UVSPT_ */
    UBYTE bOriginator;         /* The entity */
    UBYTE bEvent;
    UBYTE bSelector;
    UBYTE bAttribute;          /* UVSPA_ */
    UBYTE bValue[0];           /* As the matching GET request returns it */
};

/* ...and as a Video Streaming interface sends it: event 0 is the button */
struct UsbVideoStatusVS
{
    UBYTE bStatusType;
    UBYTE bOriginator;         /* The interface's number */
    UBYTE bEvent;
    UBYTE bValue[0];           /* Button: 0 released, 1 pressed */
};

#define UVSPT_MASK                0x0f
#define UVSPT_VIDEOCONTROL        0x01
#define UVSPT_VIDEOSTREAMING      0x02

#define UVSPA_VALUE_CHANGE        0x00
#define UVSPA_INFO_CHANGE         0x01
#define UVSPA_FAILURE_CHANGE      0x02
#define UVSPA_MIN_CHANGE          0x03
#define UVSPA_MAX_CHANGE          0x04


/*
** H.264 from cameras of USB Video Class 1.0 and 1.1
**
** Before the video class had an H.264 format of its own (1.5), the USB-IF
** gave cameras this way to deliver H.264 (USB Device Class Definition for
** Video Devices: H.264 Payload, revision 1.00, 2011): an extension unit
** through which the camera's encoder is set up, and, for a camera with one
** streaming interface, H.264 data put into the JPEG pictures of its MJPEG
** stream. Section and table numbers below are that document's.
*/

/* The extension unit: guidExtensionCode in wire order (appendix A:
   {A29E7641-DE04-47E3-8B2B-F4341AFF003B}) */
#define UVCX_H264_XU_GUID { 0x41, 0x76, 0x9e, 0xa2, 0x04, 0xde, 0xe3, 0x47, \
                            0x8b, 0x2b, 0xf4, 0x34, 0x1a, 0xff, 0x00, 0x3b }

/* Its controls (table 1) */
#define UVCX_VIDEO_CONFIG_PROBE         0x01 /* negotiate: struct below */
#define UVCX_VIDEO_CONFIG_COMMIT        0x02 /* put in force: the same struct, SET_CUR only */
#define UVCX_RATE_CONTROL_MODE          0x03
#define UVCX_TEMPORAL_SCALE_MODE        0x04
#define UVCX_SPATIAL_SCALE_MODE         0x05
#define UVCX_SNR_SCALE_MODE             0x06
#define UVCX_LTR_BUFFER_SIZE_CONTROL    0x07
#define UVCX_LTR_PICTURE_CONTROL        0x08
#define UVCX_PICTURE_TYPE_CONTROL       0x09 /* asks for a key frame */
#define UVCX_VERSION                    0x0a /* word, BCD: 0x0100 */
#define UVCX_ENCODER_RESET              0x0b
#define UVCX_FRAMERATE_CONFIG           0x0c
#define UVCX_VIDEO_ADVANCE_CONFIG       0x0d
#define UVCX_BITRATE_LAYERS             0x0e
#define UVCX_QP_STEPS_LAYERS            0x0f

/* The configuration of the encoder (table 2), for UVCX_VIDEO_CONFIG_PROBE
   and _COMMIT. GET_MAX answers with the largest value of every field taken
   by itself, GET_CUR with a configuration the camera can do. */
struct UvcxVideoConfig
{
    ULONG dwFrameInterval;     /* 100 ns units */
    ULONG dwBitRate;           /* bits per second on average */
    UWORD bmHints;             /* UVCXHF_: what the camera should keep */
    UWORD wConfigurationIndex;
    UWORD wWidth;
    UWORD wHeight;
    UWORD wSliceUnits;
    UWORD wSliceMode;
    UWORD wProfile;            /* profile_idc << 8 | constraint flags */
    UWORD wIFramePeriod;       /* ms between key frames; 0 = as the camera likes */
    UWORD wEstimatedVideoDelay; /* ms */
    UWORD wEstimatedMaxConfigDelay; /* ms */
    UBYTE bUsageType;          /* 1 real time, 2 broadcast, 3 storage */
    UBYTE bRateControlMode;    /* 1 CBR, 2 VBR, 3 constant QP */
    UBYTE bTemporalScaleMode;
    UBYTE bSpatialScaleMode;
    UBYTE bSNRScaleMode;
    UBYTE bStreamMuxOption;    /* UVCXMF_ */
    UBYTE bStreamFormat;       /* 0 byte stream (Annex B), 1 NAL stream */
    UBYTE bEntropyCABAC;
    UBYTE bTimestamp;
    UBYTE bNumOfReorderFrames;
    UBYTE bPreviewFlipped;
    UBYTE bView;
    UBYTE bReserved1;
    UBYTE bReserved2;
    UBYTE bStreamID;
    UBYTE bSpatialLayerRatio;
    UWORD wLeakyBucketSize;    /* ms */
};

#define UVCXHF_RESOLUTION               0x0001
#define UVCXHF_PROFILE                  0x0002
#define UVCXHF_RATE_CONTROL_MODE        0x0004
#define UVCXHF_USAGE_TYPE               0x0008
#define UVCXHF_FRAME_INTERVAL           0x0800
#define UVCXHF_BIT_RATE                 0x2000
#define UVCXHF_IFRAME_PERIOD            0x8000

/* bStreamMuxOption: what the camera puts into its MJPEG frames */
#define UVCXMF_ENABLE                   0x01 /* an auxiliary stream is embedded */
#define UVCXMF_H264                     0x02 /* H.264 */
#define UVCXMF_YUY2                     0x04
#define UVCXMF_NV12                     0x08
#define UVCXMF_CONTAINER                0x40 /* the JPEG picture itself is of no use */

/*
** Controls that work while the stream runs (3.3.2). Each begins with a word
** that names a layer of a layered stream; 0 for a plain one.
*/

/* UVCX_PICTURE_TYPE_CONTROL */
struct UvcxPictureType
{
    UWORD wLayerID;
    UWORD wPicType;            /* UVCXPT_: what the next frame is */
};

#define UVCXPT_I_FRAME                  0
#define UVCXPT_IDR                      1 /* a key frame */
#define UVCXPT_NEW_SETS                 2 /* a key frame with new parameter sets */

/* UVCX_BITRATE_LAYERS */
struct UvcxBitrateLayers
{
    UWORD wLayerID;
    ULONG dwPeakBitrate;       /* bits per second */
    ULONG dwAverageBitrate;
};

/* UVCX_FRAMERATE_CONFIG */
struct UvcxFramerateConfig
{
    UWORD wLayerID;
    ULONG dwFrameInterval;     /* 100 ns units */
};

/*
** The embedded stream (3.5). Its data sits in application segments with the
** marker below, in front of the picture data (the SOS marker) of a JPEG
** frame, each segment at most 64 KB. The first segment of a stream begins
** with this header; wHeaderLength bytes from its start comes the payload
** size - a long: the bytes of the stream in this frame, counting the marker
** and length of every further segment - and then the data.
*/
#define UVCX_MUX_MARKER                 0xe4 /* APP4 */

struct UvcxMuxHeader
{
    UWORD wVersion;            /* 0x0100 */
    UWORD wHeaderLength;       /* 22 in version 1.0 */
    ULONG dwStreamType;        /* the FourCC - four characters in reading order, so
                                  this one field is big-endian */
    UWORD wWidth;
    UWORD wHeight;
    ULONG dwFrameInterval;     /* 100 ns units */
    UWORD wDelay;              /* ms */
    ULONG dwPresentationTime;
};

#if defined(__GNUC__)
# pragma pack()
#endif

#endif /* DEVICES_USB_VIDEO_H */
