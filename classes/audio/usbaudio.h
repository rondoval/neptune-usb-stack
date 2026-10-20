#ifndef USBAUDIO_H
#define USBAUDIO_H

#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <libraries/mui.h>
#include <libraries/gadtools.h>
#include <devices/ahi.h>
#include <devices/timer.h>

#define ID_ABOUT        0x55555555
#define ID_STORE_CONFIG 0xaaaaaaaa

#define AHI_SUB_LIB_VERSION 4
#define AHI_USB_MODE_BASE 0x003b0000

#define AHIDB_NepAudioMode (AHIDB_UserBase+0)

/* Recorded samples go to AHI in blocks of this many frames, from two buffers in turn */
#define REC_BLOCK_FRAMES 1024
/* A recorded frame as AHI gets it (AHIST_S16S): left and right WORD */
#define REC_FRAME_WORDS  2

/* Input and output names a mode keeps for AHI */
#define MAX_PORT_NAMES   8

/* How deep a walk through the unit graph goes (descriptors may wire a loop) */
#define UNIT_DEPTH_MAX   32

#define UAF_MASTER_VOLUME  0x0001
#define UAF_INPUT_GAIN     0x0002
#define UAF_MONITOR_VOLUME 0x0004
#define UAF_SELECT_INPUT   0x0100
#define UAF_SELECT_OUTPUT  0x0200

struct ClsGlobalCfg
{
    ULONG cgc_ChunkID;
    ULONG cgc_Length;
};

/* Feature Unit channel numbers (UAC 1.0, 5.2.2.4.3: wValue low byte; 0 = the master
   Control) and their bits in nau_VolCtrlMask / nau_MuteCtrlMask. Only master, left and
   right are used. */
#define FU_CHAN_MASTER  0
#define FU_CHAN_LEFT    1
#define FU_CHAN_RIGHT   2
#define FU_CHANNELS     3
#define FU_CHANBIT(chan) (1 << (chan))

struct NepAudioUnit
{
    struct Node         nau_Node;         /* Node linkage */
    UWORD               nau_Type;         /* Type as in USB audio spec */
    UWORD               nau_UnitID;       /* ID of this unit / input / output / feature */
    UWORD               nau_NumInputs;    /* Number of inputs */
    UWORD               nau_NumOutputs;   /* Number of outputs */
    struct NepAudioUnit *nau_InputUnit[8];/* Sourcing Units (or NULL for input terminals) */
    struct NepAudioUnit *nau_OutputUnit[8]; /* Outputting Units */
    UWORD               nau_OutChannels;  /* Number of outgoing channels */
    UWORD               nau_ChannelCfg;   /* Spacial location */
    UWORD               nau_TermType;     /* Terminal Type */
    UBYTE              *nau_Descriptor;   /* Raw descriptor */
    STRPTR              nau_Name;         /* Name of unit */
    struct NepAudioUnit *nau_RootUnit;    /* Top of the chain */
    struct NepAudioUnit *nau_SinkUnit;    /* USB Streaming unit this goes to */
    struct NepAudioUnit *nau_SourceUnit;  /* USB Streaming unit this comes from */

    // only for root
    struct NepAudioUnit *nau_VolumeUnit;  /* Volume control unit */
    struct NepAudioUnit *nau_MonitorUnit; /* Monitor Volume unit */
    struct NepAudioUnit *nau_SelectorUnit; /* Selector Unit feeding this recording stream: picks the
                                              recorded input (AHI input); outputs have none */
    BOOL                nau_VolumeControl; /* Master volume control */
    BOOL                nau_Monitor;      /* Monitor volume control */
    BOOL                nau_Balance;      /* Balance Panning support */

    UWORD               nau_VolCtrlMask;  /* Volume Controls, FU_CHANBIT() per channel */
    UWORD               nau_MuteCtrlMask; /* Mute Controls, FU_CHANBIT() per channel */
    UWORD               nau_VolCtrlCount; /* Number of channels */

    WORD                nau_MinVolDb;     /* Min Volume in 8.8 DB */
    WORD                nau_MaxVolDb;     /* Max Volume in 8.8 DB */

    Fixed               nau_MinVolume;    /* Minimum volume for this unit */
    Fixed               nau_MaxVolume;    /* Maximum volume for this unit */
};

/* An output terminal a playback stream reaches, as an AHI output */
struct NepAudioOutput
{
    STRPTR               nao_Name;        /* What AHI shows */
    struct NepAudioUnit *nao_Terminal;    /* The output terminal */
    struct NepAudioUnit *nao_Mute;        /* Feature Unit with a Mute Control on its own branch; NULL = none */
    struct NepAudioUnit *nao_Volume;      /* Feature Unit with a Volume Control on its own branch; NULL = none */
};

struct NepAudioMode
{
    /* Where the mode belongs */
    struct Node         nam_Node;         /* Node linkage */
    struct NepAudioSubLibBase *nam_SubLibBase; /* usbaudio.audio AHI driver library */
    struct NepClassAudio *nam_Unit;       /* Up linkage */
    struct Library     *nam_PsdBase;      /* neptune.library base */
    struct NepAudioMode *nam_RecStream;   /* Of a playback mode: the recording stream that goes with it
                                             (full duplex); NULL = output only */
    struct NepAudioMode *nam_PartOf;      /* Of a recording stream: the AHI mode it belongs to - the playback
                                             mode it records for, or the record-only mode whose alternate it
                                             is; NULL = an AHI mode of its own */

    /* The format, from the streaming interface's descriptors */
    BOOL                nam_IsInput;      /* Is Input (recording) */
    UWORD               nam_NumChannels;  /* Number of channels */
    UWORD               nam_Resolution;   /* Number of bits per Sample */
    UWORD               nam_SampleSize;   /* Number of bytes per Sample */
    UWORD               nam_FrameSize;    /* Number of bytes per Frame */
    UWORD               nam_TerminalID;   /* ID of the terminal */
    BOOL                nam_HasFreqCtrl;  /* Has Frequency control */
    BOOL                nam_HasPitchCtrl; /* Has Pitch control */
    ULONG               nam_MinFreq;      /* Minimum Frequency */
    ULONG               nam_MaxFreq;      /* Maximum Frequency */
    ULONG               nam_NumFrequencies; /* Number of supported frequencies */
    ULONG               nam_FreqArray[64]; /* Array of supported frequencies */

    /* The USB side: interface, endpoint and the stream on it */
    ULONG               nam_IfNum;        /* Interface number */
    struct PsdInterface *nam_Interface;   /* Interface for mode */
    struct PsdInterface *nam_ZeroBWIF;    /* Interface for without bandwidth use */
    struct PsdEndpoint *nam_EP;           /* Iso Endpoint */
    IPTR                nam_EPNum;        /* Endpoint number */
    IPTR                nam_EPAddr;       /* Endpoint address (number | direction bit) for endpoint-recipient wIndex */
    ULONG               nam_IntervalBytes; /* What one service interval of the endpoint carries at most */
    ULONG               nam_Interval;     /* Interval in frames (milliseconds) */
    struct PsdRTIsoHandler *nam_RTIso;    /* The isochronous stream */
    struct PsdPipe     *nam_EP0Pipe;      /* Endpoint 0 pipe */
    struct MsgPort     *nam_TaskMsgPort;  /* Message Port of Subtask */
    struct Hook         nam_ReleaseHook;  /* Device Release hook */

    /* The AHI mode: what AHI is told, and what the program set */
    ULONG               nam_AHIModeID;    /* AHI Mode ID */
    struct TagItem      nam_Tags[12];     /* AudioDB Tags */
    struct AHIAudioCtrlDrv *nam_AudioCtrl; /* AHI Audio Ctrl */
    UWORD               nam_AHIFrameSize; /* Number of bytes per AHI Frame */
    struct NepAudioUnit *nam_RootUnit;    /* Root unit containing control information */
    ULONG               nam_NumInputs;    /* Number of Inputs */
    STRPTR              nam_InputNames[MAX_PORT_NAMES]; /* Names for the inputs */
    ULONG               nam_NumOutputs;   /* Number of Outputs */
    struct NepAudioOutput nam_Outputs[MAX_PORT_NAMES]; /* The output terminals (nMapOutputs) */
    struct NepAudioUnit *nam_OutVolume;   /* Feature Unit with a Volume Control upstream of every output;
                                             NULL = each output's own nao_Volume */
    BOOL                nam_OutputSwitch; /* Several outputs, each with its own Mute: AHI may choose one */
    UWORD               nam_CurrInput;    /* Current input */
    UWORD               nam_CurrOutput;   /* Current output */
    Fixed               nam_MasterVol;    /* Master volume for the unit */
    Fixed               nam_ChannelVol[2]; /* Left and right channel */
    Fixed               nam_InputGain;    /* Input gain */
    Fixed               nam_MonitorVol;   /* Monitor volume */

    /* AHI's player and mixer: every mode, also one that only records */
    LONG                nam_PlayerTimer;  /* Descending Timer when to call next player */
    Fixed               nam_PlayerInterval; /* Player interval (upper word is holds ms) */
    Fixed               nam_PlayerFrac;   /* Player increment */
    ULONG               nam_PlayerMS;     /* Number of milliseconds to mix before player gets called next time */
    struct Interrupt    nam_PlayerInt;    /* Interrupt to cause when Player needs to be called */
    Fixed               nam_FramesPerMS;  /* Frames per millisecond at the mix rate (16.16) */
    ULONG               nam_MaxMixFrames; /* The most frames one player pass mixes: what nam_MixBuffer and
                                             the play buffers hold */
    APTR                nam_MixBuffer;    /* What AHI mixes into (16 bit samples, 32 bit for HiFi) */
    BOOL                nam_Playing;      /* AHI started playback (subLibStart) and has not stopped it */
    BOOL                nam_FallbackTimer; /* The timer, not USB, clocks the player: the mode only records,
                                             or the device was unplugged */
    struct MsgPort      nam_TimerMsgPort; /* Msg port for Timer device on device removal */
    struct timerequest *nam_TimerIOReq;   /* Timer Request */

    /* Sample conversion between AHI and the device, the mode's direction */
    struct Hook         nam_SamConvHook;  /* Sample conversion hook */

    /* Playback: the mix, converted, to the device */
    UBYTE              *nam_PlayBuffer[2]; /* Samples in the device's format, filled and sent in turn */
    ULONG               nam_PlayLen[2];   /* Bytes in each play buffer */
    Fixed               nam_PlayCarry[2]; /* Part frame carried into the start of each play buffer */
    UWORD               nam_FillBuf;      /* 0/1: play buffer the player mixes into next */
    UWORD               nam_SendBuf;      /* 0/1: play buffer going to the device */
    LONG                nam_SendLeft;     /* Bytes of the send buffer not yet given to the host controller;
                                             <= 0: on to the other buffer */
    Fixed               nam_SendCarry;    /* Part frame carried from one interval to the next; on the
                                             timer clock, from one player pass to the next */

    /* Recording: from the device, converted, to AHI */
    UBYTE              *nam_PacketBuffer; /* The service interval the host controller wrote */
    WORD               *nam_RecBuffer;    /* Recorded samples as AHI gets them (16 bit stereo): two blocks */
    struct AHIRecordMessage nam_RecMsg;   /* Record message: the block being filled and its frames */
    ULONG               nam_RecFrames;    /* Frames recorded since the recording was started... */
    struct timeval      nam_RecStart;     /* ...which was then; tv_secs is 0 while nothing records */
};

struct NepAudioBase
{
    struct Library      nh_Library;       /* standard */
    UWORD               nh_Flags;         /* various flags */

    BPTR                nh_SegList;       /* load seglist (stored by the class skeleton) */
    struct Library     *nh_UtilityBase;   /* utility base */
    struct NepAudioSubLibBase *nh_SubLibBase; /* usbaudio.audio AHI driver library */
    struct List         nh_Units;         /* List of bindings created */

    struct Library     *nh_MUIBase;       /* MUI master base */
    struct Library     *nh_PsdBase;       /* neptune.library base */
    struct Library     *nh_IntBase;       /* Intuition base */
    struct Task        *nh_GUITask;       /* GUI Task */

    struct ClsGlobalCfg nh_CurrentCGC;
    BOOL                nh_UsingDefaultCfg;

    Object             *nh_App;
    Object             *nh_MainWindow;
    Object             *nh_UseObj;
    Object             *nh_CloseObj;

    Object             *nh_AboutMI;
    Object             *nh_UseMI;
    Object             *nh_MUIPrefsMI;
};

struct NepClassAudio
{
    struct Node         nch_Node;         /* Node linkage */
    ULONG               nch_UnitNo;       /* Unit number */
    struct NepAudioBase *nch_ClsBase;     /* Up linkage */
    struct Library     *nch_Base;         /* neptune.library base */
    struct Library     *nch_AHIBase;      /* AHI Device base */
    struct PsdDevice   *nch_Device;       /* Up linkage */
    struct PsdConfig   *nch_Config;       /* Up linkage */
    struct PsdInterface *nch_Interface;   /* Up linkage */
    struct PsdPipe     *nch_EP0Pipe;      /* Endpoint 0 pipe */
    struct Task        *nch_ReadySigTask; /* Task to send ready signal to */
    LONG                nch_ReadySignal;  /* Signal to send when ready */
    struct Task        *nch_Task;         /* Subtask */
    struct MsgPort     *nch_TaskMsgPort;  /* Message Port of Subtask */
    IPTR                nch_IfNum;        /* Interface Number */
    struct NepAudioMode *nch_CurrentMode; /* Currently selected mode */

    ULONG               nch_UpdateFlags;  /* Update Flags for Ctrl */

    struct List         nch_AudioModes;   /* List of audio modes */
    struct List         nch_AudioUnits;   /* List of audio units */

    struct Hook         nch_OutReqHook;   /* Hook for output */
    struct Hook         nch_InReqHook;    /* Hook for input */
    struct Hook         nch_InDoneHook;   /* Hook for input */

    BOOL                nch_DenyRequests; /* Do not accept further IO requests */

    struct MsgPort     *nch_AHIMsgPort;   /* AHI Device msg Port */
    struct AHIRequest  *nch_AHIReq;       /* AHI IO Request */
};

struct NepAudioSubLibBase
{
    struct Library      nas_Library;      /* standard */
    UWORD               nas_Flags;        /* various flags */
    struct NepAudioBase *nas_ClsBase;     /* Up linkage */
    struct Library     *nas_UtilityBase;  /* utility base */
    BPTR                nas_SegList;      /* device seglist */
};


/* ROM-safe per-instance MUI base (GUI subtask tc_UserData) + the MUI_NewObject fix. */
#define MUI_BASE_USERDATA struct NepAudioBase
#define MUI_BASE_FIELD    nh_MUIBase
#include "mui_base.h"

#endif /* USBAUDIO_H */
