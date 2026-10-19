#include "debug.h"

#include "cdceth.class.h"
#include "cdceth_encap.h"

#include <hardware/cdc/cdc_eem.h>
#include <hardware/cdc/cdc_ncm.h>

extern const STRPTR libname;

#define CDCETH_EEM_DEFAULT_BUFSIZE  4096
#define CDCETH_EEM_HEADER_LEN       2
#define CDCETH_EEM_ALIGN            4

#define CDCETH_NCM_ALIGN            4       /* NCM minimum alignment, the fallback */
#define CDCETH_NCM_DEFAULT_NTB      16384
#define CDCETH_NCM_NTB16_MAX        0xffff  /* wBlockLength is 16 bits */
#define CDCETH_NCM_NDP_LEN          (sizeof(struct UsbCDCNcmNdp16) + \
                                     2 * sizeof(struct UsbCDCNcmDatagramPointer16))

static ULONG cdceth_align(ULONG val, ULONG align)
{
    return (val + (align - 1)) & ~(align - 1);
}

/* Alternate setting 0 of the data interface in the selected configuration. */
static struct PsdInterface *cdceth_data_alt0(struct NepClassEth *ncp)
{
    struct Library *ps = ncp->ncp_Base;
    struct List *ifs = NULL;
    struct PsdInterface *pif;
    IPTR datanum = 0;

    psdGetAttrs(PGA_INTERFACE, ncp->ncp_Interface,
                IFA_InterfaceNum, &datanum,
                TAG_END);
    psdGetAttrs(PGA_CONFIG, ncp->ncp_Config,
                CA_InterfaceList, &ifs,
                TAG_END);

    for(pif = ifs ? (struct PsdInterface *) ifs->lh_Head : NULL;
        pif && pif->pif_Node.ln_Succ;
        pif = (struct PsdInterface *) pif->pif_Node.ln_Succ)
    {
        struct PsdInterface *altpif;
        struct List *altlist = NULL;
        IPTR ifnum = 0;
        IPTR altnum = 0;

        psdGetAttrs(PGA_INTERFACE, pif,
                    IFA_InterfaceNum, &ifnum,
                    IFA_AlternateNum, &altnum,
                    IFA_AlternateIfList, &altlist,
                    TAG_END);
        if(ifnum != datanum)
        {
            continue;
        }
        if(altnum == 0)
        {
            return(pif);
        }
        for(altpif = altlist ? (struct PsdInterface *) altlist->lh_Head : NULL;
            altpif && altpif->pif_Node.ln_Succ;
            altpif = (struct PsdInterface *) altpif->pif_Node.ln_Succ)
        {
            psdGetAttrs(PGA_INTERFACE, altpif,
                        IFA_AlternateNum, &altnum,
                        TAG_END);
            if(altnum == 0)
            {
                return(altpif);
            }
        }
        break;
    }
    return(NULL);
}

/* An OUT alignment modulus from the NTB parameters: a power of two, at least
   4 and below the NTB size, else 4 (as Linux cdc_ncm_fix_modulus). */
static UWORD cdceth_ncm_modulus(UWORD val, ULONG ntb_size)
{
    if((val < CDCETH_NCM_ALIGN) || (val & (val - 1)) || (val >= ntb_size))
    {
        return(CDCETH_NCM_ALIGN);
    }
    return(val);
}

/* Brings an NCM function to a known state and reads its NTB rules (NCM 1.0
   7.2: alternate 0, then the operational parameters, then the operating
   alternate - selected later by the caller). */
static BOOL cdceth_ncm_configure(struct NepClassEth *ncp)
{
    struct Library *ps = ncp->ncp_Base;
    struct PsdInterface *alt0 = cdceth_data_alt0(ncp);
    struct PsdDescriptor *pdd;
    struct UsbCDCNcmFunctionalDesc *ncm = NULL;
    struct UsbCDCNcmNtbParameters parm;
    ULONG inmax;
    ULONG insize;
    LONG ioerr;

    /* a no-op when alternate 0 is active (fresh enumeration, config just
       switched); on a re-bind it resets the function's NCM state */
    if(!(alt0 && psdSetAltInterface(ncp->ncp_EP0Pipe, alt0)))
    {
        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                       "Could not reset the NCM data interface!");
        return(FALSE);
    }

    pdd = psdFindDescriptor(ncp->ncp_Device, NULL,
                            DDA_Interface, ncp->ncp_ControlInterface,
                            DDA_DescriptorType, UDT_CS_INTERFACE,
                            DDA_CS_SubType, CDC_NCM_FUNC_DESC_SUBTYPE,
                            TAG_END);
    if(pdd)
    {
        psdGetAttrs(PGA_DESCRIPTOR, pdd,
                    DDA_DescriptorData, &ncm,
                    TAG_END);
    }
    /* without D0 the function passes all traffic and stalls the request */
    ncp->ncp_FilterSupported = (ncm && (ncm->bmNetworkCapabilities & CDC_NCM_NCAP_ETH_FILTER)) ? TRUE : FALSE;

    /* a short reply leaves the missing fields zero */
    memset(&parm, 0, sizeof(parm));
    ioerr = nCDCRequest(ncp, URTF_IN, CDC_NCM_GET_NTB_PARAMETERS, 0, &parm, sizeof(parm));
    if(ioerr)
    {
        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                       "GET_NTB_PARAMETERS failed: %s (%ld)",
                       psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr);
        return(FALSE);
    }
    inmax = AROS_LE2LONG(parm.dwNtbInMaxSize);
    if(!inmax)
    {
        psdAddErrorMsg(RETURN_FAIL, (STRPTR) libname,
                       "NCM function reports no IN NTB size!");
        return(FALSE);
    }

    /* The read must be exactly the IN NTB size: an NTB of that size that is a
       multiple of wMaxPacketSize ends without a ZLP (NCM 1.0 Table 3-1). */
    insize = max(min(inmax, CDCETH_NCM_DEFAULT_NTB), CDC_NCM_NTB_MIN_IN_SIZE);
    if(insize != inmax)
    {
        ULONG le_insize = AROS_LONG2LE(insize);

        ioerr = nCDCRequest(ncp, URTF_OUT, CDC_NCM_SET_NTB_INPUT_SIZE, 0, &le_insize, sizeof(le_insize));
        if(ioerr)
        {
            /* the function keeps the size it reported */
            insize = min(inmax, CDCETH_NCM_NTB16_MAX);
            psdAddErrorMsg(RETURN_WARN, (STRPTR) libname,
                           "SET_NTB_INPUT_SIZE failed: %s (%ld); reading %ld byte blocks.",
                           psdNumToStr(NTS_IOERR, ioerr, "unknown"), ioerr, (LONG) insize);
        }
    }
    ncp->ncp_ReadBufSize = insize;
    ncp->ncp_WriteBufSize = CDCETH_NCM_DEFAULT_NTB;

    ncp->ncp_NcmNdpAlign = cdceth_ncm_modulus(AROS_LE2WORD(parm.wNdpOutAlignment), ncp->ncp_WriteBufSize);
    ncp->ncp_NcmDivisor = cdceth_ncm_modulus(AROS_LE2WORD(parm.wNdpOutDivisor), ncp->ncp_WriteBufSize);
    ncp->ncp_NcmRemainder = AROS_LE2WORD(parm.wNdpOutPayloadRemainder);
    if(ncp->ncp_NcmRemainder >= ncp->ncp_NcmDivisor)
    {
        ncp->ncp_NcmRemainder = 0;
    }

    KPRINTF(5, ("NCM NTB: in max %lu -> %lu, out max %lu, out ndp align %lu div %lu rem %lu, filter %ld\n",
                inmax, insize, AROS_LE2LONG(parm.dwNtbOutMaxSize),
                (ULONG) ncp->ncp_NcmNdpAlign, (ULONG) ncp->ncp_NcmDivisor,
                (ULONG) ncp->ncp_NcmRemainder, (LONG) ncp->ncp_FilterSupported));
    return(TRUE);
}

BOOL cdceth_encap_configure(struct NepClassEth *ncp)
{
    struct Library *ps = ncp->ncp_Base;
    IPTR subcls = CDC_SUBCLASS_ETHERNET;

    KPRINTF(5, ("ncp =  0x%08lx, ps = 0x%08lx\n", ncp, ps));

    ncp->ncp_EncapType = CDCETH_ENCAP_ECM;
    ncp->ncp_ReadBufSize = ETHER_MAX_LEN;
    ncp->ncp_WriteBufSize = ETHER_MAX_LEN;
    ncp->ncp_FilterSupported = TRUE;
    ncp->ncp_NcmSeq = 0;

    if(ncp->ncp_ControlInterface)
    {
        psdGetAttrs(PGA_INTERFACE, ncp->ncp_ControlInterface,
                    IFA_SubClass, &subcls,
                    TAG_END);
    }

    KPRINTF(5, ("subclass %lu\n", subcls));
    switch(subcls)
    {
        case CDC_SUBCLASS_EEM:
            ncp->ncp_EncapType = CDCETH_ENCAP_EEM;
            ncp->ncp_ReadBufSize = CDCETH_EEM_DEFAULT_BUFSIZE;
            ncp->ncp_WriteBufSize = CDCETH_EEM_DEFAULT_BUFSIZE;
            break;

        case CDC_SUBCLASS_NCM:
            ncp->ncp_EncapType = CDCETH_ENCAP_NCM;
            if(!cdceth_ncm_configure(ncp))
            {
                return(FALSE);
            }
            break;

        case CDC_SUBCLASS_ETHERNET:
        default:
            ncp->ncp_EncapType = CDCETH_ENCAP_ECM;
            break;
    }

    KPRINTF(5, ("CDC encapsulation configured type %ld rx %lu tx %lu\n",
                (LONG) ncp->ncp_EncapType,
                ncp->ncp_ReadBufSize,
                ncp->ncp_WriteBufSize));

    return(TRUE);
}

/* NTB-16 layout for one datagram: the NTH, the NDP at the device's NDP
   alignment, then the frame. */
static ULONG cdceth_ncm_ndp_offset(struct NepClassEth *ncp)
{
    return cdceth_align(sizeof(struct UsbCDCNcmNtb16Header), ncp->ncp_NcmNdpAlign);
}

/* Frame offset after the NDP such that the payload - the data after the
   Ethernet header - meets offset % divisor == remainder (NCM 1.0 3.3.4). */
static ULONG cdceth_ncm_frame_offset(struct NepClassEth *ncp)
{
    ULONG ndp_end = cdceth_ncm_ndp_offset(ncp) + CDCETH_NCM_NDP_LEN;

    return ndp_end + ((ncp->ncp_NcmRemainder - sizeof(struct EtherPacketHeader) - ndp_end) &
                      (ncp->ncp_NcmDivisor - 1));
}

BOOL cdceth_prepare_tx(struct NepClassEth *ncp, ULONG payload_len, ULONG buf_size,
                       ULONG *payload_offset, ULONG *total_len)
{
    ULONG offset = 0;
    ULONG total = 0;

    if(!payload_offset || !total_len)
    {
        return(FALSE);
    }

    switch(ncp->ncp_EncapType)
    {
        case CDCETH_ENCAP_EEM:
        {
            if(payload_len > CDC_EEM_HDR_LEN_MASK)
            {
                return(FALSE);
            }
            offset = CDCETH_EEM_HEADER_LEN;
            total = cdceth_align(offset + payload_len, CDCETH_EEM_ALIGN);
            break;
        }

        case CDCETH_ENCAP_NCM:
            offset = cdceth_ncm_frame_offset(ncp);
            total = offset + payload_len;
            break;

        case CDCETH_ENCAP_ECM:
        default:
            offset = 0;
            total = payload_len;
            break;
    }

    if((payload_len > 0) && (total > buf_size))
    {
        return(FALSE);
    }

    *payload_offset = offset;
    *total_len = total;
    return(TRUE);
}

void cdceth_finalize_tx(struct NepClassEth *ncp, UBYTE *buf, ULONG payload_offset,
                        ULONG payload_len, ULONG total_len)
{
    switch(ncp->ncp_EncapType)
    {
        case CDCETH_ENCAP_EEM:
        {
            UWORD header = (UWORD) (payload_len & CDC_EEM_HDR_LEN_MASK);
            UWORD *hdr = (UWORD *) buf;

            *hdr = AROS_WORD2LE(header);
            if(total_len > payload_offset + payload_len)
            {
                memset(buf + payload_offset + payload_len, 0,
                       total_len - (payload_offset + payload_len));
            }
            break;
        }

        case CDCETH_ENCAP_NCM:
        {
            ULONG ndp_offset = cdceth_ncm_ndp_offset(ncp);
            struct UsbCDCNcmNtb16Header *nth = (struct UsbCDCNcmNtb16Header *) buf;
            struct UsbCDCNcmNdp16 *ndp = (struct UsbCDCNcmNdp16 *) (buf + ndp_offset);
            struct UsbCDCNcmDatagramPointer16 *dpe =
                (struct UsbCDCNcmDatagramPointer16 *) ((UBYTE *) ndp + sizeof(*ndp));

            memset(buf, 0, payload_offset);

            nth->dwSignature = AROS_LONG2LE(CDC_NCM_NTH16_SIGNATURE);
            nth->wHeaderLength = AROS_WORD2LE((UWORD) sizeof(*nth));
            nth->wSequence = AROS_WORD2LE(ncp->ncp_NcmSeq++);
            nth->wBlockLength = AROS_WORD2LE((UWORD) total_len);
            nth->wNdpIndex = AROS_WORD2LE((UWORD) ndp_offset);

            ndp->dwSignature = AROS_LONG2LE(CDC_NCM_NDP16_SIGNATURE_NCM0);
            ndp->wLength = AROS_WORD2LE((UWORD) CDCETH_NCM_NDP_LEN);
            ndp->wNextNdpIndex = AROS_WORD2LE(0);

            dpe[0].wDatagramIndex = AROS_WORD2LE((UWORD) payload_offset);
            dpe[0].wDatagramLength = AROS_WORD2LE((UWORD) payload_len);
            dpe[1].wDatagramIndex = 0;
            dpe[1].wDatagramLength = 0;
            break;
        }

        case CDCETH_ENCAP_ECM:
        default:
            break;
    }
}

static BOOL cdceth_handle_eem_rx(struct NepClassEth *ncp, UBYTE *pktptr, ULONG pktlen)
{
    ULONG offset = 0;
    BOOL delivered = FALSE;

    while(offset + sizeof(UWORD) <= pktlen)
    {
        UWORD header = AROS_LE2WORD(*(UWORD *) (pktptr + offset));
        UWORD length = (UWORD) (header & CDC_EEM_HDR_LEN_MASK);
        BOOL is_command = (header & CDC_EEM_HDR_TYPE_COMMAND) != 0;
        BOOL has_crc = (header & CDC_EEM_HDR_CRC) != 0;

        offset += sizeof(UWORD);

        if(length == 0)
        {
            offset = cdceth_align(offset, CDCETH_EEM_ALIGN);
            continue;
        }

        if(offset + length > pktlen)
        {
            ncp->ncp_DeviceStats.BadData++;
            break;
        }

        if(!is_command)
        {
            ULONG frame_len = length;
            if(has_crc && frame_len >= 4)
            {
                frame_len -= 4;
            }
            if(frame_len >= sizeof(struct EtherPacketHeader))
            {
                delivered |= nReadPacket(ncp, pktptr + offset, frame_len);
            }
            else
            {
                ncp->ncp_DeviceStats.BadData++;
            }
        }

        offset += length;
        offset = cdceth_align(offset, CDCETH_EEM_ALIGN);
    }

    return delivered;
}

static BOOL cdceth_handle_ncm_rx(struct NepClassEth *ncp, UBYTE *pktptr, ULONG pktlen)
{
    const struct UsbCDCNcmNtb16Header *nth;
    ULONG block_len;
    ULONG ndp_index;
    BOOL delivered = FALSE;
    int guard = 0;

    if(pktlen < sizeof(*nth))
    {
        ncp->ncp_DeviceStats.BadData++;
        return(FALSE);
    }

    nth = (const struct UsbCDCNcmNtb16Header *) pktptr;
    if(AROS_LE2LONG(nth->dwSignature) != CDC_NCM_NTH16_SIGNATURE)
    {
        ncp->ncp_DeviceStats.BadData++;
        return(FALSE);
    }

    block_len = AROS_LE2WORD(nth->wBlockLength);
    if(block_len == 0 || block_len > pktlen)
    {
        block_len = pktlen;
    }

    ndp_index = AROS_LE2WORD(nth->wNdpIndex);

    while(ndp_index && guard++ < 4)
    {
        const struct UsbCDCNcmNdp16 *ndp;
        ULONG ndp_length;
        ULONG ndp_end;
        ULONG entry_offset;
        ULONG signature;

        if(ndp_index + sizeof(*ndp) > block_len)
        {
            ncp->ncp_DeviceStats.BadData++;
            break;
        }

        ndp = (const struct UsbCDCNcmNdp16 *) (pktptr + ndp_index);
        signature = AROS_LE2LONG(ndp->dwSignature);
        if(signature != CDC_NCM_NDP16_SIGNATURE_NCM0 &&
           signature != CDC_NCM_NDP16_SIGNATURE_NCM1)
        {
            ncp->ncp_DeviceStats.BadData++;
            break;
        }

        ndp_length = AROS_LE2WORD(ndp->wLength);
        if(ndp_length < sizeof(*ndp))
        {
            ncp->ncp_DeviceStats.BadData++;
            break;
        }

        ndp_end = ndp_index + ndp_length;
        if(ndp_end > block_len)
        {
            ncp->ncp_DeviceStats.BadData++;
            break;
        }

        entry_offset = ndp_index + sizeof(*ndp);
        while(entry_offset + sizeof(struct UsbCDCNcmDatagramPointer16) <= ndp_end)
        {
            const struct UsbCDCNcmDatagramPointer16 *entry;
            ULONG d_index;
            ULONG d_len;

            entry = (const struct UsbCDCNcmDatagramPointer16 *) (pktptr + entry_offset);
            d_index = AROS_LE2WORD(entry->wDatagramIndex);
            d_len = AROS_LE2WORD(entry->wDatagramLength);

            if(d_index == 0 || d_len == 0)
            {
                break;
            }

            if(d_index + d_len > block_len)
            {
                ncp->ncp_DeviceStats.BadData++;
                break;
            }

            if(d_len >= sizeof(struct EtherPacketHeader))
            {
                delivered |= nReadPacket(ncp, pktptr + d_index, d_len);
            }
            else
            {
                ncp->ncp_DeviceStats.BadData++;
            }

            entry_offset += sizeof(struct UsbCDCNcmDatagramPointer16);
        }

        ndp_index = AROS_LE2WORD(ndp->wNextNdpIndex);
    }

    return delivered;
}

BOOL cdceth_handle_rx(struct NepClassEth *ncp, UBYTE *pktptr, ULONG pktlen)
{
    switch(ncp->ncp_EncapType)
    {
        case CDCETH_ENCAP_EEM:
            return cdceth_handle_eem_rx(ncp, pktptr, pktlen);
        case CDCETH_ENCAP_NCM:
            return cdceth_handle_ncm_rx(ncp, pktptr, pktlen);
        case CDCETH_ENCAP_ECM:
        default:
            return nReadPacket(ncp, pktptr, pktlen);
    }
}
