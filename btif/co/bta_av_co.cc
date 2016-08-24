/******************************************************************************
 *
 *  Copyright (C) 2004-2012 Broadcom Corporation
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

/******************************************************************************
 *
 *  This is the advanced audio/video call-out function implementation for
 *  BTIF.
 *
 ******************************************************************************/

#include "string.h"
#include "a2d_api.h"
#include "a2d_sbc.h"
#include "bta_sys.h"
#include "bta_av_api.h"
#include "bta_av_co.h"
#include "bta_av_ci.h"
#include "bta_av_sbc.h"

#include "btif_media.h"
#include "sbc_encoder.h"
#include "btif_av_co.h"
#include "btif_util.h"
#include "osi/include/mutex.h"


/*****************************************************************************
 **  Constants
 *****************************************************************************/

#define FUNC_TRACE()     APPL_TRACE_DEBUG("%s", __func__);

/* Macro to retrieve the number of elements in a statically allocated array */
#define BTA_AV_CO_NUM_ELEMENTS(__a) (sizeof(__a)/sizeof((__a)[0]))

/* MIN and MAX macros */
#define BTA_AV_CO_MIN(X,Y) ((X) < (Y) ? (X) : (Y))
#define BTA_AV_CO_MAX(X,Y) ((X) > (Y) ? (X) : (Y))

/* Macro to convert audio handle to index and vice versa */
#define BTA_AV_CO_AUDIO_HNDL_TO_INDX(hndl) (((hndl) & (~BTA_AV_CHNL_MSK)) - 1)
#define BTA_AV_CO_AUDIO_INDX_TO_HNDL(indx) (((indx) + 1) | BTA_AV_CHNL_AUDIO)


/* Offsets to access codec information in SBC codec */
#define BTA_AV_CO_SBC_FREQ_CHAN_OFF    3
#define BTA_AV_CO_SBC_BLOCK_BAND_OFF   4
#define BTA_AV_CO_SBC_MIN_BITPOOL_OFF  5
#define BTA_AV_CO_SBC_MAX_BITPOOL_OFF  6

#define BTA_AV_CO_SBC_MAX_BITPOOL  53

/* SCMS-T protect info */
const uint8_t bta_av_co_cp_scmst[BTA_AV_CP_INFO_LEN] = {0x02, 0x02, 0x00};

/* SBC SRC codec capabilities */
const tA2D_SBC_CIE bta_av_co_sbc_caps =
{
    (A2D_SBC_IE_SAMP_FREQ_44), /* samp_freq */
    (A2D_SBC_IE_CH_MD_JOINT), /* ch_mode */
    (A2D_SBC_IE_BLOCKS_16), /* block_len */
    (A2D_SBC_IE_SUBBAND_8), /* num_subbands */
    (A2D_SBC_IE_ALLOC_MD_L), /* alloc_mthd */
    BTA_AV_CO_SBC_MAX_BITPOOL, /* max_bitpool */
    A2D_SBC_IE_MIN_BITPOOL /* min_bitpool */
};

/* SBC SINK codec capabilities */
const tA2D_SBC_CIE bta_av_co_sbc_sink_caps =
{
    (A2D_SBC_IE_SAMP_FREQ_48 | A2D_SBC_IE_SAMP_FREQ_44), /* samp_freq */
    (A2D_SBC_IE_CH_MD_MONO | A2D_SBC_IE_CH_MD_STEREO | A2D_SBC_IE_CH_MD_JOINT | A2D_SBC_IE_CH_MD_DUAL), /* ch_mode */
    (A2D_SBC_IE_BLOCKS_16 | A2D_SBC_IE_BLOCKS_12 | A2D_SBC_IE_BLOCKS_8 | A2D_SBC_IE_BLOCKS_4), /* block_len */
    (A2D_SBC_IE_SUBBAND_4 | A2D_SBC_IE_SUBBAND_8), /* num_subbands */
    (A2D_SBC_IE_ALLOC_MD_L | A2D_SBC_IE_ALLOC_MD_S), /* alloc_mthd */
    A2D_SBC_IE_MAX_BITPOOL, /* max_bitpool */
    A2D_SBC_IE_MIN_BITPOOL /* min_bitpool */
};

#if !defined(BTIF_AV_SBC_DEFAULT_SAMP_FREQ)
#define BTIF_AV_SBC_DEFAULT_SAMP_FREQ A2D_SBC_IE_SAMP_FREQ_44
#endif

/* Default SBC codec configuration */
const tA2D_SBC_CIE btif_av_sbc_default_config =
{
    BTIF_AV_SBC_DEFAULT_SAMP_FREQ,   /* samp_freq */
    A2D_SBC_IE_CH_MD_JOINT,         /* ch_mode */
    A2D_SBC_IE_BLOCKS_16,           /* block_len */
    A2D_SBC_IE_SUBBAND_8,           /* num_subbands */
    A2D_SBC_IE_ALLOC_MD_L,          /* alloc_mthd */
    BTA_AV_CO_SBC_MAX_BITPOOL,      /* max_bitpool */
    A2D_SBC_IE_MIN_BITPOOL          /* min_bitpool */
};


/*****************************************************************************
**  Local data
*****************************************************************************/
typedef struct
{
    uint8_t sep_info_idx;                 /* local SEP index (in BTA tables) */
    uint8_t seid;                         /* peer SEP index (in peer tables) */
    uint8_t codec_type;                   /* peer SEP codec type */
    uint8_t codec_caps[AVDT_CODEC_SIZE];  /* peer SEP codec capabilities */
    uint8_t num_protect;                  /* peer SEP number of CP elements */
    uint8_t protect_info[BTA_AV_CP_INFO_LEN];  /* peer SEP content protection info */
} tBTA_AV_CO_SINK;

typedef struct
{
    BD_ADDR         addr;               /* address of audio/video peer */
    tBTA_AV_CO_SINK snks[BTIF_SV_AV_AA_SEP_INDEX]; /* array of supported sinks */
    tBTA_AV_CO_SINK srcs[BTIF_SV_AV_AA_SEP_INDEX]; /* array of supported srcs */
    uint8_t           num_snks;           /* total number of sinks at peer */
    uint8_t           num_srcs;           /* total number of srcs at peer */
    uint8_t           num_seps;           /* total number of seids at peer */
    uint8_t           num_rx_snks;        /* number of received sinks */
    uint8_t           num_rx_srcs;        /* number of received srcs */
    uint8_t           num_sup_snks;       /* number of supported sinks in the snks array */
    uint8_t           num_sup_srcs;       /* number of supported srcs in the srcs array */
    tBTA_AV_CO_SINK *p_snk;             /* currently selected sink */
    tBTA_AV_CO_SINK *p_src;             /* currently selected src */
    uint8_t           codec_cfg[AVDT_CODEC_SIZE]; /* current codec configuration */
    bool         cp_active;          /* current CP configuration */
    bool         acp;                /* acceptor */
    bool         recfg_needed;       /* reconfiguration is needed */
    bool         opened;             /* opened */
    uint16_t          mtu;                /* maximum transmit unit size */
    uint16_t          uuid_to_connect;    /* uuid of peer device */
} tBTA_AV_CO_PEER;

typedef struct
{
    bool active;
    uint8_t flag;
} tBTA_AV_CO_CP;

typedef struct
{
    /* Connected peer information */
    tBTA_AV_CO_PEER peers[BTA_AV_NUM_STRS];
    /* Current codec configuration - access to this variable must be protected */
    tBTIF_AV_CODEC_INFO codec_cfg;
    tBTIF_AV_CODEC_INFO codec_cfg_setconfig; /* remote peer setconfig preference */

    tBTA_AV_CO_CP cp;
} tBTA_AV_CO_CB;

/* Control block instance */
static tBTA_AV_CO_CB bta_av_co_cb;

static bool bta_av_co_audio_codec_build_config(const uint8_t *p_codec_caps, uint8_t *p_codec_cfg);
static void bta_av_co_audio_peer_reset_config(tBTA_AV_CO_PEER *p_peer);
static bool bta_av_co_cp_is_scmst(const uint8_t *p_protectinfo);
static bool bta_av_co_audio_sink_has_scmst(const tBTA_AV_CO_SINK *p_sink);
static bool bta_av_co_audio_peer_supports_codec(tBTA_AV_CO_PEER *p_peer, uint8_t *p_snk_index);
static bool bta_av_co_audio_media_supports_config(uint8_t codec_type, const uint8_t *p_codec_cfg);
static bool bta_av_co_audio_sink_supports_config(uint8_t codec_type, const uint8_t *p_codec_cfg);
static bool bta_av_co_audio_peer_src_supports_codec(tBTA_AV_CO_PEER *p_peer, uint8_t *p_src_index);



/*******************************************************************************
 **
 ** Function         bta_av_co_cp_is_active
 **
 ** Description      Get the current configuration of content protection
 **
 ** Returns          true if the current streaming has CP, false otherwise
 **
 *******************************************************************************/
bool bta_av_co_cp_is_active(void)
{
    FUNC_TRACE();
    return bta_av_co_cb.cp.active;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_cp_get_flag
 **
 ** Description      Get content protection flag
 **                  BTA_AV_CP_SCMS_COPY_NEVER
 **                  BTA_AV_CP_SCMS_COPY_ONCE
 **                  BTA_AV_CP_SCMS_COPY_FREE
 **
 ** Returns          The current flag value
 **
 *******************************************************************************/
uint8_t bta_av_co_cp_get_flag(void)
{
    FUNC_TRACE();
    return bta_av_co_cb.cp.flag;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_cp_set_flag
 **
 ** Description      Set content protection flag
 **                  BTA_AV_CP_SCMS_COPY_NEVER
 **                  BTA_AV_CP_SCMS_COPY_ONCE
 **                  BTA_AV_CP_SCMS_COPY_FREE
 **
 ** Returns          true if setting the SCMS flag is supported else false
 **
 *******************************************************************************/
bool bta_av_co_cp_set_flag(uint8_t cp_flag)
{
    FUNC_TRACE();

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
#else
    if (cp_flag != BTA_AV_CP_SCMS_COPY_FREE)
    {
        return false;
    }
#endif
    bta_av_co_cb.cp.flag = cp_flag;
    return true;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_get_peer
 **
 ** Description      find the peer entry for a given handle
 **
 ** Returns          the control block
 **
 *******************************************************************************/
static tBTA_AV_CO_PEER *bta_av_co_get_peer(tBTA_AV_HNDL hndl)
{
    uint8_t index;
    FUNC_TRACE();

    index = BTA_AV_CO_AUDIO_HNDL_TO_INDX(hndl);

    /* Sanity check */
    if (index >= BTA_AV_CO_NUM_ELEMENTS(bta_av_co_cb.peers))
    {
        APPL_TRACE_ERROR("bta_av_co_get_peer peer index out of bounds:%d", index);
        return NULL;
    }

    return &bta_av_co_cb.peers[index];
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_init
 **
 ** Description      This callout function is executed by AV when it is
 **                  started by calling BTA_AvRegister().  This function can be
 **                  used by the phone to initialize audio paths or for other
 **                  initialization purposes.
 **
 **
 ** Returns          Stream codec and content protection capabilities info.
 **
 *******************************************************************************/
bool bta_av_co_audio_init(uint8_t *p_codec_type, uint8_t *p_codec_info, uint8_t *p_num_protect,
        uint8_t *p_protect_info, uint8_t index)
{
    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_init: %d", index);

    /* By default - no content protection info */
    *p_num_protect = 0;
    *p_protect_info = 0;

    /* reset remote preference through setconfig */
    bta_av_co_cb.codec_cfg_setconfig.id = BTIF_AV_CODEC_NONE;

    switch (index)
    {
    case BTIF_SV_AV_AA_SBC_INDEX:
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
    {
        uint8_t *p = p_protect_info;

        /* Content protection info - support SCMS-T */
        *p_num_protect = 1;
        *p++ = BTA_AV_CP_LOSC;
        UINT16_TO_STREAM(p, BTA_AV_CP_SCMS_T_ID);

    }
#endif
        /* Set up for SBC codec  for SRC*/
        *p_codec_type = BTA_AV_CODEC_SBC;

        /* This should not fail because we are using constants for parameters */
        A2D_BldSbcInfo(AVDT_MEDIA_AUDIO, (tA2D_SBC_CIE *) &bta_av_co_sbc_caps, p_codec_info);

        /* Codec is valid */
        return true;
#if (BTA_AV_SINK_INCLUDED == TRUE)
    case BTIF_SV_AV_AA_SBC_SINK_INDEX:
        *p_codec_type = BTA_AV_CODEC_SBC;

        /* This should not fail because we are using constants for parameters */
        A2D_BldSbcInfo(AVDT_MEDIA_AUDIO, (tA2D_SBC_CIE *) &bta_av_co_sbc_sink_caps, p_codec_info);

        /* Codec is valid */
        return true;
#endif
    default:
        /* Not valid */
        return false;
    }
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_disc_res
 **
 ** Description      This callout function is executed by AV to report the
 **                  number of stream end points (SEP) were found during the
 **                  AVDT stream discovery process.
 **
 **
 ** Returns          void.
 **
 *******************************************************************************/
void bta_av_co_audio_disc_res(tBTA_AV_HNDL hndl, uint8_t num_seps, uint8_t num_snk,
        uint8_t num_src, BD_ADDR addr, uint16_t uuid_local)
{
    tBTA_AV_CO_PEER *p_peer;

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_disc_res h:x%x num_seps:%d num_snk:%d num_src:%d",
            hndl, num_seps, num_snk, num_src);

    /* Find the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_disc_res could not find peer entry");
        return;
    }

    /* Sanity check : this should never happen */
    if (p_peer->opened)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_disc_res peer already opened");
    }

    /* Copy the discovery results */
    bdcpy(p_peer->addr, addr);
    p_peer->num_snks = num_snk;
    p_peer->num_srcs = num_src;
    p_peer->num_seps = num_seps;
    p_peer->num_rx_snks = 0;
    p_peer->num_rx_srcs = 0;
    p_peer->num_sup_snks = 0;
    if (uuid_local == UUID_SERVCLASS_AUDIO_SINK)
        p_peer->uuid_to_connect = UUID_SERVCLASS_AUDIO_SOURCE;
    else if (uuid_local == UUID_SERVCLASS_AUDIO_SOURCE)
        p_peer->uuid_to_connect = UUID_SERVCLASS_AUDIO_SINK;
}

/*******************************************************************************
 **
 ** Function         bta_av_build_src_cfg
 **
 ** Description      This function will build preferred config from src capabilities
 **
 **
 ** Returns          Pass or Fail for current getconfig.
 **
 *******************************************************************************/
void bta_av_build_src_cfg (uint8_t *p_pref_cfg, uint8_t *p_src_cap)
{
    tA2D_SBC_CIE    src_cap;
    tA2D_SBC_CIE    pref_cap;
    uint8_t           status = 0;

    /* initialize it to default SBC configuration */
    A2D_BldSbcInfo(AVDT_MEDIA_AUDIO, (tA2D_SBC_CIE *) &btif_av_sbc_default_config, p_pref_cfg);
    /* now try to build a preferred one */
    /* parse configuration */
    if ((status = A2D_ParsSbcInfo(&src_cap, p_src_cap, true)) != 0)
    {
         APPL_TRACE_DEBUG(" Cant parse src cap ret = %d", status);
         return ;
    }

    if (src_cap.samp_freq & A2D_SBC_IE_SAMP_FREQ_48)
        pref_cap.samp_freq = A2D_SBC_IE_SAMP_FREQ_48;
    else if (src_cap.samp_freq & A2D_SBC_IE_SAMP_FREQ_44)
        pref_cap.samp_freq = A2D_SBC_IE_SAMP_FREQ_44;

    if (src_cap.ch_mode & A2D_SBC_IE_CH_MD_JOINT)
        pref_cap.ch_mode = A2D_SBC_IE_CH_MD_JOINT;
    else if (src_cap.ch_mode & A2D_SBC_IE_CH_MD_STEREO)
        pref_cap.ch_mode = A2D_SBC_IE_CH_MD_STEREO;
    else if (src_cap.ch_mode & A2D_SBC_IE_CH_MD_DUAL)
        pref_cap.ch_mode = A2D_SBC_IE_CH_MD_DUAL;
    else if (src_cap.ch_mode & A2D_SBC_IE_CH_MD_MONO)
        pref_cap.ch_mode = A2D_SBC_IE_CH_MD_MONO;

    if (src_cap.block_len & A2D_SBC_IE_BLOCKS_16)
        pref_cap.block_len = A2D_SBC_IE_BLOCKS_16;
    else if (src_cap.block_len & A2D_SBC_IE_BLOCKS_12)
        pref_cap.block_len = A2D_SBC_IE_BLOCKS_12;
    else if (src_cap.block_len & A2D_SBC_IE_BLOCKS_8)
        pref_cap.block_len = A2D_SBC_IE_BLOCKS_8;
    else if (src_cap.block_len & A2D_SBC_IE_BLOCKS_4)
        pref_cap.block_len = A2D_SBC_IE_BLOCKS_4;

    if (src_cap.num_subbands & A2D_SBC_IE_SUBBAND_8)
        pref_cap.num_subbands = A2D_SBC_IE_SUBBAND_8;
    else if(src_cap.num_subbands & A2D_SBC_IE_SUBBAND_4)
        pref_cap.num_subbands = A2D_SBC_IE_SUBBAND_4;

    if (src_cap.alloc_mthd & A2D_SBC_IE_ALLOC_MD_L)
        pref_cap.alloc_mthd = A2D_SBC_IE_ALLOC_MD_L;
    else if(src_cap.alloc_mthd & A2D_SBC_IE_ALLOC_MD_S)
        pref_cap.alloc_mthd = A2D_SBC_IE_ALLOC_MD_S;

    pref_cap.max_bitpool = src_cap.max_bitpool;
    pref_cap.min_bitpool = src_cap.min_bitpool;

    A2D_BldSbcInfo(AVDT_MEDIA_AUDIO, (tA2D_SBC_CIE *) &pref_cap, p_pref_cfg);
}

/*******************************************************************************
 **
 ** Function         bta_av_audio_sink_getconfig
 **
 ** Description      This callout function is executed by AV to retrieve the
 **                  desired codec and content protection configuration for the
 **                  A2DP Sink audio stream in Initiator.
 **
 **
 ** Returns          Pass or Fail for current getconfig.
 **
 *******************************************************************************/
uint8_t bta_av_audio_sink_getconfig(tBTA_AV_HNDL hndl, tA2D_CODEC codec_type,
                                    uint8_t *p_codec_info,
                                    uint8_t *p_sep_info_idx, uint8_t seid,
                                    uint8_t *p_num_protect,
                                    uint8_t *p_protect_info)
{

    uint8_t result = A2D_FAIL;
    bool supported;
    tBTA_AV_CO_PEER *p_peer;
    tBTA_AV_CO_SINK *p_src;
    uint8_t pref_cfg[AVDT_CODEC_SIZE];
    uint8_t index;

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_audio_sink_getconfig handle:0x%x codec_type:%d seid:%d",
                                                               hndl, codec_type, seid);
    APPL_TRACE_DEBUG("num_protect:0x%02x protect_info:0x%02x%02x%02x",
        *p_num_protect, p_protect_info[0], p_protect_info[1], p_protect_info[2]);

    /* Retrieve the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_audio_sink_getconfig could not find peer entry");
        return A2D_FAIL;
    }

    APPL_TRACE_DEBUG("bta_av_audio_sink_getconfig peer(o=%d,n_snks=%d,n_rx_snks=%d,n_sup_snks=%d)",
            p_peer->opened, p_peer->num_srcs, p_peer->num_rx_srcs, p_peer->num_sup_srcs);

    p_peer->num_rx_srcs++;

    /* Check if this is a supported configuration */
    supported = false;
    switch (codec_type)
    {
        case BTA_AV_CODEC_SBC:
            supported = true;
            break;

        default:
            break;
    }

    if (supported)
    {
        /* If there is room for a new one */
        if (p_peer->num_sup_srcs < BTA_AV_CO_NUM_ELEMENTS(p_peer->srcs))
        {
            p_src = &p_peer->srcs[p_peer->num_sup_srcs++];

            APPL_TRACE_DEBUG("bta_av_audio_sink_getconfig saved caps[%x:%x:%x:%x:%x:%x]",
                    p_codec_info[1], p_codec_info[2], p_codec_info[3],
                    p_codec_info[4], p_codec_info[5], p_codec_info[6]);

            memcpy(p_src->codec_caps, p_codec_info, AVDT_CODEC_SIZE);
            p_src->codec_type = codec_type;
            p_src->sep_info_idx = *p_sep_info_idx;
            p_src->seid = seid;
            p_src->num_protect = *p_num_protect;
            memcpy(p_src->protect_info, p_protect_info, BTA_AV_CP_INFO_LEN);
        }
        else
        {
            APPL_TRACE_ERROR("bta_av_audio_sink_getconfig no more room for SRC info");
        }
    }

    /* If last SNK get capabilities or all supported codec caps retrieved */
    if ((p_peer->num_rx_srcs == p_peer->num_srcs) ||
        (p_peer->num_sup_srcs == BTA_AV_CO_NUM_ELEMENTS(p_peer->srcs)))
    {
        APPL_TRACE_DEBUG("bta_av_audio_sink_getconfig last SRC reached");

        /* Protect access to bta_av_co_cb.codec_cfg */
        mutex_global_lock();

        /* Find a src that matches the codec config */
        if (bta_av_co_audio_peer_src_supports_codec(p_peer, &index))
        {
            APPL_TRACE_DEBUG(" Codec Supported ");
            p_src = &p_peer->srcs[index];

            /* Build the codec configuration for this sink */
            {
                /* Save the new configuration */
                p_peer->p_src = p_src;
                /* get preferred config from src_caps */
                bta_av_build_src_cfg(pref_cfg, p_src->codec_caps);
                memcpy(p_peer->codec_cfg, pref_cfg, AVDT_CODEC_SIZE);

                APPL_TRACE_DEBUG("bta_av_audio_sink_getconfig  p_codec_info[%x:%x:%x:%x:%x:%x]",
                        p_peer->codec_cfg[1], p_peer->codec_cfg[2], p_peer->codec_cfg[3],
                        p_peer->codec_cfg[4], p_peer->codec_cfg[5], p_peer->codec_cfg[6]);
                /* By default, no content protection */
                *p_num_protect = 0;

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
                    p_peer->cp_active = false;
                    bta_av_co_cb.cp.active = false;
#endif

                    *p_sep_info_idx = p_src->sep_info_idx;
                    memcpy(p_codec_info, p_peer->codec_cfg, AVDT_CODEC_SIZE);
                result =  A2D_SUCCESS;
            }
        }
        /* Protect access to bta_av_co_cb.codec_cfg */
        mutex_global_unlock();
    }
    return result;
}
/*******************************************************************************
 **
 ** Function         bta_av_co_audio_getconfig
 **
 ** Description      This callout function is executed by AV to retrieve the
 **                  desired codec and content protection configuration for the
 **                  audio stream.
 **
 **
 ** Returns          Stream codec and content protection configuration info.
 **
 *******************************************************************************/
uint8_t bta_av_co_audio_getconfig(tBTA_AV_HNDL hndl, tA2D_CODEC codec_type,
                                  uint8_t *p_codec_info,
                                  uint8_t *p_sep_info_idx, uint8_t seid,
                                  uint8_t *p_num_protect,
                                  uint8_t *p_protect_info)
{
    uint8_t result = A2D_FAIL;
    bool supported;
    tBTA_AV_CO_PEER *p_peer;
    tBTA_AV_CO_SINK *p_sink;
    uint8_t codec_cfg[AVDT_CODEC_SIZE];
    uint8_t index;

    FUNC_TRACE();

    /* Retrieve the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("%s: could not find peer entry", __func__);
        return A2D_FAIL;
    }

    if (p_peer->uuid_to_connect == UUID_SERVCLASS_AUDIO_SOURCE)
    {
        result = bta_av_audio_sink_getconfig(hndl, codec_type, p_codec_info,
                                             p_sep_info_idx, seid,
                                             p_num_protect, p_protect_info);
        return result;
    }
    APPL_TRACE_DEBUG("%s: handle:0x%x codec_type:%d seid:%d",
                     __func__, hndl, codec_type, seid);
    APPL_TRACE_DEBUG("%s: num_protect:0x%02x protect_info:0x%02x%02x%02x",
                     __func__, *p_num_protect, p_protect_info[0],
                     p_protect_info[1], p_protect_info[2]);
    APPL_TRACE_DEBUG("%s: peer(o=%d, n_snks=%d, n_rx_snks=%d, n_sup_snks=%d)",
                     __func__, p_peer->opened, p_peer->num_snks,
                     p_peer->num_rx_snks, p_peer->num_sup_snks);

    p_peer->num_rx_snks++;

    /* Check if this is a supported configuration */
    supported = false;
    switch (codec_type)
    {
    case BTA_AV_CODEC_SBC:
        supported = true;
        break;

    default:
        break;
    }

    if (supported)
    {
        /* If there is room for a new one */
        if (p_peer->num_sup_snks < BTA_AV_CO_NUM_ELEMENTS(p_peer->snks))
        {
            p_sink = &p_peer->snks[p_peer->num_sup_snks++];

            APPL_TRACE_DEBUG("%s: saved caps[%x:%x:%x:%x:%x:%x]", __func__,
                    p_codec_info[1], p_codec_info[2], p_codec_info[3],
                    p_codec_info[4], p_codec_info[5], p_codec_info[6]);

            memcpy(p_sink->codec_caps, p_codec_info, AVDT_CODEC_SIZE);
            p_sink->codec_type = codec_type;
            p_sink->sep_info_idx = *p_sep_info_idx;
            p_sink->seid = seid;
            p_sink->num_protect = *p_num_protect;
            memcpy(p_sink->protect_info, p_protect_info, BTA_AV_CP_INFO_LEN);
        } else {
            APPL_TRACE_ERROR("%s: no more room for SNK info", __func__);
        }
    }

    /* If last SNK get capabilities or all supported codec capa retrieved */
    if ((p_peer->num_rx_snks == p_peer->num_snks) ||
        (p_peer->num_sup_snks == BTA_AV_CO_NUM_ELEMENTS(p_peer->snks)))
    {
        APPL_TRACE_DEBUG("%s: last sink reached", __func__);

        /* Protect access to bta_av_co_cb.codec_cfg */
        mutex_global_lock();

        /* Find a sink that matches the codec config */
        if (bta_av_co_audio_peer_supports_codec(p_peer, &index))
        {
            /* stop fetching caps once we retrieved a supported codec */
            if (p_peer->acp)
            {
                *p_sep_info_idx = p_peer->num_seps;
                APPL_TRACE_EVENT("%s: no need to fetch more SEPs", __func__);
            }

            p_sink = &p_peer->snks[index];

            /* Build the codec configuration for this sink */
            if (bta_av_co_audio_codec_build_config(p_sink->codec_caps, codec_cfg))
            {
                APPL_TRACE_DEBUG("%s: reconfig codec_cfg[%x:%x:%x:%x:%x:%x]",
                        __func__,
                        codec_cfg[1], codec_cfg[2], codec_cfg[3],
                        codec_cfg[4], codec_cfg[5], codec_cfg[6]);
                for (int i = 0; i < AVDT_CODEC_SIZE; i++) {
                    APPL_TRACE_DEBUG("%s: p_codec_info[%d]: %x",
                                     __func__, i,  p_codec_info[i]);
                }

                /* Save the new configuration */
                p_peer->p_snk = p_sink;
                memcpy(p_peer->codec_cfg, codec_cfg, AVDT_CODEC_SIZE);

                /* By default, no content protection */
                *p_num_protect = 0;

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
                /* Check if this sink supports SCMS */
                if (bta_av_co_audio_sink_has_scmst(p_sink))
                {
                    p_peer->cp_active = true;
                    bta_av_co_cb.cp.active = true;
                    *p_num_protect = BTA_AV_CP_INFO_LEN;
                    memcpy(p_protect_info, bta_av_co_cp_scmst, BTA_AV_CP_INFO_LEN);
                }
                else
                {
                    p_peer->cp_active = false;
                    bta_av_co_cb.cp.active = false;
                }
#endif

                /* If acceptor -> reconfig otherwise reply for configuration */
                if (p_peer->acp)
                {
                    if (p_peer->recfg_needed)
                    {
                        APPL_TRACE_DEBUG("%s: call BTA_AvReconfig(x%x)",
                                         __func__, hndl);
                        BTA_AvReconfig(hndl, true, p_sink->sep_info_idx, p_peer->codec_cfg, *p_num_protect, (uint8_t *)bta_av_co_cp_scmst);
                    }
                }
                else
                {
                    *p_sep_info_idx = p_sink->sep_info_idx;
                    memcpy(p_codec_info, p_peer->codec_cfg, AVDT_CODEC_SIZE);
                }
                result =  A2D_SUCCESS;
            }
        }
        /* Protect access to bta_av_co_cb.codec_cfg */
        mutex_global_unlock();
    }
    return result;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_setconfig
 **
 ** Description      This callout function is executed by AV to set the codec and
 **                  content protection configuration of the audio stream.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_setconfig(tBTA_AV_HNDL hndl, tA2D_CODEC codec_type,
                               uint8_t *p_codec_info, uint8_t seid,
                               BD_ADDR addr, uint8_t num_protect,
                               uint8_t *p_protect_info, uint8_t t_local_sep,
                               uint8_t avdt_handle)
{
    tBTA_AV_CO_PEER *p_peer;
    uint8_t status = A2D_SUCCESS;
    uint8_t category = A2D_SUCCESS;
    bool recfg_needed = false;
    bool codec_cfg_supported = false;
    UNUSED(seid);
    UNUSED(addr);

    FUNC_TRACE();

    APPL_TRACE_DEBUG("%s: p_codec_info[%x:%x:%x:%x:%x:%x]",
            __func__,
            p_codec_info[1], p_codec_info[2], p_codec_info[3],
            p_codec_info[4], p_codec_info[5], p_codec_info[6]);
    APPL_TRACE_DEBUG("num_protect:0x%02x protect_info:0x%02x%02x%02x",
        num_protect, p_protect_info[0], p_protect_info[1], p_protect_info[2]);

    /* Retrieve the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("%s: could not find peer entry", __func__);

        /* Call call-in rejecting the configuration */
        bta_av_ci_setconfig(hndl, A2D_BUSY, AVDT_ASC_CODEC, 0, NULL, false, avdt_handle);
        return;
    }
    APPL_TRACE_DEBUG("%s: peer(o=%d, n_snks=%d, n_rx_snks=%d, n_sup_snks=%d)",
                     __func__, p_peer->opened, p_peer->num_snks,
                     p_peer->num_rx_snks, p_peer->num_sup_snks);

    /* Sanity check: should not be opened at this point */
    if (p_peer->opened)
    {
        APPL_TRACE_ERROR("%s: peer already in use", __func__);
    }

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
    if (num_protect != 0)
    {
        /* If CP is supported */
        if ((num_protect != 1) ||
            (bta_av_co_cp_is_scmst(p_protect_info) == false))
        {
            APPL_TRACE_ERROR("%s: wrong CP configuration", __func__);
            status = A2D_BAD_CP_TYPE;
            category = AVDT_ASC_PROTECT;
        }
    }
#else
    /* Do not support content protection for the time being */
    if (num_protect != 0)
    {
        APPL_TRACE_ERROR("%s: wrong CP configuration", __func__);
        status = A2D_BAD_CP_TYPE;
        category = AVDT_ASC_PROTECT;
    }
#endif
    if (status == A2D_SUCCESS)
    {
        if(AVDT_TSEP_SNK == t_local_sep)
        {
            codec_cfg_supported = bta_av_co_audio_sink_supports_config(codec_type, p_codec_info);
            APPL_TRACE_DEBUG(" Peer is  A2DP SRC ");
        }
        if(AVDT_TSEP_SRC == t_local_sep)
        {
            codec_cfg_supported = bta_av_co_audio_media_supports_config(codec_type, p_codec_info);
            APPL_TRACE_DEBUG(" Peer is A2DP SINK ");
        }
        /* Check if codec configuration is supported */
        if (codec_cfg_supported)
        {

            /* Protect access to bta_av_co_cb.codec_cfg */
            mutex_global_lock();

            /* Check if the configuration matches the current codec config */
            switch (bta_av_co_cb.codec_cfg.id)
            {
            case BTIF_AV_CODEC_SBC:
                if ((codec_type != BTA_AV_CODEC_SBC) || memcmp(p_codec_info, bta_av_co_cb.codec_cfg.info, 5))
                {
                    recfg_needed = true;
                }
                else if ((num_protect == 1) && (!bta_av_co_cb.cp.active))
                {
                    recfg_needed = true;
                }

                /* if remote side requests a restricted notify sinks preferred bitpool range as all other params are
                   already checked for validify */
                APPL_TRACE_EVENT("remote peer setconfig bitpool range [%d:%d]",
                   p_codec_info[BTA_AV_CO_SBC_MIN_BITPOOL_OFF],
                   p_codec_info[BTA_AV_CO_SBC_MAX_BITPOOL_OFF] );

                bta_av_co_cb.codec_cfg_setconfig.id = BTIF_AV_CODEC_SBC;
                memcpy(bta_av_co_cb.codec_cfg_setconfig.info, p_codec_info, AVDT_CODEC_SIZE);
                if(AVDT_TSEP_SNK == t_local_sep)
                {
                    /* If Peer is SRC, and our cfg subset matches with what is requested by peer, then
                                         just accept what peer wants */
                    memcpy(bta_av_co_cb.codec_cfg.info, p_codec_info, AVDT_CODEC_SIZE);
                    recfg_needed = false;
                }
                break;


            default:
                APPL_TRACE_ERROR("%s: unsupported cid %d", __func__,
                                 bta_av_co_cb.codec_cfg.id);
                recfg_needed = true;
                break;
            }
            /* Protect access to bta_av_co_cb.codec_cfg */
            mutex_global_unlock();
        }
        else
        {
            category = AVDT_ASC_CODEC;
            status = A2D_WRONG_CODEC;
        }
    }

    if (status != A2D_SUCCESS)
    {
        APPL_TRACE_DEBUG("%s: reject s=%d c=%d", __func__, status, category);

        /* Call call-in rejecting the configuration */
        bta_av_ci_setconfig(hndl, status, category, 0, NULL, false, avdt_handle);
    }
    else
    {
        /* Mark that this is an acceptor peer */
        p_peer->acp = true;
        p_peer->recfg_needed = recfg_needed;

        APPL_TRACE_DEBUG("%s: accept reconf=%d", __func__, recfg_needed);

        /* Call call-in accepting the configuration */
        bta_av_ci_setconfig(hndl, A2D_SUCCESS, A2D_SUCCESS, 0, NULL, recfg_needed, avdt_handle);
    }
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_open
 **
 ** Description      This function is called by AV when the audio stream connection
 **                  is opened.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_open(tBTA_AV_HNDL hndl, tA2D_CODEC codec_type,
                          uint8_t *p_codec_info, uint16_t mtu)
{
    tBTA_AV_CO_PEER *p_peer;
    UNUSED(p_codec_info);

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_open mtu:%d codec_type:%d", mtu, codec_type);

    /* Retrieve the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("%s: could not find peer entry", __func__);
    }
    else
    {
        p_peer->opened = true;
        p_peer->mtu = mtu;
    }
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_close
 **
 ** Description      This function is called by AV when the audio stream connection
 **                  is closed.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_close(tBTA_AV_HNDL hndl, tA2D_CODEC codec_type,
                           uint16_t mtu)

{
    tBTA_AV_CO_PEER *p_peer;
    UNUSED(codec_type);
    UNUSED(mtu);

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_close");

    /* Retrieve the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer)
    {
        /* Mark the peer closed and clean the peer info */
        memset(p_peer, 0, sizeof(*p_peer));
    }
    else
    {
        APPL_TRACE_ERROR("bta_av_co_audio_close could not find peer entry");
    }

    /* reset remote preference through setconfig */
    bta_av_co_cb.codec_cfg_setconfig.id = BTIF_AV_CODEC_NONE;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_start
 **
 ** Description      This function is called by AV when the audio streaming data
 **                  transfer is started.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_start(tBTA_AV_HNDL hndl, tA2D_CODEC codec_type,
                           uint8_t *p_codec_info, bool *p_no_rtp_hdr)
{
    UNUSED(hndl);
    UNUSED(codec_type);
    UNUSED(p_codec_info);
    UNUSED(p_no_rtp_hdr);

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_start");

}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_stop
 **
 ** Description      This function is called by AV when the audio streaming data
 **                  transfer is stopped.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_stop(tBTA_AV_HNDL hndl, tA2D_CODEC codec_type)
{
    UNUSED(hndl);
    UNUSED(codec_type);

    FUNC_TRACE();

    APPL_TRACE_DEBUG("%s", __func__);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_src_data_path
 **
 ** Description      This function is called to manage data transfer from
 **                  the audio codec to AVDTP.
 **
 ** Returns          Pointer to the GKI buffer to send, NULL if no buffer to send
 **
 *******************************************************************************/
void * bta_av_co_audio_src_data_path(tA2D_CODEC codec_type, uint32_t *p_len,
                                     uint32_t *p_timestamp)
{
    BT_HDR *p_buf;
    UNUSED(p_len);

    FUNC_TRACE();

    p_buf = btif_media_aa_readbuf();
    if (p_buf != NULL)
    {
        switch (codec_type)
        {
        case BTA_AV_CODEC_SBC:
            /* In media packet SBC, the following information is available:
             * p_buf->layer_specific : number of SBC frames in the packet
             * p_buf->word[0] : timestamp
             */
            /* Retrieve the timestamp information from the media packet */
            *p_timestamp = *((uint32_t *) (p_buf + 1));

            /* Set up packet header */
            bta_av_sbc_bld_hdr(p_buf, p_buf->layer_specific);
            break;


        default:
            APPL_TRACE_ERROR("bta_av_co_audio_src_data_path Unsupported codec type (%d)", codec_type);
            break;
        }
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
        {
            uint8_t *p;
            if (bta_av_co_cp_is_active())
            {
                p_buf->len++;
                p_buf->offset--;
                p = (uint8_t *)(p_buf + 1) + p_buf->offset;
                *p = bta_av_co_cp_get_flag();
            }
        }
#endif
    }
    return p_buf;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_drop
 **
 ** Description      An Audio packet is dropped. .
 **                  It's very likely that the connected headset with this handle
 **                  is moved far away. The implementation may want to reduce
 **                  the encoder bit rate setting to reduce the packet size.
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_drop(tBTA_AV_HNDL hndl)
{
    FUNC_TRACE();

    APPL_TRACE_ERROR("bta_av_co_audio_drop dropped: x%x", hndl);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_delay
 **
 ** Description      This function is called by AV when the audio stream connection
 **                  needs to send the initial delay report to the connected SRC.
 **
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_delay(tBTA_AV_HNDL hndl, uint16_t delay)
{
    FUNC_TRACE();

    APPL_TRACE_ERROR("bta_av_co_audio_delay handle: x%x, delay:0x%x", hndl, delay);
}



/*******************************************************************************
 **
 ** Function         bta_av_co_audio_codec_build_config
 **
 ** Description      Build the codec configuration
 **
 ** Returns          true if the codec was built successfully, false otherwise
 **
 *******************************************************************************/
static bool bta_av_co_audio_codec_build_config(const uint8_t *p_codec_caps, uint8_t *p_codec_cfg)
{
    FUNC_TRACE();

    memset(p_codec_cfg, 0, AVDT_CODEC_SIZE);

    switch (bta_av_co_cb.codec_cfg.id)
    {
    case BTIF_AV_CODEC_SBC:
        /*  only copy the relevant portions for this codec to avoid issues when
            comparing codec configs covering larger codec sets than SBC (7 bytes) */
        memcpy(p_codec_cfg, bta_av_co_cb.codec_cfg.info, BTA_AV_CO_SBC_MAX_BITPOOL_OFF+1);

        /* Update the bit pool boundaries with the codec capabilities */
        p_codec_cfg[BTA_AV_CO_SBC_MIN_BITPOOL_OFF] = p_codec_caps[BTA_AV_CO_SBC_MIN_BITPOOL_OFF];
        p_codec_cfg[BTA_AV_CO_SBC_MAX_BITPOOL_OFF] = p_codec_caps[BTA_AV_CO_SBC_MAX_BITPOOL_OFF];

        APPL_TRACE_EVENT("bta_av_co_audio_codec_build_config : bitpool min %d, max %d",
                    p_codec_cfg[BTA_AV_CO_SBC_MIN_BITPOOL_OFF],
                    p_codec_caps[BTA_AV_CO_SBC_MAX_BITPOOL_OFF]);
        break;
    default:
        APPL_TRACE_ERROR("bta_av_co_audio_codec_build_config: unsupported codec id %d", bta_av_co_cb.codec_cfg.id);
        return false;
        break;
    }
    return true;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_codec_cfg_matches_caps
 **
 ** Description      Check if a codec config matches a codec capabilities
 **
 ** Returns          true if it codec config is supported, false otherwise
 **
 *******************************************************************************/
static bool bta_av_co_audio_codec_cfg_matches_caps(uint8_t codec_id, const uint8_t *p_codec_caps, const uint8_t *p_codec_cfg)
{
    FUNC_TRACE();

    switch(codec_id)
    {
    case BTIF_AV_CODEC_SBC:

        APPL_TRACE_EVENT("bta_av_co_audio_codec_cfg_matches_caps : min %d/%d max %d/%d",
           p_codec_caps[BTA_AV_CO_SBC_MIN_BITPOOL_OFF],
           p_codec_cfg[BTA_AV_CO_SBC_MIN_BITPOOL_OFF],
           p_codec_caps[BTA_AV_CO_SBC_MAX_BITPOOL_OFF],
           p_codec_cfg[BTA_AV_CO_SBC_MAX_BITPOOL_OFF]);

        /* Must match all items exactly except bitpool boundaries which can be adjusted */
        if (!((p_codec_caps[BTA_AV_CO_SBC_FREQ_CHAN_OFF] & p_codec_cfg[BTA_AV_CO_SBC_FREQ_CHAN_OFF]) &&
              (p_codec_caps[BTA_AV_CO_SBC_BLOCK_BAND_OFF] & p_codec_cfg[BTA_AV_CO_SBC_BLOCK_BAND_OFF])))
        {
            APPL_TRACE_EVENT("false %x %x %x %x",
                    p_codec_caps[BTA_AV_CO_SBC_FREQ_CHAN_OFF],
                    p_codec_cfg[BTA_AV_CO_SBC_FREQ_CHAN_OFF],
                    p_codec_caps[BTA_AV_CO_SBC_BLOCK_BAND_OFF],
                    p_codec_cfg[BTA_AV_CO_SBC_BLOCK_BAND_OFF]);
            return false;
        }
        break;


    default:
        APPL_TRACE_ERROR("bta_av_co_audio_codec_cfg_matches_caps: unsupported codec id %d", codec_id);
        return false;
        break;
    }
    APPL_TRACE_EVENT("true");

    return true;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_codec_match
 **
 ** Description      Check if a codec capabilities supports the codec config
 **
 ** Returns          true if the connection supports this codec, false otherwise
 **
 *******************************************************************************/
static bool bta_av_co_audio_codec_match(const uint8_t *p_codec_caps)
{
    FUNC_TRACE();

    return bta_av_co_audio_codec_cfg_matches_caps(bta_av_co_cb.codec_cfg.id, p_codec_caps, bta_av_co_cb.codec_cfg.info);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_peer_reset_config
 **
 ** Description      Reset the peer codec configuration
 **
 ** Returns          Nothing
 **
 *******************************************************************************/
static void bta_av_co_audio_peer_reset_config(tBTA_AV_CO_PEER *p_peer)
{
    FUNC_TRACE();

    /* Indicate that there is no currently selected sink */
    p_peer->p_snk = NULL;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_cp_is_scmst
 **
 ** Description      Check if a content protection service is SCMS-T
 **
 ** Returns          true if this CP is SCMS-T, false otherwise
 **
 *******************************************************************************/
static bool bta_av_co_cp_is_scmst(const uint8_t *p_protectinfo)
{
    uint16_t cp_id;
    FUNC_TRACE();

    if (*p_protectinfo >= BTA_AV_CP_LOSC)
    {
        p_protectinfo++;
        STREAM_TO_UINT16(cp_id, p_protectinfo);
        if (cp_id == BTA_AV_CP_SCMS_T_ID)
        {
            APPL_TRACE_DEBUG("bta_av_co_cp_is_scmst: SCMS-T found");
            return true;
        }
    }

    return false;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_sink_has_scmst
 **
 ** Description      Check if a sink supports SCMS-T
 **
 ** Returns          true if the sink supports this CP, false otherwise
 **
 *******************************************************************************/
static bool bta_av_co_audio_sink_has_scmst(const tBTA_AV_CO_SINK *p_sink)
{
    uint8_t index;
    const uint8_t *p;
    FUNC_TRACE();

    /* Check if sink supports SCMS-T */
    index = p_sink->num_protect;
    p = &p_sink->protect_info[0];

    while (index)
    {
        if (bta_av_co_cp_is_scmst(p))
        {
            return true;
        }
        /* Move to the next SC */
        p += *p + 1;
        /* Decrement the SC counter */
        index--;
    }
    APPL_TRACE_DEBUG("bta_av_co_audio_sink_has_scmst: SCMS-T not found");
    return false;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_sink_supports_cp
 **
 ** Description      Check if a sink supports the current content protection
 **
 ** Returns          true if the sink supports this CP, false otherwise
 **
 *******************************************************************************/
static bool bta_av_co_audio_sink_supports_cp(const tBTA_AV_CO_SINK *p_sink)
{
    FUNC_TRACE();

    /* Check if content protection is enabled for this stream */
    if (bta_av_co_cp_get_flag() != BTA_AV_CP_SCMS_COPY_FREE)
    {
        return bta_av_co_audio_sink_has_scmst(p_sink);
    }
    else
    {
        APPL_TRACE_DEBUG("bta_av_co_audio_sink_supports_cp: not required");
        return true;
    }
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_peer_supports_codec
 **
 ** Description      Check if a connection supports the codec config
 **
 ** Returns          true if the connection supports this codec, false otherwise
 **
 *******************************************************************************/
static bool bta_av_co_audio_peer_supports_codec(tBTA_AV_CO_PEER *p_peer, uint8_t *p_snk_index)
{
    int index;
    uint8_t codec_type;
    FUNC_TRACE();

    /* Configure the codec type to look for */
    codec_type = bta_av_co_cb.codec_cfg.id;


    for (index = 0; index < p_peer->num_sup_snks; index++)
    {
        if (p_peer->snks[index].codec_type == codec_type)
        {
            switch (bta_av_co_cb.codec_cfg.id)
            {
            case BTIF_AV_CODEC_SBC:
                if (p_snk_index) *p_snk_index = index;
                return bta_av_co_audio_codec_match(p_peer->snks[index].codec_caps);
                break;


            default:
                APPL_TRACE_ERROR("bta_av_co_audio_peer_supports_codec: unsupported codec id %d", bta_av_co_cb.codec_cfg.id);
                return false;
                break;
            }
        }
    }
    return false;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_peer_src_supports_codec
 **
 ** Description      Check if a peer acting as src supports codec config
 **
 ** Returns          true if the connection supports this codec, false otherwise
 **
 *******************************************************************************/
static bool bta_av_co_audio_peer_src_supports_codec(tBTA_AV_CO_PEER *p_peer, uint8_t *p_src_index)
{
    int index;
    uint8_t codec_type;
    FUNC_TRACE();

    /* Configure the codec type to look for */
    codec_type = bta_av_co_cb.codec_cfg.id;


    for (index = 0; index < p_peer->num_sup_srcs; index++)
    {
        if (p_peer->srcs[index].codec_type == codec_type)
        {
            switch (bta_av_co_cb.codec_cfg.id)
            {
            case BTIF_AV_CODEC_SBC:
                if (p_src_index) *p_src_index = index;
                if (0 ==  bta_av_sbc_cfg_matches_cap((uint8_t *)p_peer->srcs[index].codec_caps,
                                                     (tA2D_SBC_CIE *)&bta_av_co_sbc_sink_caps))
                {
                    return true;
                }
                break;

            default:
                APPL_TRACE_ERROR("peer_src_supports_codec: unsupported codec id %d",
                                                            bta_av_co_cb.codec_cfg.id);
                return false;
                break;
            }
        }
    }
    return false;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_sink_supports_config
 **
 ** Description      Check if the media source supports a given configuration
 **
 ** Returns          true if the media source supports this config, false otherwise
 **
 *******************************************************************************/
static bool bta_av_co_audio_sink_supports_config(uint8_t codec_type, const uint8_t *p_codec_cfg)
{
    FUNC_TRACE();

    switch (codec_type)
    {
    case BTA_AV_CODEC_SBC:
        if (bta_av_sbc_cfg_in_cap((uint8_t *)p_codec_cfg, (tA2D_SBC_CIE *)&bta_av_co_sbc_sink_caps))
        {
            return false;
        }
        break;


    default:
        APPL_TRACE_ERROR("bta_av_co_audio_media_supports_config unsupported codec type %d", codec_type);
        return false;
        break;
    }
    return true;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_media_supports_config
 **
 ** Description      Check if the media sink supports a given configuration
 **
 ** Returns          true if the media source supports this config, false otherwise
 **
 *******************************************************************************/
static bool bta_av_co_audio_media_supports_config(uint8_t codec_type, const uint8_t *p_codec_cfg)
{
    FUNC_TRACE();

    switch (codec_type)
    {
    case BTA_AV_CODEC_SBC:
        if (bta_av_sbc_cfg_in_cap((uint8_t *)p_codec_cfg, (tA2D_SBC_CIE *)&bta_av_co_sbc_caps))
        {
            return false;
        }
        break;


    default:
        APPL_TRACE_ERROR("bta_av_co_audio_media_supports_config unsupported codec type %d", codec_type);
        return false;
        break;
    }
    return true;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_codec_supported
 **
 ** Description      Check if all opened connections are compatible with a codec
 **                  configuration and content protection
 **
 ** Returns          true if all opened devices support this codec, false otherwise
 **
 *******************************************************************************/
bool bta_av_co_audio_codec_supported(tBTIF_STATUS *p_status)
{
    uint8_t index;
    uint8_t snk_index;
    tBTA_AV_CO_PEER *p_peer;
    tBTA_AV_CO_SINK *p_sink;
    uint8_t codec_cfg[AVDT_CODEC_SIZE];
    uint8_t num_protect = 0;
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
    bool cp_active;
#endif

    FUNC_TRACE();

    APPL_TRACE_DEBUG("bta_av_co_audio_codec_supported");

    /* Check AV feeding is supported */
    *p_status = BTIF_ERROR_SRV_AV_FEEDING_NOT_SUPPORTED;

    for (index = 0; index < BTA_AV_CO_NUM_ELEMENTS(bta_av_co_cb.peers); index++)
    {
        p_peer = &bta_av_co_cb.peers[index];
        if (p_peer->opened)
        {
            if (bta_av_co_audio_peer_supports_codec(p_peer, &snk_index))
            {
                p_sink = &p_peer->snks[snk_index];

                /* Check that this sink is compatible with the CP */
                if (!bta_av_co_audio_sink_supports_cp(p_sink))
                {
                    APPL_TRACE_DEBUG("bta_av_co_audio_codec_supported sink %d of peer %d doesn't support cp",
                            snk_index, index);
                    *p_status = BTIF_ERROR_SRV_AV_CP_NOT_SUPPORTED;
                    return false;
                }

                /* Build the codec configuration for this sink */
                if (bta_av_co_audio_codec_build_config(p_sink->codec_caps, codec_cfg))
                {
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
                    /* Check if this sink supports SCMS */
                    cp_active = bta_av_co_audio_sink_has_scmst(p_sink);
#endif
                    /* Check if this is a new configuration (new sink or new config) */
                    if ((p_sink != p_peer->p_snk) ||
                        (memcmp(codec_cfg, p_peer->codec_cfg, AVDT_CODEC_SIZE))
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
                        || (p_peer->cp_active != cp_active)
#endif
                        )
                    {
                        /* Save the new configuration */
                        p_peer->p_snk = p_sink;
                        memcpy(p_peer->codec_cfg, codec_cfg, AVDT_CODEC_SIZE);
#if (BTA_AV_CO_CP_SCMS_T == TRUE)
                        p_peer->cp_active = cp_active;
                        if (p_peer->cp_active)
                        {
                            bta_av_co_cb.cp.active = true;
                            num_protect = BTA_AV_CP_INFO_LEN;
                        }
                        else
                        {
                            bta_av_co_cb.cp.active = false;
                        }
#endif
                        APPL_TRACE_DEBUG("bta_av_co_audio_codec_supported call BTA_AvReconfig(x%x)", BTA_AV_CO_AUDIO_INDX_TO_HNDL(index));
                        BTA_AvReconfig(BTA_AV_CO_AUDIO_INDX_TO_HNDL(index), true, p_sink->sep_info_idx,
                                p_peer->codec_cfg, num_protect, (uint8_t *)bta_av_co_cp_scmst);
                    }
                }
            }
            else
            {
                APPL_TRACE_DEBUG("bta_av_co_audio_codec_supported index %d doesn't support codec", index);
                return false;
            }
        }
    }

    *p_status = BTIF_SUCCESS;
    return true;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_codec_reset
 **
 ** Description      Reset the current codec configuration
 **
 ** Returns          void
 **
 *******************************************************************************/
void bta_av_co_audio_codec_reset(void)
{
    mutex_global_lock();
    FUNC_TRACE();

    /* Reset the current configuration to SBC */
    bta_av_co_cb.codec_cfg.id = BTIF_AV_CODEC_SBC;

    if (A2D_BldSbcInfo(A2D_MEDIA_TYPE_AUDIO, (tA2D_SBC_CIE *)&btif_av_sbc_default_config, bta_av_co_cb.codec_cfg.info) != A2D_SUCCESS)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_codec_reset A2D_BldSbcInfo failed");
    }

    mutex_global_unlock();
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_set_codec
 **
 ** Description      Set the current codec configuration from the feeding type.
 **                  This function is starting to modify the configuration, it
 **                  should be protected.
 **
 ** Returns          true if successful, false otherwise
 **
 *******************************************************************************/
bool bta_av_co_audio_set_codec(const tBTIF_AV_MEDIA_FEEDINGS *p_feeding, tBTIF_STATUS *p_status)
{
    tA2D_SBC_CIE sbc_config;
    tBTIF_AV_CODEC_INFO new_cfg;

    FUNC_TRACE();

    /* Check AV feeding is supported */
    *p_status = BTIF_ERROR_SRV_AV_FEEDING_NOT_SUPPORTED;

    APPL_TRACE_DEBUG("bta_av_co_audio_set_codec cid=%d", p_feeding->format);

    /* Supported codecs */
    switch (p_feeding->format)
    {
    case BTIF_AV_CODEC_PCM:
        new_cfg.id = BTIF_AV_CODEC_SBC;

        sbc_config = btif_av_sbc_default_config;
        if ((p_feeding->cfg.pcm.num_channel != 1) &&
            (p_feeding->cfg.pcm.num_channel != 2))
        {
            APPL_TRACE_ERROR("bta_av_co_audio_set_codec PCM channel number unsupported");
            return false;
        }
        if ((p_feeding->cfg.pcm.bit_per_sample != 8) &&
            (p_feeding->cfg.pcm.bit_per_sample != 16))
        {
            APPL_TRACE_ERROR("bta_av_co_audio_set_codec PCM sample size unsupported");
            return false;
        }
        switch (p_feeding->cfg.pcm.sampling_freq)
        {
        case 8000:
        case 12000:
        case 16000:
        case 24000:
        case 32000:
        case 48000:
            sbc_config.samp_freq = A2D_SBC_IE_SAMP_FREQ_48;
            break;

        case 11025:
        case 22050:
        case 44100:
            sbc_config.samp_freq = A2D_SBC_IE_SAMP_FREQ_44;
            break;
        default:
            APPL_TRACE_ERROR("bta_av_co_audio_set_codec PCM sampling frequency unsupported");
            return false;
            break;
        }
        /* Build the codec config */
        if (A2D_BldSbcInfo(A2D_MEDIA_TYPE_AUDIO, &sbc_config, new_cfg.info) != A2D_SUCCESS)
        {
            APPL_TRACE_ERROR("bta_av_co_audio_set_codec A2D_BldSbcInfo failed");
            return false;
        }
        break;


    default:
        APPL_TRACE_ERROR("bta_av_co_audio_set_codec Feeding format unsupported");
        return false;
        break;
    }

    /* The new config was correctly built */
    bta_av_co_cb.codec_cfg = new_cfg;


    /* Check all devices support it */
    *p_status = BTIF_SUCCESS;
    return bta_av_co_audio_codec_supported(p_status);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_get_sbc_config
 **
 ** Description      Retrieves the SBC codec configuration.  If the codec in use
 **                  is not SBC, return the default SBC codec configuration.
 **
 ** Returns          true if codec is SBC, false otherwise
 **
 *******************************************************************************/
bool bta_av_co_audio_get_sbc_config(tA2D_SBC_CIE *p_sbc_config, uint16_t *p_minmtu)
{
    bool result = false;
    uint8_t index, jndex;
    tBTA_AV_CO_PEER *p_peer;
    tBTA_AV_CO_SINK *p_sink;

    APPL_TRACE_EVENT("bta_av_co_cb.codec_cfg.id : codec 0x%x", bta_av_co_cb.codec_cfg.id);

    /* Minimum MTU is by default very large */
    *p_minmtu = 0xFFFF;

    mutex_global_lock();
    if (bta_av_co_cb.codec_cfg.id == BTIF_AV_CODEC_SBC)
    {
        if (A2D_ParsSbcInfo(p_sbc_config, bta_av_co_cb.codec_cfg.info, false) == A2D_SUCCESS)
        {
            for (index = 0; index < BTA_AV_CO_NUM_ELEMENTS(bta_av_co_cb.peers); index++)
            {
                p_peer = &bta_av_co_cb.peers[index];
                if (p_peer->opened)
                {
                    if (p_peer->mtu < *p_minmtu)
                    {
                        *p_minmtu = p_peer->mtu;
                    }
                    for (jndex = 0; jndex < p_peer->num_sup_snks; jndex++)
                    {
                        p_sink = &p_peer->snks[jndex];
                        if (p_sink->codec_type == A2D_MEDIA_CT_SBC)
                        {
                            /* Update the bitpool boundaries of the current config */
                            p_sbc_config->min_bitpool =
                               BTA_AV_CO_MAX(p_sink->codec_caps[BTA_AV_CO_SBC_MIN_BITPOOL_OFF],
                                             p_sbc_config->min_bitpool);
                            p_sbc_config->max_bitpool =
                               BTA_AV_CO_MIN(p_sink->codec_caps[BTA_AV_CO_SBC_MAX_BITPOOL_OFF],
                                             p_sbc_config->max_bitpool);
                            APPL_TRACE_EVENT("bta_av_co_audio_get_sbc_config : sink bitpool min %d, max %d",
                                 p_sbc_config->min_bitpool, p_sbc_config->max_bitpool);
                            break;
                        }
                    }
                }
            }
            result = true;
        }
    }

    if (!result)
    {
        /* Not SBC, still return the default values */
        *p_sbc_config = btif_av_sbc_default_config;
    }
    mutex_global_unlock();

    return result;
}

/*******************************************************************************
 **
 ** Function         bta_av_co_audio_discard_config
 **
 ** Description      Discard the codec configuration of a connection
 **
 ** Returns          Nothing
 **
 *******************************************************************************/
void bta_av_co_audio_discard_config(tBTA_AV_HNDL hndl)
{
    tBTA_AV_CO_PEER *p_peer;

    FUNC_TRACE();

    /* Find the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_co_audio_discard_config could not find peer entry");
        return;
    }

    /* Reset the peer codec configuration */
    bta_av_co_audio_peer_reset_config(p_peer);
}

/*******************************************************************************
 **
 ** Function         bta_av_co_init
 **
 ** Description      Initialization
 **
 ** Returns          Nothing
 **
 *******************************************************************************/
void bta_av_co_init(void)
{
    FUNC_TRACE();

    /* Reset the control block */
    memset(&bta_av_co_cb, 0, sizeof(bta_av_co_cb));

    bta_av_co_cb.codec_cfg_setconfig.id = BTIF_AV_CODEC_NONE;

#if (BTA_AV_CO_CP_SCMS_T == TRUE)
    bta_av_co_cp_set_flag(BTA_AV_CP_SCMS_COPY_NEVER);
#else
    bta_av_co_cp_set_flag(BTA_AV_CP_SCMS_COPY_FREE);
#endif

    /* Reset the current config */
    bta_av_co_audio_codec_reset();
}


/*******************************************************************************
 **
 ** Function         bta_av_co_peer_cp_supported
 **
 ** Description      Checks if the peer supports CP
 **
 ** Returns          true if the peer supports CP
 **
 *******************************************************************************/
bool bta_av_co_peer_cp_supported(tBTA_AV_HNDL hndl)
{
    tBTA_AV_CO_PEER *p_peer;
    tBTA_AV_CO_SINK *p_sink;
    uint8_t index;

    FUNC_TRACE();

    /* Find the peer info */
    p_peer = bta_av_co_get_peer(hndl);
    if (p_peer == NULL)
    {
        APPL_TRACE_ERROR("bta_av_co_peer_cp_supported could not find peer entry");
        return false;
    }

    for (index = 0; index < p_peer->num_sup_snks; index++)
    {
        p_sink = &p_peer->snks[index];
        if (p_sink->codec_type == A2D_MEDIA_CT_SBC)
        {
            return bta_av_co_audio_sink_has_scmst(p_sink);
        }
    }
    APPL_TRACE_ERROR("bta_av_co_peer_cp_supported did not find SBC sink");
    return false;
}


/*******************************************************************************
 **
 ** Function         bta_av_co_get_remote_bitpool_pref
 **
 ** Description      Check if remote side did a setconfig within the limits
 **                  of our exported bitpool range. If set we will set the
 **                  remote preference.
 **
 ** Returns          true if config set, false otherwize
 **
 *******************************************************************************/

bool bta_av_co_get_remote_bitpool_pref(uint8_t *min, uint8_t *max)
{
    /* check if remote peer did a set config */
    if (bta_av_co_cb.codec_cfg_setconfig.id == BTIF_AV_CODEC_NONE)
        return false;

    *min = bta_av_co_cb.codec_cfg_setconfig.info[BTA_AV_CO_SBC_MIN_BITPOOL_OFF];
    *max = bta_av_co_cb.codec_cfg_setconfig.info[BTA_AV_CO_SBC_MAX_BITPOOL_OFF];

    return true;
}