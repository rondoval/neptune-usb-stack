/*
 *----------------------------------------------------------------------------
 *              USBVideo: the window
 *----------------------------------------------------------------------------
 * Implements usbvideo_gui.h.
 *
 * Which formats, sizes and rates can be chosen depends on the choice before.
 * A cycle gadget's entries are fixed once it exists (MUI 3.8), so each list
 * is a gadget replaced whole inside a group of its own (struct Choice).
 */

#include <proto/dos.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>

#include <clib/alib_protos.h>
#include <dos/dos.h>
#include <exec/memory.h>
#include <libraries/asl.h>
#include <libraries/iffparse.h>
#include <libraries/mui.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "uvc_client.h"
#include "uvc_preview.h"
#include "usbvideo_gui.h"

#define MAX_UNITS    8  /* units looked at for a camera */
#define CHOICE_MAX   32 /* entries of a list */
#define CHOICE_TEXT  64
#define STEM_SIZE    256
#define LINE_SIZE    160
#define DEFAULT_STEM "RAM:cam"

/* Frame rates offered from a continuous range, fastest first */
static const UBYTE RangeRates[] = { 60, 50, 30, 25, 24, 20, 15, 10, 5, 1 };

/* Button and gadget IDs for MUIM_Application_ReturnID */
enum
{
    ID_CAMERA = 1,
    ID_RESCAN,
    ID_FORMAT,
    ID_SIZE,
    ID_START,
    ID_STOP,
    ID_SNAPSHOT,
    ID_RECORD,
    ID_SETTINGS
};

/* A cycle gadget with entries of the moment, and a value for each */
struct Choice
{
    Object *ch_Group;                         /* holds the gadget while it is replaced */
    Object *ch_Cycle;                         /* the gadget of the moment */
    ULONG   ch_Id;                            /* its ReturnID */
    STRPTR  ch_Names[CHOICE_MAX + 1];         /* MUIA_Cycle_Entries, NULL at the end; "none" if empty */
    UBYTE   ch_Text[CHOICE_MAX][CHOICE_TEXT]; /* the entries ch_Names points at */
    ULONG   ch_Values[CHOICE_MAX];            /* a value per entry: see struct Gui */
    ULONG   ch_Count;                         /* entries in ch_Text and ch_Values */
};

struct Gui
{
    Object *gu_App;
    Object *gu_Window;
    Object *gu_Info;
    Object *gu_BitRate;
    Object *gu_KeyInterval;
    Object *gu_StemPop;
    Object *gu_Stem;
    Object *gu_Rescan;
    Object *gu_Start;
    Object *gu_Stop;
    Object *gu_Snapshot;
    Object *gu_Record;
    Object *gu_Settings;
    Object *gu_Status;
    Object *gu_Why;

    struct Choice gu_Camera;  /* values: the unit */
    struct Choice gu_Format;  /* the FourCC */
    struct Choice gu_Size;    /* the mode, in the stream's table */
    struct Choice gu_Rate;    /* the interval */

    struct UvcPreview *gu_Stream;
    ULONG              gu_StopMask;
    ULONG              gu_TickMask;

    /* the counts at the last look, for rates */
    struct timeval     gu_LastTime;
    struct UvpCounts   gu_Last;
};

struct Library *MUIMasterBase;

/*
 * Lists
 */

static void choiceClear(struct Choice *ch)
{
    ch->ch_Count = 0;
    ch->ch_Names[0] = (STRPTR) "none";
    ch->ch_Names[1] = NULL;
}

/* Add an entry; those past CHOICE_MAX are left out */
static void choiceAdd(struct Choice *ch, ULONG value, const char *format, ...)
{
    va_list args;

    if(ch->ch_Count == CHOICE_MAX)
    {
        return;
    }
    va_start(args, format);
    vsnprintf((char *) ch->ch_Text[ch->ch_Count], CHOICE_TEXT, format, args);
    va_end(args);
    ch->ch_Values[ch->ch_Count] = value;
    ch->ch_Names[ch->ch_Count] = ch->ch_Text[ch->ch_Count];
    ch->ch_Count++;
    ch->ch_Names[ch->ch_Count] = NULL;
}

/* Return the index of the entry with this value, 0 if none has it */
static ULONG choiceFind(const struct Choice *ch, ULONG value)
{
    for(ULONG cnt = 0; cnt < ch->ch_Count; cnt++)
    {
        if(ch->ch_Values[cnt] == value)
        {
            return cnt;
        }
    }
    return 0;
}

/* Return the value of the entry chosen; 0 if the list is empty */
static ULONG choiceValue(const struct Choice *ch)
{
    IPTR active = 0;

    if(!ch->ch_Count)
    {
        return 0;
    }
    get(ch->ch_Cycle, MUIA_Cycle_Active, &active);
    return ch->ch_Values[active];
}

/* A new cycle gadget for the list as it is now, in place of the old one */
static void choiceShow(struct Gui *gui, struct Choice *ch, ULONG active)
{
    DoMethod(ch->ch_Group, MUIM_Group_InitChange);
    if(ch->ch_Cycle)
    {
        DoMethod(ch->ch_Group, OM_REMMEMBER, ch->ch_Cycle);
        MUI_DisposeObject(ch->ch_Cycle);
    }
    ch->ch_Cycle = CycleObject,
        MUIA_CycleChain, 1,
        MUIA_Cycle_Entries, (IPTR) ch->ch_Names,
        MUIA_Cycle_Active, active,
        End;
    if(ch->ch_Cycle)
    {
        DoMethod(ch->ch_Group, OM_ADDMEMBER, ch->ch_Cycle);
        DoMethod(ch->ch_Cycle, MUIM_Notify, MUIA_Cycle_Active, MUIV_EveryTime,
                 gui->gu_App, 2, MUIM_Application_ReturnID, ch->ch_Id);
    }
    DoMethod(ch->ch_Group, MUIM_Group_ExitChange);
}

/* Fill the rates of a mode. Of a continuous range, the usual ones in it. */
static void fillRates(struct Gui *gui, const struct UsbVideoMode *uvm)
{
    struct Choice *ch = &gui->gu_Rate;

    choiceClear(ch);
    if(uvm && (uvm->uvm_Flags & UVMF_CONTINUOUS))
    {
        for(ULONG cnt = 0; cnt < sizeof(RangeRates); cnt++)
        {
            ULONG interval = UVC_INTERVAL_UNITS / RangeRates[cnt];

            if((interval >= uvm->uvm_Intervals[0]) && (interval <= uvm->uvm_Intervals[1]))
            {
                choiceAdd(ch, interval, "%lu fps", (ULONG) RangeRates[cnt]);
            }
        }
    }
    else if(uvm)
    {
        for(ULONG cnt = 0; cnt < uvm->uvm_NumIntervals; cnt++)
        {
            ULONG interval = uvm->uvm_Intervals[cnt];

            if(interval)
            {
                choiceAdd(ch, interval, "%lu.%lu fps", UVC_INTERVAL_UNITS / interval,
                          (10 * UVC_INTERVAL_UNITS / interval) % 10);
            }
        }
    }
    choiceShow(gui, ch, uvm ? choiceFind(ch, uvm->uvm_DefInterval) : 0);
}

/* The mode the Format and Size lists point at, NULL if none */
static const struct UsbVideoMode *chosenMode(const struct Gui *gui)
{
    return (const struct UsbVideoMode *) choiceValue(&gui->gu_Size);
}

/* Fill the sizes of the format chosen; the default mode is chosen if it is
   one of them, and the rates follow */
static void fillSizes(struct Gui *gui)
{
    struct Choice *ch = &gui->gu_Size;
    ULONG fourcc = choiceValue(&gui->gu_Format);
    ULONG active = 0;

    choiceClear(ch);
    if(gui->gu_Stream)
    {
        const struct UvcCamera *cm = uvpCamera(gui->gu_Stream);

        UVC_EACH_MODE(uvm, cm->cm_Modes, cm->cm_ModesSize)
        {
            if(uvm->uvm_FourCC == fourcc)
            {
                if(uvm->uvm_Flags & UVMF_DEFAULT)
                {
                    active = ch->ch_Count;
                }
                choiceAdd(ch, (ULONG) uvm, "%lu x %lu", (ULONG) uvm->uvm_Width, (ULONG) uvm->uvm_Height);
            }
        }
    }
    choiceShow(gui, ch, active);
    fillRates(gui, chosenMode(gui));
}

/* Fill the formats of the camera; MJPEG comes first where there is one, and
   the sizes follow */
static void fillFormats(struct Gui *gui)
{
    struct Choice *ch = &gui->gu_Format;

    choiceClear(ch);
    if(gui->gu_Stream)
    {
        const struct UvcCamera *cm = uvpCamera(gui->gu_Stream);

        UVC_EACH_MODE(uvm, cm->cm_Modes, cm->cm_ModesSize)
        {
            ULONG fourcc = uvm->uvm_FourCC;
            UBYTE name[UVC_FOURCC_SIZE];
            BOOL known = (ch->ch_Count != 0) && (ch->ch_Values[choiceFind(ch, fourcc)] == fourcc);

            if(!known)
            {
                choiceAdd(ch, fourcc, ((fourcc == UVFCC_MJPG) || (fourcc == UVFCC_H264)) ? "%s" : "%s (no preview)",
                          uvcFourCCText(fourcc, name));
            }
        }
    }
    choiceShow(gui, ch, choiceFind(ch, UVFCC_MJPG));
    fillSizes(gui);
}

/*
 * What the window says
 */

/* Say what is true now: which gadgets work, and why not */
static void showState(struct Gui *gui)
{
    BOOL camera = gui->gu_Stream != NULL;
    BOOL running = camera && uvpRunning(gui->gu_Stream);
    BOOL h264 = choiceValue(&gui->gu_Format) == UVFCC_H264;

    set(gui->gu_Camera.ch_Group, MUIA_Disabled, running);
    set(gui->gu_Rescan, MUIA_Disabled, running);
    set(gui->gu_Format.ch_Group, MUIA_Disabled, running || !camera);
    set(gui->gu_Size.ch_Group, MUIA_Disabled, running || !camera);
    set(gui->gu_Rate.ch_Group, MUIA_Disabled, running || !camera);
    set(gui->gu_BitRate, MUIA_Disabled, running || !h264);
    set(gui->gu_KeyInterval, MUIA_Disabled, running || !h264);
    set(gui->gu_StemPop, MUIA_Disabled, running);
    set(gui->gu_Start, MUIA_Disabled, running || !chosenMode(gui));
    set(gui->gu_Stop, MUIA_Disabled, !running);
    set(gui->gu_Snapshot, MUIA_Disabled, !running || uvcIsStream(choiceValue(&gui->gu_Format)));
    set(gui->gu_Record, MUIA_Disabled, !running);
    set(gui->gu_Settings, MUIA_Disabled, !camera);
    if(!running)
    {
        nnset(gui->gu_Record, MUIA_Selected, FALSE);
    }
    if(camera)
    {
        set(gui->gu_Why, MUIA_Text_Contents, (IPTR) uvpWhy(gui->gu_Stream));
    }
}

/* A second of frames passed: the rates since the last look */
static void showCounts(struct Gui *gui)
{
    const struct UvpCounts *uc = uvpCounts(gui->gu_Stream);
    const struct UvpCounts *last = &gui->gu_Last;
    struct timeval now;
    UBYTE text[LINE_SIZE];

    CurrentTime(&now.tv_secs, &now.tv_micro);

    ULONG ms = (now.tv_secs - gui->gu_LastTime.tv_secs) * 1000 + (now.tv_micro / 1000) - (gui->gu_LastTime.tv_micro / 1000);

    if(!ms)
    {
        return;
    }

    ULONG frames = (uc->uc_Frames - last->uc_Frames) * 10000 / ms;
    ULONG shown = (uc->uc_Shown - last->uc_Shown) * 10000 / ms;
    ULONG kbits = (uc->uc_Bytes - last->uc_Bytes) / ms * 8;

    gui->gu_LastTime = now;
    gui->gu_Last = *uc;
    snprintf((char *) text, sizeof(text), "%lu.%lu fps in, %lu.%lu shown, %lu lost, %lu kbit/s, %lu saved",
             frames / 10, frames % 10, shown / 10, shown % 10, uc->uc_Lost, kbits, uc->uc_Saved);
    set(gui->gu_Status, MUIA_Text_Contents, (IPTR) text);
}

/*
 * Cameras
 */

/* List the units with a free camera. None may be held open while looking. A
   failure is none here: a unit without a camera is normal. */
static void scanCameras(struct Gui *gui)
{
    struct Choice *ch = &gui->gu_Camera;
    UBYTE ignored[UVC_TEXT_SIZE];

    choiceClear(ch);
    uvcReportTo = ignored;
    for(ULONG unit = 0; unit < MAX_UNITS; unit++)
    {
        struct UvcCamera cm;

        if(uvcOpenCamera(&cm, unit))
        {
            choiceAdd(ch, unit, "Unit %lu: %s", unit, (const char *) cm.cm_Info.uvi_ProductName);
        }
        uvcCloseCamera(&cm);
    }
    uvcReportTo = NULL;
}

/* Let the camera go, if one is open */
static void closeCamera(struct Gui *gui)
{
    if(gui->gu_Stream)
    {
        uvpClose(gui->gu_Stream);
        gui->gu_Stream = NULL;
    }
}

/* Open the camera chosen in the list; its modes fill the lists. Say what it
   is, or why not. */
static void openCamera(struct Gui *gui)
{
    UBYTE why[UVC_TEXT_SIZE] = "";
    UBYTE info[LINE_SIZE] = "";

    closeCamera(gui);
    if(!gui->gu_Camera.ch_Count)
    {
        strcpy((char *) why, "No free camera. Plug one in, then Rescan.");
    }
    else if((gui->gu_Stream = uvpOpen(choiceValue(&gui->gu_Camera), gui->gu_StopMask, gui->gu_TickMask, why)))
    {
        const struct UsbVideoInfo *uvi = &uvpCamera(gui->gu_Stream)->cm_Info;

        snprintf((char *) info, sizeof(info), "%s, %04lx:%04lx, UVC %lx.%02lx, %s",
                 uvi->uvi_Manufacturer[0] ? (const char *) uvi->uvi_Manufacturer : "maker not known",
                 (ULONG) uvi->uvi_VendorID, (ULONG) uvi->uvi_ProductID,
                 (ULONG) uvi->uvi_Version >> 8, (ULONG) uvi->uvi_Version & 0xff,
                 (uvi->uvi_Flags & UVIF_BULK) ? "bulk" : "isochronous");
        why[0] = 0;
    }
    set(gui->gu_Info, MUIA_Text_Contents, (IPTR) info);
    set(gui->gu_Why, MUIA_Text_Contents, (IPTR) why);
    fillFormats(gui);
}

/* Look for cameras, and open the one of that unit if it is there. The one
   held is let go for that. */
static void rescan(struct Gui *gui, ULONG unit)
{
    closeCamera(gui);
    scanCameras(gui);
    choiceShow(gui, &gui->gu_Camera, choiceFind(&gui->gu_Camera, unit));
    openCamera(gui);
}

/*
 * The stream
 */

/* The number in a string gadget */
static ULONG stringNumber(Object *obj)
{
    IPTR value = 0;

    get(obj, MUIA_String_Integer, &value);
    return value;
}

/* Start the stream in the mode chosen */
static void start(struct Gui *gui)
{
    const struct UsbVideoMode *uvm = chosenMode(gui);
    BOOL h264 = uvm->uvm_FourCC == UVFCC_H264;
    struct UsbVideoModeReq uvq =
    {
        .uvq_FourCC = uvm->uvm_FourCC,
        .uvq_Width = uvm->uvm_Width,
        .uvq_Height = uvm->uvm_Height,
        .uvq_Interval = choiceValue(&gui->gu_Rate),
        .uvq_BitRate = h264 ? stringNumber(gui->gu_BitRate) * 1000 : 0,
        .uvq_KeyInterval = h264 ? stringNumber(gui->gu_KeyInterval) : 0
    };
    STRPTR stem = NULL;
    struct Window *win = NULL;

    get(gui->gu_Window, MUIA_Window_Window, &win);

    struct IBox box = { win->LeftEdge, win->TopEdge, win->Width, win->Height };

    uvpKeepClear(gui->gu_Stream, &box);
    get(gui->gu_Stem, MUIA_String_Contents, &stem);
    memset(&gui->gu_Last, 0, sizeof(gui->gu_Last));
    CurrentTime(&gui->gu_LastTime.tv_secs, &gui->gu_LastTime.tv_micro);
    uvpStart(gui->gu_Stream, &uvq, stem);
    set(gui->gu_Status, MUIA_Text_Contents, (IPTR) "");
}

/*
 * The window's making
 */

/* A button of text, as the class's windows make them */
static Object *button(const char *text, ULONG mode)
{
    return TextObject, ButtonFrame,
        MUIA_Background, MUII_ButtonBack,
        MUIA_CycleChain, 1,
        MUIA_InputMode, mode,
        MUIA_Text_PreParse, (IPTR) "\33c",
        MUIA_Text_Contents, (IPTR) text,
        End;
}

/* Tell the application when a button is let go */
static void notifyPressed(struct Gui *gui, Object *obj, ULONG id)
{
    DoMethod(obj, MUIM_Notify, MUIA_Pressed, FALSE, gui->gu_App, 2, MUIM_Application_ReturnID, id);
}

/* A group for a Choice, with a first gadget in it */
static Object *choiceGroup(struct Choice *ch, ULONG id)
{
    choiceClear(ch);
    ch->ch_Id = id;
    ch->ch_Cycle = CycleObject,
        MUIA_CycleChain, 1,
        MUIA_Cycle_Entries, (IPTR) ch->ch_Names,
        End;
    ch->ch_Group = HGroup,
        MUIA_Group_HorizSpacing, 0,
        Child, (IPTR) ch->ch_Cycle,
        End;
    return ch->ch_Group;
}

/* A string gadget for a number, with its help text */
static Object *numberGadget(ULONG value, const char *help)
{
    return StringObject, StringFrame,
        MUIA_CycleChain, 1,
        MUIA_ShortHelp, (IPTR) help,
        MUIA_String_Accept, (IPTR) "0123456789",
        MUIA_String_MaxLen, 12,
        MUIA_String_Integer, value,
        End;
}

/* Make the application; FALSE if it could not be */
static BOOL makeWindow(struct Gui *gui, CONST_STRPTR version)
{
    gui->gu_App = ApplicationObject,
        MUIA_Application_Title, (IPTR) "USBVideo",
        MUIA_Application_Version, (IPTR) version,
        MUIA_Application_Description, (IPTR) "Shows and saves what a USB camera sees",
        MUIA_Application_Base, (IPTR) "USBVIDEO",

        SubWindow, (IPTR) (gui->gu_Window = WindowObject,
            MUIA_Window_ID, MAKE_ID('M','A','I','N'),
            MUIA_Window_Title, (IPTR) "USBVideo",
            MUIA_Window_LeftEdge, 0, /* leaves the preview the room beside it */
            MUIA_Window_TopEdge, MUIV_Window_TopEdge_Delta(0),

            WindowContents, (IPTR) VGroup,
                Child, (IPTR) ColGroup(2),
                    Child, (IPTR) Label("Camera"),
                    Child, (IPTR) HGroup,
                        Child, (IPTR) choiceGroup(&gui->gu_Camera, ID_CAMERA),
                        Child, (IPTR) (gui->gu_Rescan = button("Rescan", MUIV_InputMode_RelVerify)),
                        End,
                    Child, (IPTR) HSpace(0),
                    Child, (IPTR) (gui->gu_Info = TextObject,
                        MUIA_Text_Contents, (IPTR) "",
                        End),
                    Child, (IPTR) Label("Format"),
                    Child, (IPTR) HGroup,
                        Child, (IPTR) choiceGroup(&gui->gu_Format, ID_FORMAT),
                        Child, (IPTR) Label("Size"),
                        Child, (IPTR) choiceGroup(&gui->gu_Size, ID_SIZE),
                        Child, (IPTR) Label("Rate"),
                        Child, (IPTR) choiceGroup(&gui->gu_Rate, 0),
                        End,
                    Child, (IPTR) Label("Bit rate"),
                    Child, (IPTR) HGroup,
                        Child, (IPTR) (gui->gu_BitRate = numberGadget(0, "H.264: kbit/s, 0 leaves it to the camera")),
                        Child, (IPTR) Label("Key frame every"),
                        Child, (IPTR) (gui->gu_KeyInterval = numberGadget(UVP_KEY_INTERVAL, "H.264: ms between key frames. After a lost frame the picture needs one")),
                        Child, (IPTR) Label("ms"),
                        End,
                    Child, (IPTR) Label("Save to"),
                    Child, (IPTR) (gui->gu_StemPop = PopaslObject,
                        MUIA_CycleChain, 1,
                        MUIA_ShortHelp, (IPTR) "Files take this name and a number, an H.264 stream this name and .h264",
                        MUIA_Popstring_String, (IPTR) (gui->gu_Stem = StringObject, StringFrame,
                            MUIA_CycleChain, 1,
                            MUIA_String_MaxLen, STEM_SIZE,
                            MUIA_String_Contents, (IPTR) DEFAULT_STEM,
                            End),
                        MUIA_Popstring_Button, (IPTR) PopButton(MUII_PopFile),
                        ASLFR_TitleText, (IPTR) "Save frames as",
                        ASLFR_DoSaveMode, TRUE,
                        End),
                    End,
                Child, (IPTR) HGroup,
                    MUIA_Group_SameWidth, TRUE,
                    Child, (IPTR) (gui->gu_Start = button("Start", MUIV_InputMode_RelVerify)),
                    Child, (IPTR) (gui->gu_Stop = button("Stop", MUIV_InputMode_RelVerify)),
                    Child, (IPTR) (gui->gu_Snapshot = button("Snapshot", MUIV_InputMode_RelVerify)),
                    Child, (IPTR) (gui->gu_Record = button("Record", MUIV_InputMode_Toggle)),
                    Child, (IPTR) (gui->gu_Settings = button("Camera settings...", MUIV_InputMode_RelVerify)),
                    End,
                Child, (IPTR) (gui->gu_Status = TextObject, TextFrame,
                    MUIA_Background, MUII_TextBack,
                    MUIA_Text_Contents, (IPTR) "",
                    End),
                Child, (IPTR) (gui->gu_Why = TextObject,
                    MUIA_Text_Contents, (IPTR) "",
                    End),
                End,
            End),
        End;

    return gui->gu_App != NULL;
}

/* Wire the gadgets to the application */
static void connectWindow(struct Gui *gui)
{
    DoMethod(gui->gu_Window, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             gui->gu_App, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    notifyPressed(gui, gui->gu_Rescan, ID_RESCAN);
    notifyPressed(gui, gui->gu_Start, ID_START);
    notifyPressed(gui, gui->gu_Stop, ID_STOP);
    notifyPressed(gui, gui->gu_Snapshot, ID_SNAPSHOT);
    notifyPressed(gui, gui->gu_Settings, ID_SETTINGS);
    DoMethod(gui->gu_Record, MUIM_Notify, MUIA_Selected, MUIV_EveryTime,
             gui->gu_App, 2, MUIM_Application_ReturnID, ID_RECORD);
}

/* Do what a gadget asked for */
static void obey(struct Gui *gui, LONG id)
{
    IPTR on = 0;

    switch(id)
    {
        case ID_CAMERA:
            openCamera(gui);
            break;

        case ID_RESCAN:
            rescan(gui, choiceValue(&gui->gu_Camera));
            break;

        case ID_FORMAT:
            fillSizes(gui);
            break;

        case ID_SIZE:
            fillRates(gui, chosenMode(gui));
            break;

        case ID_START:
            start(gui);
            break;

        case ID_STOP:
            uvpStop(gui->gu_Stream);
            break;

        case ID_SNAPSHOT:
            uvpSnapshot(gui->gu_Stream);
            break;

        case ID_RECORD:
            get(gui->gu_Record, MUIA_Selected, &on);
            if(!uvpRecord(gui->gu_Stream, on))
            {
                nnset(gui->gu_Record, MUIA_Selected, FALSE);
            }
            break;

        case ID_SETTINGS:
            uvpSettings(gui->gu_Stream);
            break;

        default:
            return;
    }
    showState(gui);
}

/* Run the application until it quits */
static void runWindow(struct Gui *gui)
{
    ULONG sigs = 0;

    for(;;)
    {
        LONG id = DoMethod(gui->gu_App, MUIM_Application_NewInput, &sigs);

        if(id == MUIV_Application_ReturnID_Quit)
        {
            return;
        }
        obey(gui, id);
        if(sigs)
        {
            sigs = Wait(sigs | gui->gu_StopMask | gui->gu_TickMask | SIGBREAKF_CTRL_C);
            if(sigs & SIGBREAKF_CTRL_C)
            {
                return;
            }
            if(gui->gu_Stream && (sigs & gui->gu_StopMask))
            {
                showState(gui);
            }
            if(gui->gu_Stream && (sigs & gui->gu_TickMask))
            {
                showCounts(gui);
            }
        }
    }
}

/* Say that there is no MUI, where a Workbench program can */
static void noMUI(void)
{
    struct EasyStruct es =
    {
        sizeof(es), 0, (STRPTR) "USBVideo", (STRPTR) "The USBVideo window needs MUI (" MUIMASTER_NAME ").", (STRPTR) "OK"
    };

    EasyRequestArgs(NULL, &es, NULL, NULL);
}

LONG usbVideoGui(ULONG unit, CONST_STRPTR version)
{
    LONG stopsig = AllocSignal(-1);
    LONG ticksig = AllocSignal(-1);
    struct Gui *gui = AllocVec(sizeof(*gui), MEMF_PUBLIC | MEMF_CLEAR);
    LONG rc = RETURN_FAIL;

    if(!(MUIMasterBase = OpenLibrary(MUIMASTER_NAME, MUIMASTER_VMIN)))
    {
        noMUI();
    }
    else if(gui && (stopsig >= 0) && (ticksig >= 0) && makeWindow(gui, version))
    {
        IPTR isopen = 0;

        gui->gu_StopMask = 1UL << stopsig;
        gui->gu_TickMask = 1UL << ticksig;
        connectWindow(gui);
        rescan(gui, unit);
        showState(gui);
        set(gui->gu_Window, MUIA_Window_Open, TRUE);
        get(gui->gu_Window, MUIA_Window_Open, &isopen);
        if(isopen)
        {
            runWindow(gui);
            rc = RETURN_OK;
        }
        closeCamera(gui);
        set(gui->gu_Window, MUIA_Window_Open, FALSE);
    }
    if(gui && gui->gu_App)
    {
        MUI_DisposeObject(gui->gu_App);
    }
    CloseLibrary(MUIMasterBase);
    MUIMasterBase = NULL;
    FreeVec(gui);
    FreeSignal(ticksig);
    FreeSignal(stopsig);
    return rc;
}
