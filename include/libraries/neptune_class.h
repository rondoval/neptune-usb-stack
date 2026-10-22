/*
 *----------------------------------------------------------------------------
 *                  Neptune USB class-driver protocol
 *----------------------------------------------------------------------------
 * The attributes and methods neptune.library and its class drivers exchange
 * through usbGetAttrsA/usbSetAttrsA/usbDoMethodA (<proto/neptune_class.h>).
 * Derived from Poseidon's usbclass.h by Chris Hodges <hodges@in.tum.de>, but
 * Neptune's protocol, not Poseidon's: a class built against one does not run
 * on the other.
 */

#ifndef LIBRARIES_NEPTUNE_CLASS_H
#define LIBRARIES_NEPTUNE_CLASS_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <utility/pack.h>

/* Types for usbGetAttrs() and usbSetAttrs() */

#define UGA_CLASS      0x01
#define UGA_BINDING    0x02
#define UGA_CONFIG     0x03

/* Tags for usbGetAttrs(UGA_CLASS,...) */

#define UCCA_Dummy           (TAG_USER + 4489)
#define UCCA_Priority        (UCCA_Dummy + 0x01)
#define UCCA_Description     (UCCA_Dummy + 0x02)
#define UCCA_HasClassCfgGUI  (UCCA_Dummy + 0x10)
#define UCCA_HasBindingCfgGUI (UCCA_Dummy + 0x11)
#define UCCA_AfterDOSRestart (UCCA_Dummy + 0x20)
#define UCCA_UsingDefaultCfg (UCCA_Dummy + 0x30)
#define UCCA_SupportsSuspend (UCCA_Dummy + 0x40)
#define UCCA_SupportsSafeEject (UCCA_Dummy + 0x41) /* implements UCM_SafeEject */

/* Tags for usbGetAttrs(UGA_BINDING,...) */

#define UCBA_Dummy           (TAG_USER  + 103)
#define UCBA_UsingDefaultCfg (UCBA_Dummy + 0x30)

/* Tags for usbGetAttrs(UGA_CONFIG,...) */

#define UCFA_Dummy          (TAG_USER  + 2612)

/* Methods for usbDoMethod() */

#define UCM_AttemptInterfaceBinding 0x0001
#define UCM_ForceInterfaceBinding   0x0002
#define UCM_ReleaseInterfaceBinding 0x0003
#define UCM_AttemptDeviceBinding    0x0004
#define UCM_ForceDeviceBinding      0x0005
#define UCM_ReleaseDeviceBinding    0x0006
#define UCM_OpenCfgWindow           0x0020
#define UCM_CloseCfgWindow          0x0021
#define UCM_OpenBindingCfgWindow    0x0022 /* { binding } */
#define UCM_CloseBindingCfgWindow   0x0023 /* { binding } */
#define UCM_LocaleAvailableEvent    0x0030
#define UCM_DOSAvailableEvent       0x0031
#define UCM_ConfigChangedEvent      0x0032
#define UCM_SoftRestart             0x0040
#define UCM_HardRestart             0x0041
#define UCM_AttemptSuspendDevice    0x0050 /* success = { binding } */
#define UCM_AttemptResumeDevice     0x0051 /* success = { binding } */
#define UCM_SafeEject               0x0052 /* SAFEEJECT_* = { binding, STRPTR busybuf,
                                              ULONG busybufsize } - device-scoped: quiesce
                                              everything this class holds on the binding's
                                              device (for storage: flush + verified unmount
                                              of every volume, then stop the drives) so it
                                              can be unplugged.  All-or-nothing: refuse with
                                              SAFEEJECT_BUSY, naming the object in busybuf,
                                              rather than losing data, and undo whatever was
                                              already done.  Called on a Process, never with
                                              a device lock held - it may block for seconds.
                                              Advertise UCCA_SupportsSafeEject to be asked.
                                              Reached through psdSafeEjectDevice(), which
                                              calls every capable class on the device and
                                              disables its hub port afterwards. */

#define UCM_MediaPending            0x0053 /* count = { } - class-scoped, no arguments: how
                                              many of this class's bound units are still
                                              bringing a medium up, i.e. a mount is expected
                                              but has not happened yet.  A drive that reports
                                              no medium at all (an empty CD tray, a card
                                              reader with no card) is *not* pending and must
                                              not be counted, or a boot gate waiting on this
                                              would pay the full timeout on every boot.
                                              Called from the Kickstart ROM startup resident,
                                              pre-DOS, in a poll loop - it must not block.
                                              0 = nothing pending, which is also what a class
                                              that does not implement this returns. */
#define UCM_PortsPending            0x0054 /* count = { } - class-scoped, no arguments: how
                                              many of this class's hubs have not finished a
                                              port pass - the initial scan after binding, or
                                              a port change, power cycle or class scan still
                                              being worked.  A hub in its power-good wait, or
                                              between seeing a connection and enumerating it,
                                              has nothing in the device list yet, so a boot
                                              gate cannot see it any other way.  Same rules
                                              as UCM_MediaPending: polled pre-DOS from the
                                              Kickstart ROM startup resident, must not block,
                                              0 = nothing pending / not implemented. */

/* UCM_SafeEject / psdSafeEjectDevice() results. 0 is deliberately reserved: a class's
   usbDoMethodA() default arm returns 0 for methods it does not know, so 0 must always
   read as "not supported". */
#define SAFEEJECT_NOT_SUPPORTED 0
#define SAFEEJECT_OK            1 /* quiesced - safe to remove */
#define SAFEEJECT_BUSY          2 /* still in use; busybuf names it; nothing changed */
#define SAFEEJECT_FAIL          3 /* wrong context / no resources */

/* only for hubs */
#define UCM_HubPowerCyclePort       0x0f01 /* { device, portnumber } */
#define UCM_HubClassScan            0x0f02 /* { hubbinding } */
#define UCM_HubClaimAppBinding      0x0f03 /* { hubbinding, taglist } */
#define UCM_HubReleaseIfBinding     0x0f04 /* { hubbinding, if } */
#define UCM_HubReleaseDevBinding    0x0f05 /* { hubbinding, device } */
#define UCM_HubDisablePort          0x0f06 /* { device, portnumber } */
#define UCM_HubSuspendDevice        0x0f07 /* { hubbinding, device } */
#define UCM_HubResumeDevice         0x0f08 /* { hubbinding, device } */
#define UCM_HubResetPort            0x0f09 /* { hubbinding, device } - hot-reset the device's port;
                                              no re-enumeration, no device teardown */

#endif /* LIBRARIES_NEPTUNE_CLASS_H */
