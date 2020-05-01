/**
* Digital Voice Modem - Host Software
* GPLv2 Open Source. Use is subject to license terms.
* DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
*
* @package DVM / Host Software
*
*/
//
// Based on code from the MMDVMHost project. (https://github.com/g4klx/MMDVMHost)
// Licensed under the GPLv2 License (https://opensource.org/licenses/GPL-2.0)
//
/*
*   Copyright (C) 2017-2020 by Bryan Biedenkapp N2PLL
*
*   This program is free software; you can redistribute it and/or modify
*   it under the terms of the GNU General Public License as published by
*   the Free Software Foundation; either version 2 of the License, or
*   (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program; if not, write to the Free Software
*   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/
#include "Defines.h"
#include "p25/P25Defines.h"
#include "p25/TrunkPacket.h"
#include "p25/acl/AccessControl.h"
#include "p25/P25Utils.h"
#include "p25/Sync.h"
#include "edac/CRC.h"
#include "Log.h"
#include "Utils.h"

using namespace p25;

#include <cassert>
#include <cstdio>
#include <cstring>
#include <ctime>

// ---------------------------------------------------------------------------
//  Macros
// ---------------------------------------------------------------------------
// Make sure control data is supported.
#define IS_SUPPORT_CONTROL_CHECK(_PCKT_STR, _PCKT, _SRCID)                              \
    if (!m_p25->m_control) {                                                            \
        LogWarning(LOG_RF, P25_TSDU_STR ", " _PCKT_STR " denial, unsupported service, srcId = %u", _SRCID); \
        writeRF_TSDU_Deny(P25_DENY_RSN_SYS_UNSUPPORTED_SVC, _PCKT);                     \
        m_p25->checkAndReject();                                                        \
        return false;                                                                   \
    }

// Validate the source RID.
#define VALID_SRCID(_PCKT_STR, _PCKT, _SRCID)                                           \
    if (!acl::AccessControl::validateSrcId(_SRCID)) {                                   \
        LogWarning(LOG_RF, P25_TSDU_STR ", " _PCKT_STR " denial, RID rejection, srcId = %u", _SRCID); \
        writeRF_TSDU_Deny(P25_DENY_RSN_REQ_UNIT_NOT_VALID, _PCKT);                      \
        denialInhibit(_SRCID);                                                          \
        m_p25->checkAndReject();                                                        \
        return false;                                                                   \
    }

// Validate the target RID.
#define VALID_DSTID(_PCKT_STR, _PCKT, _DSTID)                                           \
    if (!acl::AccessControl::validateSrcId(_DSTID)) {                                   \
        LogWarning(LOG_RF, P25_TSDU_STR ", " _PCKT_STR " denial, RID rejection, dstId = %u", _DSTID); \
        writeRF_TSDU_Deny(P25_DENY_RSN_TGT_UNIT_NOT_VALID, _PCKT);                      \
        m_p25->checkAndReject();                                                        \
        return false;                                                                   \
    }

// Validate the talkgroup ID.
#define VALID_TGID(_PCKT_STR, _PCKT, _DSTID)                                            \
    if (!acl::AccessControl::validateTGId(_DSTID)) {                                    \
        LogWarning(LOG_RF, P25_TSDU_STR ", " _PCKT_STR " denial, TGID rejection, dstId = %u", _DSTID); \
        writeRF_TSDU_Deny(P25_DENY_RSN_TGT_GROUP_NOT_VALID, _PCKT);                     \
        m_p25->checkAndReject();                                                        \
        return false;                                                                   \
    }

// Verify the source RID is registered.
#define VERIFY_SRCID_REG(_PCKT_STR, _PCKT, _SRCID)                                      \
    if (!hasSrcIdUnitReg(_SRCID) && m_verifyReg) {                                      \
        LogWarning(LOG_RF, P25_TSDU_STR ", " _PCKT_STR " denial, RID not registered, srcId = %u", _SRCID); \
        writeRF_TSDU_Deny(P25_DENY_RSN_REQ_UNIT_NOT_AUTH, _PCKT);                       \
        writeRF_TSDU_U_Reg_Cmd(_SRCID);                                                 \
        m_p25->checkAndReject();                                                        \
        return false;                                                                   \
    }

// Verify the source RID is affiliated.
#define VERIFY_SRCID_AFF(_PCKT_STR, _PCKT, _SRCID, _DSTID)                              \
    if (!hasSrcIdGrpAff(_SRCID, _DSTID) && m_verifyAff) {                               \
        LogWarning(LOG_RF, P25_TSDU_STR ", " _PCKT_STR " denial, RID not affiliated to TGID, srcId = %u, dstId = %u", _SRCID, _DSTID); \
        writeRF_TSDU_Deny(P25_DENY_RSN_REQ_UNIT_NOT_AUTH, _PCKT);                       \
        writeRF_TSDU_U_Reg_Cmd(_SRCID);                                                 \
        m_p25->checkAndReject();                                                        \
        return false;                                                                   \
    }

// Validate the source RID (network).
#define VALID_SRCID_NET(_PCKT_STR, _SRCID)                                              \
    if (!acl::AccessControl::validateSrcId(_SRCID)) {                                   \
        LogWarning(LOG_NET, P25_TSDU_STR ", " _PCKT_STR " denial, RID rejection, srcId = %u", _SRCID); \
        return false;                                                                   \
    }

// Validate the target RID (network).
#define VALID_DSTID_NET(_PCKT_STR, _DSTID)                                              \
    if (!acl::AccessControl::validateSrcId(_DSTID)) {                                   \
        LogWarning(LOG_NET, P25_TSDU_STR ", " _PCKT_STR " denial RID rejection, dstId = %u", _DSTID); \
        return false;                                                                   \
    }

#define RF_TO_WRITE_NET()                                                               \
    if (m_network != NULL) {                                                            \
        uint8_t _buf[P25_TSDU_FRAME_LENGTH_BYTES];                                      \
        writeNet_TSDU_From_RF(_buf);                                                    \
        writeNetworkRF(_buf, true);                                                     \
    }

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

const uint32_t ADJ_SITE_TIMER_TIMEOUT = 30U;
const uint32_t ADJ_SITE_UPDATE_CNT = 5U;
const uint32_t TSDU_CTRL_BURST_COUNT = 2U;
const uint32_t TSBK_MBF_CNT = 3U;
const uint32_t GRANT_TIMER_TIMEOUT = 15U;

// ---------------------------------------------------------------------------
//  Public Class Members
// ---------------------------------------------------------------------------
/// <summary>
/// Sets local configured site data.
/// </summary>
/// <param name="netId">P25 Network ID.</param>
/// <param name="sysId">P25 System ID.</param>
/// <param name="rfssId">P25 RFSS ID.</param>
/// <param name="siteId">P25 Site ID.</param>
/// <param name="lra">P25 Location Resource Area.</param>
/// <param name="channelId">Channel ID.</param>
/// <param name="channelNo">Channel Number.</param>
void TrunkPacket::setSiteData(uint32_t netId, uint32_t sysId, uint8_t rfssId, uint8_t siteId, uint8_t lra,
    uint8_t channelId, uint32_t channelNo)
{
    m_siteData = SiteData(netId, sysId, rfssId, siteId, lra, channelId, channelNo);

    m_rfTSBK.setSiteData(m_siteData);
    m_rfTDULC.setSiteData(m_siteData);
    m_p25->m_voice->m_rfLC.setSiteData(m_siteData);

    m_netTSBK.setSiteData(m_siteData);
    m_netTDULC.setSiteData(m_siteData);
    m_p25->m_voice->m_netLC.setSiteData(m_siteData);
}

/// <summary>
/// Sets local configured site callsign.
/// </summary>
/// <param name="callsign"></param>
void TrunkPacket::setCallsign(std::string callsign)
{
    m_rfTSBK.setCallsign(callsign);
    m_netTSBK.setCallsign(callsign);
}

/// <summary>
/// Sets a flag indicating whether or not networking is active.
/// </summary>
/// <param name="active"></param>
void TrunkPacket::setNetActive(bool active)
{
    m_rfTSBK.setNetActive(active);
    m_rfTDULC.setNetActive(active);
    m_netTSBK.setNetActive(active);
    m_netTDULC.setNetActive(active);
}

/// <summary>
/// Sets the total number of channels at the site.
/// </summary>
/// <param name="chCnt"></param>
void TrunkPacket::setSiteChCnt(uint8_t chCnt)
{
    m_rfTSBK.setSiteChCnt(chCnt);
    m_netTSBK.setSiteChCnt(chCnt);
}

/// <summary>
/// Resets the data states for the RF interface.
/// </summary>
void TrunkPacket::resetRF()
{
    m_rfTSBK.reset();
    m_rfTDULC.reset();
}

/// <summary>
/// Resets the data states for the network.
/// </summary>
void TrunkPacket::resetNet()
{
    m_netTSBK.reset();
    m_netTDULC.reset();
}

/// <summary>
/// Sets the RF TSBK and TDULC data to match the given LC data.
/// </summary>
/// <param name="lc"></param>
void TrunkPacket::setRFLC(const lc::LC& lc)
{
    m_rfTSBK.reset();
    m_rfTDULC.reset();

    m_rfTSBK.setProtect(lc.getProtect());
    m_rfTDULC.setProtect(lc.getProtect());
    m_rfTSBK.setMFId(lc.getMFId());
    m_rfTDULC.setMFId(lc.getMFId());

    m_rfTSBK.setSrcId(lc.getSrcId());
    m_rfTDULC.setSrcId(lc.getSrcId());
    m_rfTSBK.setDstId(lc.getDstId());
    m_rfTDULC.setDstId(lc.getDstId());

    m_rfTSBK.setGrpVchNo(lc.getGrpVchNo());
    m_rfTDULC.setGrpVchNo(lc.getGrpVchNo());

    m_rfTSBK.setEmergency(lc.getEmergency());
    m_rfTDULC.setEmergency(lc.getEmergency());
    m_rfTSBK.setEncrypted(lc.getEncrypted());
    m_rfTDULC.setEncrypted(lc.getEmergency());
    m_rfTSBK.setPriority(lc.getPriority());
    m_rfTDULC.setPriority(lc.getPriority());

    m_rfTSBK.setGroup(lc.getGroup());
    m_rfTDULC.setGroup(lc.getGroup());
}

/// <summary>
/// Sets the network TSBK and TDULC data to match the given LC data.
/// </summary>
/// <param name="lc"></param>
void TrunkPacket::setNetLC(const lc::LC& lc)
{
    m_netTSBK.reset();
    m_netTDULC.reset();

    m_netTSBK.setProtect(lc.getProtect());
    m_netTDULC.setProtect(lc.getProtect());
    m_netTSBK.setMFId(lc.getMFId());
    m_netTDULC.setMFId(lc.getMFId());

    m_netTSBK.setSrcId(lc.getSrcId());
    m_netTDULC.setSrcId(lc.getSrcId());
    m_netTSBK.setDstId(lc.getDstId());
    m_netTDULC.setDstId(lc.getDstId());

    m_netTSBK.setGrpVchNo(lc.getGrpVchNo());
    m_netTDULC.setGrpVchNo(lc.getGrpVchNo());

    m_netTSBK.setEmergency(lc.getEmergency());
    m_netTDULC.setEmergency(lc.getEmergency());
    m_netTSBK.setEncrypted(lc.getEncrypted());
    m_netTDULC.setEncrypted(lc.getEmergency());
    m_netTSBK.setPriority(lc.getPriority());
    m_netTDULC.setPriority(lc.getPriority());

    m_netTSBK.setGroup(lc.getGroup());
    m_netTDULC.setGroup(lc.getGroup());
}

/// <summary>
/// Process a data frame from the RF interface.
/// </summary>
/// <param name="data">Buffer containing data frame.</param>
/// <param name="len">Length of data frame.</param>
/// <returns></returns>
bool TrunkPacket::process(uint8_t* data, uint32_t len)
{
    assert(data != NULL);

    // Decode the NID
    bool valid = m_p25->m_nid.decode(data + 2U);

    if (m_p25->m_rfState == RS_RF_LISTENING && !valid)
        return false;

    RPT_RF_STATE prevRfState = m_p25->m_rfState;
    uint8_t duid = m_p25->m_nid.getDUID();

    // handle individual DUIDs
    if (duid == P25_DUID_TSDU) {
        if (m_p25->m_rfState != RS_RF_DATA) {
            m_p25->m_rfState = RS_RF_DATA;
        }

        m_p25->m_queue.clear();
        m_rfTSBK.reset();
        m_netTSBK.reset();

        bool ret = m_rfTSBK.decode(data + 2U);
        if (!ret) {
            LogWarning(LOG_RF, P25_TSDU_STR ", undecodable LC");
            m_p25->m_rfState = prevRfState;
            return false;
        }

        uint32_t srcId = m_rfTSBK.getSrcId();
        uint32_t dstId = m_rfTSBK.getDstId();

        resetStatusCommand(m_rfTSBK);

        m_p25->writeRF_Preamble();

        switch (m_rfTSBK.getLCO()) {
            case TSBK_IOSP_GRP_VCH:
                // make sure control data is supported
                IS_SUPPORT_CONTROL_CHECK("TSBK_IOSP_GRP_VCH (Group Voice Channel Request)", TSBK_IOSP_GRP_VCH, srcId);

                // validate the source RID
                VALID_SRCID("TSBK_IOSP_GRP_VCH (Group Voice Channel Request)", TSBK_IOSP_GRP_VCH, srcId);

                // validate the talkgroup ID
                VALID_TGID("TSBK_IOSP_GRP_VCH (Group Voice Channel Request)", TSBK_IOSP_GRP_VCH, dstId);

                // verify the source RID is affiliated
                VERIFY_SRCID_AFF("TSBK_IOSP_GRP_VCH (Group Voice Channel Request)", TSBK_IOSP_GRP_VCH, srcId, dstId);

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_GRP_VCH (Group Voice Channel Request), srcId = %u, dstId = %u", srcId, dstId);
                }

                ::ActivityLog("P25", true, "received group grant request from %u to TG %u", srcId, dstId);

                writeRF_TSDU_Grant(true, false);
                break;
            case TSBK_IOSP_UU_VCH:
                // make sure control data is supported
                IS_SUPPORT_CONTROL_CHECK("TSBK_IOSP_UU_VCH (Unit-to-Unit Voice Channel Request)", TSBK_IOSP_UU_VCH, srcId);

                // validate the source RID
                VALID_SRCID("TSBK_IOSP_UU_VCH (Unit-to-Unit Voice Channel Request)", TSBK_IOSP_UU_VCH, srcId);

                // validate the target RID
                VALID_DSTID("TSBK_IOSP_UU_VCH (Unit-to-Unit Voice Channel Request)", TSBK_IOSP_UU_VCH, dstId);

                // verify the source RID is registered
                VERIFY_SRCID_REG("TSBK_IOSP_UU_VCH (Unit-to-Unit Voice Channel Request)", TSBK_IOSP_UU_VCH, srcId);

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_UU_VCH (Unit-to-Unit Voice Channel Request), srcId = %u, dstId = %u", srcId, dstId);
                }

                ::ActivityLog("P25", true, "received unit-to-unit grant request from %u to %u", srcId, dstId);

                writeRF_TSDU_UU_Ans_Req(srcId, dstId);
                break;
            case TSBK_IOSP_UU_ANS:
                // make sure control data is supported
                IS_SUPPORT_CONTROL_CHECK("TSBK_IOSP_UU_ANS (Unit-to-Unit Answer Response)", TSBK_IOSP_UU_ANS, srcId);

                // validate the source RID
                VALID_SRCID("TSBK_IOSP_UU_ANS (Unit-to-Unit Answer Response)", TSBK_IOSP_UU_ANS, srcId);

                // validate the target RID
                VALID_DSTID("TSBK_IOSP_UU_ANS (Unit-to-Unit Answer Response)", TSBK_IOSP_UU_ANS, dstId);

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_UU_ANS (Unit-to-Unit Answer Response), response = $%02X, srcId = %u, dstId = %u", 
                        m_rfTSBK.getResponse(), srcId, dstId);
                }

                writeRF_TSDU_ACK_FNE(srcId, TSBK_IOSP_UU_ANS, true);

                if (m_rfTSBK.getResponse() == P25_ANS_RSP_PROCEED) {
                    writeRF_TSDU_Grant(false, false);
                }
                else if (m_rfTSBK.getResponse() == P25_ANS_RSP_DENY) {
                    writeRF_TSDU_Deny(P25_DENY_RSN_TGT_UNIT_REFUSED, TSBK_IOSP_UU_ANS);
                }
                else if (m_rfTSBK.getResponse() == P25_ANS_RSP_WAIT) {
                    writeRF_TSDU_Queue(P25_QUE_RSN_TGT_UNIT_QUEUED, TSBK_IOSP_UU_ANS);
                }
                break;
            case TSBK_IOSP_TELE_INT_ANS:
                // make sure control data is supported
                IS_SUPPORT_CONTROL_CHECK("TSBK_IOSP_TELE_INT_ANS (Telephone Interconnect Answer Response)", TSBK_IOSP_TELE_INT_ANS, srcId);

                // validate the source RID
                VALID_SRCID("TSBK_IOSP_TELE_INT_ANS (Telephone Interconnect Answer Response)", TSBK_IOSP_TELE_INT_ANS, srcId);

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_TELE_INT_ANS (Telephone Interconnect Answer Response), response = $%02X, srcId = %u",
                        m_rfTSBK.getResponse(), srcId);
                }

                writeRF_TSDU_ACK_FNE(srcId, TSBK_IOSP_TELE_INT_ANS, true);

                if (m_rfTSBK.getResponse() == P25_ANS_RSP_PROCEED) {
                    //writeRF_TSDU_Grant(false);
                    writeRF_TSDU_Deny(P25_DENY_RSN_SYS_UNSUPPORTED_SVC, TSBK_IOSP_TELE_INT_ANS);
                }
                else if (m_rfTSBK.getResponse() == P25_ANS_RSP_DENY) {
                    writeRF_TSDU_ACK_FNE(srcId, TSBK_IOSP_TELE_INT_ANS, true);
                }
                else if (m_rfTSBK.getResponse() == P25_ANS_RSP_WAIT) {
                    writeRF_TSDU_Queue(P25_QUE_RSN_TGT_UNIT_QUEUED, TSBK_IOSP_TELE_INT_ANS);
                }
                break;
            case TSBK_IOSP_STS_UPDT:
                // validate the source RID
                VALID_SRCID("TSBK_IOSP_STS_UPDT (Status Update)", TSBK_IOSP_STS_UPDT, srcId);

                if ((m_statusSrcId == 0U) && (m_statusValue == 0U)) {
                    RF_TO_WRITE_NET();
                }

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_STS_UPDT (Status Update), status = $%02X, srcId = %u", m_rfTSBK.getStatus(), srcId);
                }

                ::ActivityLog("P25", true, "received status update from %u", srcId);

                if (!m_noStatusAck) {
                    writeRF_TSDU_ACK_FNE(srcId, TSBK_IOSP_STS_UPDT, false);
                }

                if (m_statusCmdEnable) {
                    preprocessStatusCommand();
                }
                break;
            case TSBK_IOSP_MSG_UPDT:
                // validate the source RID
                VALID_SRCID("TSBK_IOSP_MSG_UPDT (Message Update)", TSBK_IOSP_MSG_UPDT, srcId);

                RF_TO_WRITE_NET();

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_MSG_UPDT (Message Update), message = $%02X, srcId = %u, dstId = %u", 
                        m_rfTSBK.getMessage(), srcId, dstId);
                }

                if (!m_noMessageAck) {
                    writeRF_TSDU_ACK_FNE(srcId, TSBK_IOSP_MSG_UPDT, false);
                }

                ::ActivityLog("P25", true, "received message update from %u", srcId);
                break;
            case TSBK_IOSP_CALL_ALRT:
                // validate the source RID
                VALID_SRCID("TSBK_IOSP_CALL_ALRT (Call Alert)", TSBK_IOSP_CALL_ALRT, srcId);

                // is status command mode enabled with status data?
                if (m_statusCmdEnable) {
                    if (processStatusCommand(srcId, dstId)) {
                        m_p25->m_rfState = prevRfState;
                        return true;
                    }

                    resetStatusCommand();
                }

                // validate the target RID
                VALID_DSTID("TSBK_IOSP_CALL_ALRT (Call Alert)", TSBK_IOSP_CALL_ALRT, dstId);

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_CALL_ALRT (Call Alert), srcId = %u, dstId = %u", srcId, dstId);
                }

                ::ActivityLog("P25", true, "received call alert request from %u to %u", srcId, dstId);

                writeRF_TSDU_Call_Alrt(srcId, dstId);
                break;
            case TSBK_IOSP_ACK_RSP:
                // validate the source RID
                VALID_SRCID("TSBK_IOSP_ACK_RSP (Acknowledge Response)", TSBK_IOSP_ACK_RSP, srcId);

                // validate the target RID
                VALID_DSTID("TSBK_IOSP_ACK_RSP (Acknowledge Response)", TSBK_IOSP_ACK_RSP, dstId);

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_ACK_RSP (Acknowledge Response), AIV = %u, serviceType = $%02X, srcId = %u, dstId = %u", 
                        m_rfTSBK.getAIV(), m_rfTSBK.getService(), srcId, dstId);
                }

                ::ActivityLog("P25", true, "received ack response from %u to %u", srcId, dstId);

                // bryanb: HACK -- for some reason, if the AIV is false and we have a dstId
                // its very likely srcId and dstId are swapped so we'll swap them
                if (!m_rfTSBK.getAIV() && dstId != 0U) {
                    m_rfTSBK.setAIV(true);
                    m_rfTSBK.setSrcId(dstId);
                    m_rfTSBK.setDstId(srcId);
                }

                writeRF_TSDU_SBF(false);
                break;
            case TSBK_ISP_CAN_SRV_REQ:
                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_ISP_CAN_SRV_REQ (Cancel Service Request), AIV = %u, serviceType = $%02X, reason = $%02X, srcId = %u, dstId = %u",
                        m_rfTSBK.getAIV(), m_rfTSBK.getService(), m_rfTSBK.getResponse(), srcId, dstId);
                }

                ::ActivityLog("P25", true, "received cancel service request from %u", srcId);

                writeRF_TSDU_ACK_FNE(srcId, TSBK_ISP_CAN_SRV_REQ, true);
                break;
            case TSBK_IOSP_EXT_FNCT:
                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_EXT_FNCT (Extended Function), op = $%02X, arg = %u, tgt = %u", 
                        m_rfTSBK.getExtendedFunction(), dstId, srcId);
                }

                // is status control mode enabled with status data?
                if (m_statusCmdEnable && (m_statusValue != 0U)) {
                    m_rfTSBK.setLCO(TSBK_IOSP_ACK_RSP);
                    m_rfTSBK.setAIV(true);
                    m_rfTSBK.setService(TSBK_IOSP_CALL_ALRT);

                    if (m_verbose) {
                        LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_ACK_RSP (Acknowledge Response), serviceType = $%02X, srcId = %u", 
                            m_rfTSBK.getService(), m_statusSrcId);
                    }
                }

                // generate activity log entry
                if (m_rfTSBK.getExtendedFunction() == P25_EXT_FNCT_CHECK_ACK) {
                    ::ActivityLog("P25", true, "received radio check response from %u to %u", dstId, srcId);
                }
                else if (m_rfTSBK.getExtendedFunction() == P25_EXT_FNCT_INHIBIT_ACK) {
                    ::ActivityLog("P25", true, "received radio inhibit response from %u to %u", dstId, srcId);
                }
                else if (m_rfTSBK.getExtendedFunction() == P25_EXT_FNCT_UNINHIBIT_ACK) {
                    ::ActivityLog("P25", true, "received radio uninhibit response from %u to %u", dstId, srcId);
                }

                writeRF_TSDU_SBF(true);
                resetStatusCommand();
                break;
            case TSBK_IOSP_GRP_AFF:
                // make sure control data is supported
                IS_SUPPORT_CONTROL_CHECK("TSBK_IOSP_GRP_AFF (Group Affiliation Request)", TSBK_IOSP_GRP_AFF, srcId);

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_GRP_AFF (Group Affiliation Request), srcId = %u, dstId = %u", srcId, dstId);
                }

                writeRF_TSDU_ACK_FNE(srcId, TSBK_IOSP_GRP_AFF, true);
                writeRF_TSDU_Grp_Aff_Rsp(srcId, dstId);
                break;
            case TSBK_ISP_GRP_AFF_Q_RSP:
                // make sure control data is supported
                IS_SUPPORT_CONTROL_CHECK("TSBK_IOSP_GRP_AFF (Group Affiliation Query Response)", TSBK_ISP_GRP_AFF_Q_RSP, srcId);

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_GRP_AFF (Group Affiliation Query Response), srcId = %u, dstId = %u, anncId = %u", srcId, dstId, 
                        m_rfTSBK.getPatchSuperGroupId());
                }

                ::ActivityLog("P25", true, "received group affiliation query response from %u to %s %u", srcId, "TG ", dstId);
                break;
            case TSBK_ISP_U_DEREG_REQ:
                // make sure control data is supported
                IS_SUPPORT_CONTROL_CHECK("TSBK_ISP_U_DEREG_REQ (Unit Deregistration Request)", TSBK_ISP_U_DEREG_REQ, srcId);

                // validate the source RID
                VALID_SRCID("TSBK_ISP_U_DEREG_REQ (Unit Deregistration Request)", TSBK_ISP_U_DEREG_REQ, srcId);

                // HACK: ensure the DEREG_REQ transmits something ...
                if (dstId == 0U) {
                    dstId = P25_WUID_SYS;
                }

                writeRF_TSDU_ACK_FNE(srcId, TSBK_ISP_U_DEREG_REQ, true);
                writeRF_TSDU_U_Dereg_Ack(srcId);
                break;
            case TSBK_IOSP_U_REG:
                // make sure control data is supported
                IS_SUPPORT_CONTROL_CHECK("TSBK_ISP_U_REG_REQ (Unit Registration Request)", TSBK_IOSP_U_REG, srcId);

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_ISP_U_REG_REQ (Unit Registration Request), srcId = %u", srcId);
                }

                writeRF_TSDU_ACK_FNE(srcId, TSBK_IOSP_U_REG, true);
                writeRF_TSDU_U_Reg_Rsp(srcId);
                break;
            case TSBK_ISP_LOC_REG_REQ:
                // make sure control data is supported
                IS_SUPPORT_CONTROL_CHECK("TSBK_ISP_LOC_REG_REQ (Location Registration Request)", TSBK_ISP_LOC_REG_REQ, srcId);

                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_ISP_LOC_REG_REQ (Location Registration Request), srcId = %u, dstId = %u", srcId, dstId);
                }

                writeRF_TSDU_U_Reg_Cmd(srcId);
                break;
            default:
                LogError(LOG_RF, P25_TSDU_STR ", unhandled LCO, mfId = $%02X, lco = $%02X", m_rfTSBK.getMFId(), m_rfTSBK.getLCO());
                break;
        }

        // add trailing null pad; only if control data isn't being transmitted
        if (!m_p25->m_ccRunning) {
            m_p25->writeRF_Nulls();
        }

        m_p25->m_rfState = prevRfState;
        return true;
    }
    else {
        LogError(LOG_RF, "P25 unhandled data DUID, duid = $%02X", duid);
    }

    return false;
}

/// <summary>
/// Process a data frame from the network.
/// </summary>
/// <param name="data">Buffer containing data frame.</param>
/// <param name="len">Length of data frame.</param>
/// <param name="control"></param>
/// <param name="lsd"></param>
/// <param name="duid"></param>
/// <returns></returns>
bool TrunkPacket::processNetwork(uint8_t* data, uint32_t len, lc::LC& control, data::LowSpeedData& lsd, uint8_t& duid)
{
    if (m_p25->m_rfState != RS_RF_LISTENING && m_p25->m_netState == RS_NET_IDLE)
        return false;

    switch (duid) {
        case P25_DUID_TSDU:
            if (m_p25->m_netState == RS_NET_IDLE) {
                m_rfTSBK.reset();
                m_netTSBK.reset();

                bool ret = m_netTSBK.decode(data);
                if (!ret) {
                    return false;
                }

                // handle updating internal adjacent site information
                if (m_netTSBK.getLCO() == TSBK_OSP_ADJ_STS_BCAST) {
                    if (!m_p25->m_control) {
                        return false;
                    }

                    if (m_netTSBK.getAdjSiteId() != m_siteData.siteId()) {
                        // update site table data
                        SiteData site;
                        try {
                            site = m_adjSiteTable.at(m_netTSBK.getAdjSiteId());
                        } catch (...) {
                            site = SiteData();
                        }

                        if (m_verbose) {
                            LogMessage(LOG_NET, P25_TSDU_STR ", TSBK_OSP_ADJ_STS_BCAST (Adjacent Site Status Broadcast), sysId = $%03X, rfss = $%02X, site = $%02X, chId = %u, chNo = %u",
                                m_netTSBK.getAdjSiteSysId(), m_netTSBK.getAdjSiteRFSSId(), m_netTSBK.getAdjSiteId(), m_netTSBK.getAdjSiteChnId(), m_netTSBK.getAdjSiteChnNo());
                        }

                        site.setAdjSite(m_netTSBK.getAdjSiteSysId(), m_netTSBK.getAdjSiteRFSSId(),
                            m_netTSBK.getAdjSiteId(), m_netTSBK.getAdjSiteChnId(), m_netTSBK.getAdjSiteChnNo());

                        m_adjSiteTable[site.siteId()] = site;
                        m_adjSiteUpdateCnt[site.siteId()] = ADJ_SITE_UPDATE_CNT;
                    }

                    return true;
                }

                uint32_t srcId = m_netTSBK.getSrcId();
                uint32_t dstId = m_netTSBK.getDstId();

                resetStatusCommand(m_netTSBK);

                switch (m_netTSBK.getLCO()) {
                    case TSBK_IOSP_UU_ANS:
                        if (m_netTSBK.getResponse() > 0U) {
                            if (m_verbose) {
                                LogMessage(LOG_NET, P25_TSDU_STR ", TSBK_IOSP_UU_ANS (Unit-to-Unit Answer Response), response = $%02X, srcId = %u, dstId = %u",
                                    m_netTSBK.getResponse(), srcId, dstId);
                            }
                        }
                        else {
                            if (m_verbose) {
                                LogMessage(LOG_NET, P25_TSDU_STR ", TSBK_IOSP_UU_ANS (Unit-to-Unit Answer Request), srcId = %u, dstId = %u", srcId, dstId);
                            }
                        }
                        break;
                    case TSBK_IOSP_STS_UPDT:
                        // validate the source RID
                        VALID_SRCID_NET("TSBK_IOSP_STS_UPDT (Status Update)", srcId);

                        if (m_verbose) {
                            LogMessage(LOG_NET, P25_TSDU_STR ", TSBK_IOSP_STS_UPDT (Status Update), status = $%02X, srcId = %u", 
                                m_netTSBK.getStatus(), srcId);
                        }

                        ::ActivityLog("P25", false, "received status update from %u", srcId);
                        break;
                    case TSBK_IOSP_MSG_UPDT:
                        // validate the source RID
                        VALID_SRCID_NET("TSBK_IOSP_MSG_UPDT (Message Update)", srcId);

                        if (m_verbose) {
                            LogMessage(LOG_NET, P25_TSDU_STR ", TSBK_IOSP_MSG_UPDT (Message Update), message = $%02X, srcId = %u, dstId = %u", 
                                m_netTSBK.getMessage(), srcId, dstId);
                        }

                        ::ActivityLog("P25", false, "received message update from %u", srcId);
                        break;
                    case TSBK_IOSP_CALL_ALRT:
                        // validate the source RID
                        VALID_SRCID_NET("TSBK_IOSP_CALL_ALRT (Call Alert)", srcId);

                        // validate the target RID
                        VALID_DSTID_NET("TSBK_IOSP_CALL_ALRT (Call Alert)", dstId);

                        // validate source RID
                        if (!acl::AccessControl::validateSrcId(srcId)) {
                            LogWarning(LOG_NET, "P25_DUID_TSDU (Trunking System Data Unit) denial, RID rejection, srcId = %u", srcId);
                            return false;
                        }

                        if (m_verbose) {
                            LogMessage(LOG_NET, P25_TSDU_STR ", TSBK_IOSP_CALL_ALRT (Call Alert), srcId = %u, dstId = %u", srcId, dstId);
                        }

                        ::ActivityLog("P25", false, "received call alert request from %u to %u", srcId, dstId);
                        break;
                    case TSBK_IOSP_ACK_RSP:
                        // validate the source RID
                        VALID_SRCID_NET("TSBK_IOSP_ACK_RSP (Acknowledge Response)", srcId);

                        // validate the target RID
                        VALID_DSTID_NET("TSBK_IOSP_ACK_RSP (Acknowledge Response)", dstId);

                        if (m_verbose) {
                            LogMessage(LOG_NET, P25_TSDU_STR ", TSBK_IOSP_ACK_RSP (Acknowledge Response), AIV = %u, serviceType = $%02X, srcId = %u, dstId = %u", 
                                m_netTSBK.getAIV(), m_netTSBK.getService(), dstId, srcId);
                        }

                        ::ActivityLog("P25", false, "received ack response from %u to %u", srcId, dstId);
                        break;
                    case TSBK_IOSP_EXT_FNCT:
                        // validate the target RID
                        VALID_DSTID_NET("TSBK_IOSP_EXT_FNCT (Extended Function)", dstId);

                        if (m_verbose) {
                            LogMessage(LOG_NET, P25_TSDU_STR ", TSBK_IOSP_EXT_FNCT (Extended Function), serviceType = $%02X, arg = %u, tgt = %u", 
                                m_netTSBK.getService(), srcId, dstId);
                        }

                        resetStatusCommand();
                        break;
                    case TSBK_IOSP_GRP_AFF:
                        // ignore a network group affiliation command
                        break;
                    case TSBK_OSP_U_DEREG_ACK:
                        // ignore a network user deregistration command
                        break;
                    case TSBK_OSP_DENY_RSP:
                        if (m_verbose) {
                            LogMessage(LOG_NET, P25_TSDU_STR ", TSBK_OSP_DENY_RSP (Deny Response), AIV = %u, reason = $%02X, srcId = %u, dstId = %u",
                                m_netTSBK.getAIV(), m_netTSBK.getResponse(), m_netTSBK.getSrcId(), m_netTSBK.getDstId());
                        }
                        break;
                    case TSBK_OSP_QUE_RSP:
                        if (m_verbose) {
                            LogMessage(LOG_NET, P25_TSDU_STR ", TSBK_OSP_QUE_RSP (Queue Response), AIV = %u, reason = $%02X, srcId = %u, dstId = %u",
                                m_netTSBK.getAIV(), m_netTSBK.getResponse(), m_netTSBK.getSrcId(), m_netTSBK.getDstId());
                        }
                        break;
                    default:
                        LogError(LOG_NET, P25_TSDU_STR ", unhandled LCO, mfId = $%02X, lco = $%02X", m_netTSBK.getMFId(), m_netTSBK.getLCO());
                        return false;
                }

                writeNet_TSDU();
            }
            break;
        default:
            return false;
    }

    return true;
}

/// <summary>
/// Helper to write P25 adjacent site information to the network.
/// </summary>
void TrunkPacket::writeAdjSSNetwork()
{
    if (!m_p25->m_control) {
        return;
    }

    m_rfTSBK.reset();
    m_netTSBK.reset();

    if (m_network != NULL) {
        if (m_verbose) {
            LogMessage(LOG_NET, P25_TSDU_STR ", TSBK_OSP_ADJ_STS_BCAST (Adjacent Site Status Broadcast), network announce, sysId = $%03X, rfss = $%02X, site = $%02X, chId = %u, chNo = %u", 
                m_siteData.sysId(), m_siteData.rfssId(), m_siteData.siteId(), m_siteData.channelId(), m_siteData.channelNo());
        }

        // transmit adjacent site broadcast
        m_rfTSBK.setLCO(TSBK_OSP_ADJ_STS_BCAST);
        m_rfTSBK.setAdjSiteCFVA(P25_CFVA_CONV | P25_CFVA_VALID);
        m_rfTSBK.setAdjSiteSysId(m_siteData.sysId());
        m_rfTSBK.setAdjSiteRFSSId(m_siteData.rfssId());
        m_rfTSBK.setAdjSiteId(m_siteData.siteId());
        m_rfTSBK.setAdjSiteChnId(m_siteData.channelId());
        m_rfTSBK.setAdjSiteChnNo(m_siteData.channelNo());
        
        RF_TO_WRITE_NET();
    }
}

/// <summary>
/// Helper to determine if the source ID has affiliated to the group destination ID.
/// </summary>
/// <param name="srcId"></param>
/// <param name="dstId"></param>
/// <returns></returns>
bool TrunkPacket::hasSrcIdGrpAff(uint32_t srcId, uint32_t dstId) const
{
    // lookup dynamic affiliation table entry
    try {
        uint32_t tblDstId = m_grpAffTable.at(srcId);
        if (tblDstId == dstId) {
            return true;
        }
        else {
            return false;
        }
    } catch (...) {
        return false;
    }
}

/// <summary>
/// Helper to determine if the source ID has unit registered.
/// </summary>
/// <param name="srcId"></param>
/// <returns></returns>
bool TrunkPacket::hasSrcIdUnitReg(uint32_t srcId) const
{
    // lookup dynamic unit registration table entry
    if (std::find(m_unitRegTable.begin(), m_unitRegTable.end(), srcId) != m_unitRegTable.end()) {
        return true;
    }
    else {
        return false;
    }
}

/// <summary>
/// Helper to determine if the channel number is busy.
/// </summary>
/// <param name="chNo"></param>
/// <returns></returns>
bool TrunkPacket::isChBusy(uint32_t chNo) const
{
    if (chNo == 0U) {
        return false;
    }

    // lookup dynamic channel grant table entry
    for (auto it = m_grantChTable.begin(); it != m_grantChTable.end(); ++it) {
        if (it->second == chNo) {
            return true;
        }
    }

    return false;
}

/// <summary>
/// Helper to determine if the destination ID is already granted.
/// </summary>
/// <param name="dstId"></param>
/// <returns></returns>
bool TrunkPacket::hasDstIdGranted(uint32_t dstId) const
{
    if (dstId == 0U) {
        return false;
    }

    // lookup dynamic channel grant table entry
    try {
        uint32_t chNo = m_grantChTable.at(dstId);
        if (chNo != 0U) {
            return true;
        }
        else {
            return false;
        }
    } catch (...) {
        return false;
    }
}

/// <summary>
/// Helper to start the destination ID grant timer.
/// </summary>
/// <param name="dstId"></param>
/// <returns></returns>
void TrunkPacket::touchDstIdGrant(uint32_t dstId)
{
    if (dstId == 0U) {
        return;
    }

    if (hasDstIdGranted(dstId)) {
        m_grantTimers[dstId].start();
    }
}

/// <summary>
/// Helper to release the channel grant for the destination ID.
/// </summary>
/// <param name="dstId"></param>
/// <param name="releaseAll"></param>
void TrunkPacket::releaseDstIdGrant(uint32_t dstId, bool releaseAll)
{
    if (dstId == 0U && !releaseAll) {
        return;
    }

    if (dstId == 0U && releaseAll) {
        LogWarning(LOG_RF, "P25, force releasing all channel grants");

        std::vector<uint32_t> gntsToRel = std::vector<uint32_t>();
        for (auto it = m_grantChTable.begin(); it != m_grantChTable.end(); ++it) {
            uint32_t dstId = it->first;
            gntsToRel.push_back(dstId);
        }

        // release grants
        for (auto it = gntsToRel.begin(); it != gntsToRel.end(); ++it) {
            releaseDstIdGrant(*it, false);
        }

        return;
    }

    if (hasDstIdGranted(dstId)) {
        uint32_t chNo = m_grantChTable.at(dstId);

        if (m_verbose) {
            LogMessage(LOG_RF, "P25, releasing channel grant, chNo = %u, dstId = %u",
                chNo, dstId);
        }

        m_grantChTable[dstId] = 0U;
        m_voiceChTable.push_back(chNo);

        if (m_voiceGrantChCnt > 0U) {
            m_voiceGrantChCnt--;
            setSiteChCnt(m_voiceChCnt + m_voiceGrantChCnt);
        }
        else {
            m_voiceGrantChCnt = 0U;
            setSiteChCnt(m_voiceChCnt);
        }

        m_grantTimers[dstId].stop();
    }
}

/// <summary>
/// Helper to release group affiliations.
/// </summary>
/// <param name="dstId"></param>
/// <param name="releaseAll"></param>
void TrunkPacket::clearGrpAff(uint32_t dstId, bool releaseAll)
{
    if (dstId == 0U && !releaseAll) {
        return;
    }

    std::vector<uint32_t> srcToRel = std::vector<uint32_t>();
    if (dstId == 0U && releaseAll) {
        LogWarning(LOG_RF, "P25, releasing all group affiliations");
        for (auto it = m_grpAffTable.begin(); it != m_grpAffTable.end(); ++it) {
            uint32_t srcId = it->first;
            srcToRel.push_back(srcId);
        }
    }
    else {
        LogWarning(LOG_RF, "P25, releasing group affiliations, dstId = %u", dstId);
        for (auto it = m_grpAffTable.begin(); it != m_grpAffTable.end(); ++it) {
            uint32_t srcId = it->first;
            uint32_t grpId = it->second;
            if (grpId == dstId) {
                srcToRel.push_back(srcId);
            }
        }
    }

    // release affiliations
    for (auto it = srcToRel.begin(); it != srcToRel.end(); ++it) {
        writeRF_TSDU_U_Dereg_Ack(*it);
    }
}

/// <summary>
///
/// </summary>
void TrunkPacket::resetStatusCommand()
{
    // reset status control data
    if (m_statusCmdEnable) {
        if (m_statusSrcId != 0U && m_statusValue != 0U) {
            if (m_verbose) {
                LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_STS_UPDT (Status Update), canceled command mode, statusCurrentStatus = $%02X", m_statusValue);
            }
        }

        m_statusSrcId = 0U;
        m_statusValue = 0U;
    }
}

/// <summary>
/// Updates the processor by the passed number of milliseconds.
/// </summary>
/// <param name="ms"></param>
void TrunkPacket::clock(uint32_t ms)
{
    if (m_p25->m_control) {
        // clock all the grant timers
        std::vector<uint32_t> gntsToRel = std::vector<uint32_t>();
        for (auto it = m_grantChTable.begin(); it != m_grantChTable.end(); ++it) {
            uint32_t dstId = it->first;

            m_grantTimers[dstId].clock(ms);
            if (m_grantTimers[dstId].isRunning() && m_grantTimers[dstId].hasExpired()) {
                gntsToRel.push_back(dstId);
            }
        }

        // release grants that have timed out
        for (auto it = gntsToRel.begin(); it != gntsToRel.end(); ++it) {
            releaseDstIdGrant(*it, false);
        }

        // clock adjacent site update timers
        m_adjSiteUpdateTimer.clock(ms);
        if (m_adjSiteUpdateTimer.isRunning() && m_adjSiteUpdateTimer.hasExpired()) {
            for (auto it = m_adjSiteUpdateCnt.begin(); it != m_adjSiteUpdateCnt.end(); ++it) {
                uint8_t siteId = it->first;
                
                uint8_t updateCnt = it->second;
                if (updateCnt > 0U) {
                    updateCnt--;
                }
                
                if (updateCnt == 0U) {
                    SiteData siteData = m_adjSiteTable[siteId];
                    LogWarning(LOG_NET, P25_TSDU_STR ", TSBK_OSP_ADJ_STS_BCAST (Adjacent Site Status Broadcast), no data [FAILED], sysId = $%03X, rfss = $%02X, site = $%02X, chId = %u, chNo = %u",
                        siteData.sysId(), siteData.rfssId(), siteData.siteId(), siteData.channelId(), siteData.channelNo());
                }

                m_adjSiteUpdateCnt[siteId] = updateCnt;
            }

            m_adjSiteUpdateTimer.setTimeout(m_adjSiteUpdateInterval);
            m_adjSiteUpdateTimer.start();
        }
    }
}

/// <summary>
/// Helper to write a call alert packet.
/// </summary>
/// <param name="srcId"></param>
/// <param name="dstId"></param>
void TrunkPacket::writeRF_TSDU_Call_Alrt(uint32_t srcId, uint32_t dstId)
{
    if (m_verbose) {
        LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_CALL_ALRT (Call Alert), srcId = %u, dstId = %u", srcId, dstId);
    }

    ::ActivityLog("P25", true, "received call alert request from %u to %u", srcId, dstId);

    m_rfTSBK.setLCO(TSBK_IOSP_CALL_ALRT);
    m_rfTSBK.setSrcId(srcId);
    m_rfTSBK.setDstId(dstId);
    writeRF_TSDU_SBF(false);
}

/// <summary>
/// Helper to write a extended function packet.
/// </summary>
/// <param name="func"></param>
/// <param name="arg"></param>
/// <param name="dstId"></param>
void TrunkPacket::writeRF_TSDU_Ext_Func(uint32_t func, uint32_t arg, uint32_t dstId)
{
    uint8_t lco = m_rfTSBK.getLCO();
    uint8_t mfId = m_rfTSBK.getMFId();

    m_rfTSBK.setMFId(P25_MFG_STANDARD);

    m_rfTSBK.setLCO(TSBK_IOSP_EXT_FNCT);
    m_rfTSBK.setExtendedFunction(func);
    m_rfTSBK.setSrcId(arg);
    m_rfTSBK.setDstId(dstId);

    if (m_verbose) {
        LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_EXT_FNCT (Extended Function), op = $%02X, arg = %u, tgt = %u",
            m_rfTSBK.getExtendedFunction(), m_rfTSBK.getSrcId(), m_rfTSBK.getDstId());
    }

    // generate activity log entry
    if (func == P25_EXT_FNCT_CHECK) {
        ::ActivityLog("P25", true, "received radio check request from %u to %u", arg, dstId);
    }
    else if (func == P25_EXT_FNCT_INHIBIT) {
        ::ActivityLog("P25", true, "received radio inhibit request from %u to %u", arg, dstId);
    }
    else if (func == P25_EXT_FNCT_UNINHIBIT) {
        ::ActivityLog("P25", true, "received radio uninhibit request from %u to %u", arg, dstId);
    }

    writeRF_TSDU_SBF(false);

    m_rfTSBK.setLCO(lco);
    m_rfTSBK.setMFId(mfId);
}

/// <summary>
/// Helper to write a group affiliation query packet.
/// </summary>
/// <param name="dstId"></param>
void TrunkPacket::writeRF_TSDU_Grp_Aff_Q(uint32_t dstId)
{
    if (m_verbose) {
        LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_GRP_AFF_Q (Group Affiliation Query), dstId = %u", dstId);
    }

    ::ActivityLog("P25", true, "received group affiliation query command from %u to %u", P25_WUID_SYS, dstId);

    m_rfTSBK.setLCO(TSBK_OSP_GRP_AFF_Q);
    m_rfTSBK.setSrcId(P25_WUID_SYS);
    m_rfTSBK.setDstId(dstId);
    writeRF_TSDU_SBF(true);
}

/// <summary>
/// Helper to write a unit registration command packet.
/// </summary>
/// <param name="dstId"></param>
void TrunkPacket::writeRF_TSDU_U_Reg_Cmd(uint32_t dstId)
{
    if (m_verbose) {
        LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_U_REG_CMD (Unit Registration Command), dstId = %u", dstId);
    }

    ::ActivityLog("P25", true, "received unit registration command from %u to %u", P25_WUID_SYS, dstId);

    m_rfTSBK.setLCO(TSBK_OSP_U_REG_CMD);
    m_rfTSBK.setSrcId(P25_WUID_SYS);
    m_rfTSBK.setDstId(dstId);
    writeRF_TSDU_SBF(true);
}

/// <summary>
/// Helper to write a Motorola patch packet.
/// </summary>
/// <param name="group1"></param>
/// <param name="group2"></param>
/// <param name="group3"></param>
void TrunkPacket::writeRF_TSDU_Mot_Patch(uint32_t group1, uint32_t group2, uint32_t group3)
{
    uint8_t lco = m_rfTSBK.getLCO();

    if (m_verbose) {
        LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_MOT_GRG_ADD (Group Regroup Add - Patch Supergroup), superGrp = %u, group1 = %u, group2 = %u, group3 = %u",
            m_patchSuperGroup, group1, group2, group3);
    }

    m_rfTSBK.setLCO(TSBK_OSP_MOT_GRG_ADD);
    m_rfTSBK.setMFId(P25_MFG_MOT);
    m_rfTSBK.setPatchSuperGroupId(m_patchSuperGroup);
    m_rfTSBK.setPatchGroup1Id(group1);
    m_rfTSBK.setPatchGroup2Id(group2);
    m_rfTSBK.setPatchGroup3Id(group3);
    writeRF_TSDU_SBF(true);

    m_rfTSBK.setLCO(lco);
    m_rfTSBK.setMFId(P25_MFG_STANDARD);
}

// ---------------------------------------------------------------------------
//  Private Class Members
// ---------------------------------------------------------------------------
/// <summary>
/// Initializes a new instance of the TrunkPacket class.
/// </summary>
/// <param name="p25">Instance of the Control class.</param>
/// <param name="network">Instance of the BaseNetwork class.</param>
/// <param name="debug">Flag indicating whether P25 debug is enabled.</param>
/// <param name="verbose">Flag indicating whether P25 verbose logging is enabled.</param>
TrunkPacket::TrunkPacket(Control* p25, network::BaseNetwork* network, bool debug, bool verbose) :
    m_p25(p25),
    m_network(network),
    m_patchSuperGroup(0xFFFFU),
    m_verifyAff(false),
    m_verifyReg(false),
    m_rfTSBK(),
    m_netTSBK(),
    m_rfMBF(NULL),
    m_mbfCnt(0U),
    m_mbfIdenCnt(0U),
    m_mbfAdjSSCnt(0U),
    m_rfTDULC(),
    m_netTDULC(),
    m_voiceChTable(),
    m_adjSiteTable(),
    m_adjSiteUpdateCnt(),
    m_unitRegTable(),
    m_grpAffTable(),
    m_grantChTable(),
    m_grantTimers(),
    m_voiceChCnt(1U),
    m_voiceGrantChCnt(0U),
    m_noStatusAck(false),
    m_noMessageAck(true),
    m_statusCmdEnable(false),
    m_statusRadioCheck(0U),
    m_statusRadioInhibit(0U),
    m_statusRadioUninhibit(0U),
    m_statusRadioForceReg(0U),
    m_statusRadioForceDereg(0U),
    m_statusSrcId(0U),
    m_statusValue(0U),
    m_siteData(),
    m_adjSiteUpdateTimer(1000U),
    m_adjSiteUpdateInterval(ADJ_SITE_TIMER_TIMEOUT),
    m_skipSBFPreamble(false),
    m_verbose(verbose),
    m_debug(debug)
{
    m_rfMBF = new uint8_t[P25_MAX_PDU_COUNT * P25_LDU_FRAME_LENGTH_BYTES + 2U];
    ::memset(m_rfMBF, 0x00U, P25_MAX_PDU_COUNT * P25_LDU_FRAME_LENGTH_BYTES + 2U);

    // set metadata defaults
    m_rfTSBK.setSiteData(m_siteData);
    m_netTSBK.setSiteData(m_siteData);
    m_rfTSBK.setCallsign("CHANGEME");
    m_netTSBK.setCallsign("CHANGEME");

    m_rfTDULC.setSiteData(m_siteData);
    m_netTDULC.setSiteData(m_siteData);

    m_voiceChTable.clear();
 
    m_adjSiteTable.clear();
    m_adjSiteUpdateCnt.clear();
 
    m_unitRegTable.clear();
    m_grpAffTable.clear();

    m_grantChTable.clear();
    m_grantTimers.clear();

    m_adjSiteUpdateInterval = ADJ_SITE_TIMER_TIMEOUT + m_p25->m_ccBcstInterval;
    m_adjSiteUpdateTimer.setTimeout(m_adjSiteUpdateInterval);
    m_adjSiteUpdateTimer.start();
}

/// <summary>
/// Finalizes a instance of the TrunkPacket class.
/// </summary>
TrunkPacket::~TrunkPacket()
{
    delete[] m_rfMBF;
}

/// <summary>
/// Write data processed from RF to the network.
/// </summary>
/// <param name="data"></param>
/// <param name="autoReset"></param>
void TrunkPacket::writeNetworkRF(const uint8_t* data, bool autoReset)
{
    assert(data != NULL);

    if (m_network == NULL)
        return;

    if (m_p25->m_rfTimeout.isRunning() && m_p25->m_rfTimeout.hasExpired())
        return;

    m_network->writeP25TSDU(m_rfTSBK, data);
    if (autoReset)
        m_network->resetP25();
}

/// <summary>
/// Helper to write control channel packet data.
/// </summary>
/// <param name="frameCnt"></param>
/// <param name="n"></param>
/// <param name="adjSS"></param>
void TrunkPacket::writeRF_ControlData(uint8_t frameCnt, uint8_t n, bool adjSS)
{
    uint8_t i = 0U, seqCnt = 0U;

    if (!m_p25->m_control) {
        return;
    }

    // loop to generate 6 control sequences
    if (frameCnt == 255U) {
        seqCnt = 6U;
    }

    do
    {
        m_rfTSBK.reset();

        if (m_debug) {
            LogDebug(LOG_P25, "writeRF_ControlData, mbfCnt = %u, frameCnt = %u, seq = %u, adjSS = %u", m_mbfCnt, frameCnt, n, adjSS);
        }

        switch (n)
        {
        case 0:
            queueRF_TSBK_Ctrl_MBF(TSBK_OSP_IDEN_UP);
            break;
        case 1:
            queueRF_TSBK_Ctrl_MBF(TSBK_OSP_RFSS_STS_BCAST);
            break;
        case 2:
            queueRF_TSBK_Ctrl_MBF(TSBK_OSP_NET_STS_BCAST);
            break;
        case 3:
            queueRF_TSBK_Ctrl_MBF(TSBK_OSP_SNDCP_CH_ANN);
            break;
        case 4:
            // write ADJSS
            if (adjSS) {
                queueRF_TSBK_Ctrl_MBF(TSBK_OSP_ADJ_STS_BCAST);
            }
            break;
        }
        
        if (seqCnt > 0U)
            n++;
        i++;
    } while (i <= seqCnt);

    // should we insert the BSI bursts?
    bool bsi = (frameCnt % 64U) == 0U;
    if (bsi || frameCnt == 255U) {
        queueRF_TSBK_Ctrl_MBF(TSBK_OSP_MOT_CC_BSI);
    }

    // add padding after the 4th sequence
    if (seqCnt > 4U) {
        // pad MBF if we have 1 queued TSDUs
        if (m_mbfCnt == 1U) {
            queueRF_TSBK_Ctrl_MBF(TSBK_OSP_RFSS_STS_BCAST);
            queueRF_TSBK_Ctrl_MBF(TSBK_OSP_NET_STS_BCAST);
            if (m_debug) {
                LogDebug(LOG_P25, "writeRF_ControlData, have 1 pad 2, mbfCnt = %u", m_mbfCnt);
            }
        }

        // pad MBF if we have 2 queued TSDUs
        if (m_mbfCnt == 2U) {
            std::vector<lookups::IdenTable> entries = m_p25->m_idenTable->list();
            if (entries.size() > 1U) {
                queueRF_TSBK_Ctrl_MBF(TSBK_OSP_IDEN_UP);
            }
            else {
                queueRF_TSBK_Ctrl_MBF(TSBK_OSP_RFSS_STS_BCAST);
            }

            if (m_debug) {
                LogDebug(LOG_P25, "writeRF_ControlData, have 2 pad 1, mbfCnt = %u", m_mbfCnt);
            }
        }

        // reset MBF count
        m_mbfCnt = 0U;
    }
}

/// <summary>
/// Helper to write a P25 TDU w/ link control packet.
/// </summary>
/// <param name="duid"></param>
/// <param name="noNetwork"></param>
void TrunkPacket::writeRF_TDULC(uint8_t duid, bool noNetwork)
{
    uint8_t data[P25_TDULC_FRAME_LENGTH_BYTES + 2U];
    ::memset(data + 2U, 0x00U, P25_TDULC_FRAME_LENGTH_BYTES);

    // Generate Sync
    Sync::addP25Sync(data + 2U);

    // Generate NID
    m_p25->m_nid.encode(data + 2U, P25_DUID_TDULC);

    // Generate TDULC Data
    m_rfTDULC.encode(data + 2U);

    // Add busy bits
    m_p25->addBusyBits(data + 2U, P25_TDULC_FRAME_LENGTH_BITS, true, true);

    m_p25->m_rfTimeout.stop();

    if (!noNetwork)
        writeNetworkRF(data + 2U, P25_DUID_TDULC);

    if (m_p25->m_duplex) {
        data[0U] = TAG_EOT;
        data[1U] = 0x00U;

        m_p25->writeQueueRF(data, P25_TDULC_FRAME_LENGTH_BYTES + 2U);
    }

    if (m_debug) {
        Utils::dump(2U, "!!! *TX P25 Frame - P25_DUID_TDULC", data + 2U, P25_TDULC_FRAME_LENGTH_BYTES);
    }
}

/// <summary>
/// Helper to write a P25 TDU w/ link control channel grant packet.
/// </summary>
/// <param name="grp"></param>
/// <param name="srcId"></param>
/// <param name="dstId"></param>
void TrunkPacket::writeRF_TDULC_ChanGrant(bool grp, uint32_t srcId, uint32_t dstId)
{
    m_p25->writeRF_TDU(true);
    m_p25->m_voice->m_lastDUID = P25_DUID_TDU;

    if ((srcId != 0U) && (dstId != 0U)) {
        for (uint32_t i = 0; i < 4; i++) {
            m_rfTDULC.setSrcId(srcId);
            m_rfTDULC.setDstId(dstId);
            m_rfTDULC.setEmergency(false);

            if (grp) {
                m_rfTDULC.setLCO(LC_GROUP);
                writeRF_TDULC(P25_DUID_TDULC, true);
            }
            else {
                m_rfTDULC.setLCO(LC_PRIVATE);
                writeRF_TDULC(P25_DUID_TDULC, true);
            }
        }
    }
}

/// <summary>
/// Helper to write a P25 TDU w/ link control channel release packet.
/// </summary>
/// <param name="grp"></param>
/// <param name="srcId"></param>
/// <param name="dstId"></param>
void TrunkPacket::writeRF_TDULC_ChanRelease(bool grp, uint32_t srcId, uint32_t dstId)
{
    uint32_t count = m_p25->m_hangCount / 2;

    for (uint32_t i = 0; i < count; i++) {
        if ((srcId != 0U) && (dstId != 0U)) {
            m_rfTDULC.setSrcId(srcId);
            m_rfTDULC.setDstId(dstId);
            m_rfTDULC.setEmergency(false);

            if (grp) {
                m_rfTDULC.setLCO(LC_GROUP);
                writeRF_TDULC(P25_DUID_TDULC, true);
            }
            else {
                m_rfTDULC.setLCO(LC_PRIVATE);
                writeRF_TDULC(P25_DUID_TDULC, true);
            }
        }

        m_rfTDULC.setLCO(LC_NET_STS_BCAST);
        writeRF_TDULC(P25_DUID_TDULC, true);
        m_rfTDULC.setLCO(LC_RFSS_STS_BCAST);
        writeRF_TDULC(P25_DUID_TDULC, true);
    }

    if (m_verbose) {
        LogMessage(LOG_RF, P25_TDULC_STR ", LC_CALL_TERM (Call Termination), srcId = %u, dstId = %u", m_rfTDULC.getSrcId(), m_rfTDULC.getDstId());
    }

    m_rfTDULC.setLCO(LC_CALL_TERM);
    writeRF_TDULC(P25_DUID_TDULC, true);

    m_rfTDULC.reset();
}

/// <summary>
/// Helper to write a single-block P25 TSDU packet.
/// </summary>
/// <param name="noNetwork"></param>
/// <param name="clearBeforeWrite"></param>
void TrunkPacket::writeRF_TSDU_SBF(bool noNetwork, bool clearBeforeWrite)
{
    uint8_t data[P25_TSDU_FRAME_LENGTH_BYTES + 2U];
    ::memset(data + 2U, 0x00U, P25_TSDU_FRAME_LENGTH_BYTES);

    // Generate Sync
    Sync::addP25Sync(data + 2U);

    // Generate NID
    m_p25->m_nid.encode(data + 2U, P25_DUID_TSDU);

    // Generate TSBK block
    m_rfTSBK.setLastBlock(true); // always set last block -- this a Single Block TSDU
    m_rfTSBK.encode(data + 2U, true);

    if (m_debug) {
        Utils::dump(2U, "!!! *TSDU (SBF) TSBK Block Data", data + P25_PREAMBLE_LENGTH_BYTES + 2U, P25_TSBK_FEC_LENGTH_BYTES);
    }

    // Add busy bits
    m_p25->addBusyBits(data + 2U, P25_TSDU_FRAME_LENGTH_BITS, true, false);

    // Set first busy bits to 1,1
    m_p25->setBusyBits(data + 2U, P25_SS0_START, true, true);

    if (!noNetwork)
        writeNetworkRF(data + 2U, true);

    if (m_p25->m_continuousControl) {
        writeRF_TSDU_MBF(clearBeforeWrite);
        return;
    }

    if (m_p25->m_ccRunning) {
        writeRF_TSDU_MBF(clearBeforeWrite);
        return;
    }

    if (clearBeforeWrite) {
        m_p25->m_modem->clearP25Data();
        m_p25->m_queue.clear();
    }

    if (!m_skipSBFPreamble) {
        m_p25->writeRF_Preamble();
    }

    m_skipSBFPreamble = false;

    if (m_p25->m_duplex) {
        data[0U] = TAG_DATA;
        data[1U] = 0x00U;

        m_p25->writeQueueRF(data, P25_TSDU_FRAME_LENGTH_BYTES + 2U);
    }

    if (m_debug) {
        Utils::dump(2U, "!!! *TX P25 Frame - (SBF) P25_DUID_TSDU", data + 2U, P25_TSDU_FRAME_LENGTH_BYTES);
    }
}

/// <summary>
/// Helper to write a multi-block P25 TSDU packet.
/// </summary>
/// <param name="clearBeforeWrite"></param>
void TrunkPacket::writeRF_TSDU_MBF(bool clearBeforeWrite)
{
    uint8_t tsbk[P25_TSBK_FEC_LENGTH_BYTES];
    ::memset(tsbk, 0x00U, P25_TSBK_FEC_LENGTH_BYTES);

    // LogDebug(LOG_P25, "writeRF_TSDU_MBF, mbfCnt = %u", m_mbfCnt);

    // can't transmit MBF with duplex disabled
    if (!m_p25->m_duplex) {
        ::memset(m_rfMBF, 0x00U, P25_MAX_PDU_COUNT * P25_LDU_FRAME_LENGTH_BYTES + 2U);
        m_mbfCnt = 0U;
        return;
    }

    if (m_mbfCnt == 0U) {
        ::memset(m_rfMBF, 0x00U, P25_TSBK_FEC_LENGTH_BYTES * TSBK_MBF_CNT);
    }

    // trigger encoding of last block and write to queue
    if (m_mbfCnt + 1U == TSBK_MBF_CNT) {
        // Generate TSBK block
        m_rfTSBK.setLastBlock(true); // set last block
        m_rfTSBK.encode(tsbk, false);

        if (m_debug) {
            Utils::dump(2U, "!!! *TSDU MBF Last TSBK Block", tsbk, P25_TSBK_FEC_LENGTH_BYTES);
        }

        Utils::setBitRange(tsbk, m_rfMBF, (m_mbfCnt * P25_TSBK_FEC_LENGTH_BITS), P25_TSBK_FEC_LENGTH_BITS);

        // Generate TSDU frame
        uint8_t tsdu[P25_TSDU_TRIPLE_FRAME_LENGTH_BYTES];
        ::memset(tsdu, 0x00U, P25_TSDU_TRIPLE_FRAME_LENGTH_BYTES);

        uint32_t offset = 0U;
        for (uint8_t i = 0U; i < m_mbfCnt + 1U; i++) {
            ::memset(tsbk, 0x00U, P25_TSBK_FEC_LENGTH_BYTES);
            Utils::getBitRange(m_rfMBF, tsbk, offset, P25_TSBK_FEC_LENGTH_BITS);

            if (m_debug) {
                Utils::dump(2U, "!!! *TSDU (MBF) TSBK Block", tsbk, P25_TSBK_FEC_LENGTH_BYTES);
            }
                
            // Add TSBK data
            Utils::setBitRange(tsbk, tsdu, offset, P25_TSBK_FEC_LENGTH_BITS);

            offset += P25_TSBK_FEC_LENGTH_BITS;
        }

        // Utils::dump(2U, "!!! *TSDU DEBUG - tsdu", tsdu, P25_TSDU_TRIPLE_FRAME_LENGTH_BYTES);

        uint8_t data[P25_TSDU_TRIPLE_FRAME_LENGTH_BYTES + 2U];
        ::memset(data + 2U, 0x00U, P25_TSDU_TRIPLE_FRAME_LENGTH_BYTES);

        // Generate Sync
        Sync::addP25Sync(data + 2U);

        // Generate NID
        m_p25->m_nid.encode(data + 2U, P25_DUID_TSDU);

        // interleave
        P25Utils::encode(tsdu, data + 2U, 114U, 720U);

        // Add busy bits
        m_p25->addBusyBits(data + 2U, P25_TSDU_TRIPLE_FRAME_LENGTH_BITS, true, false);

        // Add idle bits
        addIdleBits(data + 2U, P25_TSDU_TRIPLE_FRAME_LENGTH_BITS, true, true);

        data[0U] = TAG_DATA;
        data[1U] = 0x00U;
        
        if (clearBeforeWrite) {
            m_p25->m_modem->clearP25Data();
            m_p25->m_queue.clear();
        }

        m_p25->writeQueueRF(data, P25_TSDU_TRIPLE_FRAME_LENGTH_BYTES + 2U);

        if (m_debug) {
            Utils::dump(2U, "!!! *TX P25 Frame - (MBF) P25_DUID_TSDU", data + 2U, P25_TSDU_TRIPLE_FRAME_LENGTH_BYTES);
        }

        ::memset(m_rfMBF, 0x00U, P25_MAX_PDU_COUNT * P25_LDU_FRAME_LENGTH_BYTES + 2U);
        m_mbfCnt = 0U;
        return;
    }

    // Generate TSBK block
    m_rfTSBK.setLastBlock(false); // clear last block
    m_rfTSBK.encode(tsbk, false);

    if (m_debug) {
        Utils::dump(2U, "!!! *TSDU MBF Block Data", tsbk, P25_TSBK_FEC_LENGTH_BYTES);
    }

    Utils::setBitRange(tsbk, m_rfMBF, (m_mbfCnt * P25_TSBK_FEC_LENGTH_BITS), P25_TSBK_FEC_LENGTH_BITS);
    m_mbfCnt++;
}

/// <summary>
/// Helper to queue the given control TSBK into the MBF queue.
/// </summary>
/// <param name="lco"></param>
void TrunkPacket::queueRF_TSBK_Ctrl_MBF(uint8_t lco)
{
    m_rfTSBK.reset();

    switch (lco) {
        case TSBK_OSP_IDEN_UP:
            {
                if (m_debug) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_IDEN_UP (Identity Update)");
                }

                std::vector<lookups::IdenTable> entries = m_p25->m_idenTable->list();
                if (m_mbfIdenCnt >= entries.size())
                    m_mbfIdenCnt = 0U;

                uint8_t i = 0U;
                for (auto it = entries.begin(); it != entries.end(); ++it) {
                    // no good very bad way of skipping entries...
                    if (i != m_mbfIdenCnt) {
                        i++;
                        continue;
                    }
                    else {
                        lookups::IdenTable entry = *it;

                        // LogDebug(LOG_P25, "baseFrequency = %uHz, txOffsetMhz = %fMHz, chBandwidthKhz = %fKHz, chSpaceKhz = %fKHz",
                        //    entry.baseFrequency(), entry.txOffsetMhz(), entry.chBandwidthKhz(), entry.chSpaceKhz());

                        // handle 700/800/900 identities
                        if (entry.baseFrequency() >= 762000000U) {
                            m_rfTSBK.setIdenTable(entry);

                            // transmit channel ident broadcast
                            m_rfTSBK.setLCO(TSBK_OSP_IDEN_UP);
                        }
                        else {
                            // handle as a VHF/UHF identity
                            m_rfTSBK.setIdenTable(entry);

                            // transmit channel ident broadcast
                            m_rfTSBK.setLCO(TSBK_OSP_IDEN_UP_VU);
                        }

                        m_mbfIdenCnt++;
                        break;
                    }
                }
            }
            break;
        case TSBK_OSP_NET_STS_BCAST:
            if (m_debug) {
                LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_NET_STS_BCAST (Network Status Broadcast)");
            }

            // transmit net status burst
            m_rfTSBK.setLCO(TSBK_OSP_NET_STS_BCAST);
            break;
        case TSBK_OSP_RFSS_STS_BCAST:
            if (m_debug) {
                LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_RFSS_STS_BCAST (RFSS Status Broadcast)");
            }

            // transmit rfss status burst
            m_rfTSBK.setLCO(TSBK_OSP_RFSS_STS_BCAST);
            break;
        case TSBK_OSP_ADJ_STS_BCAST:
            // write ADJSS
            if (m_adjSiteTable.size() > 0) {
                if (m_mbfAdjSSCnt >= m_adjSiteTable.size())
                    m_mbfAdjSSCnt = 0U;

                if (m_debug) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_ADJ_STS_BCAST (Adjacent Site Broadcast)");
                }

                uint8_t i = 0U;
                for (auto it = m_adjSiteTable.begin(); it != m_adjSiteTable.end(); ++it) {
                    // no good very bad way of skipping entries...
                    if (i != m_mbfAdjSSCnt) {
                        i++;
                        continue;
                    }
                    else {
                        SiteData site = it->second;

                        uint8_t cfva = P25_CFVA_CONV | P25_CFVA_NETWORK;
                        if (m_adjSiteUpdateCnt[site.siteId()] == 0U) {
                            cfva |= P25_CFVA_FAILURE;
                        }
                        else {
                            cfva |= P25_CFVA_VALID;
                        }

                        // transmit adjacent site broadcast
                        m_rfTSBK.setLCO(TSBK_OSP_ADJ_STS_BCAST);
                        m_rfTSBK.setAdjSiteCFVA(cfva);
                        m_rfTSBK.setAdjSiteSysId(site.sysId());
                        m_rfTSBK.setAdjSiteRFSSId(site.rfssId());
                        m_rfTSBK.setAdjSiteId(site.siteId());
                        m_rfTSBK.setAdjSiteChnId(site.channelId());
                        m_rfTSBK.setAdjSiteChnNo(site.channelNo());

                        m_mbfAdjSSCnt++;
                        break;
                    }
                }
            }
            else {
                return; // don't create anything
            }
            break;
        case TSBK_OSP_SNDCP_CH_ANN:
            if (m_debug) {
                LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_SNDCP_CH_ANN (SNDCP Channel Announcement)");
            }

            // transmit SNDCP announcement
            m_rfTSBK.setLCO(TSBK_OSP_SNDCP_CH_ANN);
            break;

        /** Motorola CC data */
        case TSBK_OSP_MOT_PSH_CCH:
            if (m_debug) {
                LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_MOT_PSH_CCH (Motorola Planned Shutdown)");
            }

            // transmit motorola PSH CCH burst
            m_rfTSBK.setLCO(TSBK_OSP_MOT_PSH_CCH);
            m_rfTSBK.setMFId(P25_MFG_MOT);
            break;

        case TSBK_OSP_MOT_CC_BSI:
            if (m_debug) {
                LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_MOT_CC_BSI (Motorola Control Channel BSI)");
            }

            // transmit motorola CC BSI burst
            m_rfTSBK.setLCO(TSBK_OSP_MOT_CC_BSI);
            m_rfTSBK.setMFId(P25_MFG_MOT);
            break;
    }

    m_rfTSBK.setLastBlock(true); // always set last block
    writeRF_TSDU_MBF();
}

/// <summary>
/// Helper to write a grant packet.
/// </summary>
/// <param name="grp"></param>
/// <param name="skip"></param>
/// <returns></returns>
bool TrunkPacket::writeRF_TSDU_Grant(bool grp, bool skip)
{
    uint8_t lco = m_rfTSBK.getLCO();

    if (m_rfTSBK.getDstId() == P25_TGID_ALL) {
        return true; // do not generate grant packets for $FFFF (All Call) TGID
    }

    // are we skipping checking?
    if (!skip) {
        if (m_p25->m_rfState != RS_RF_LISTENING && m_p25->m_rfState != RS_RF_DATA) {
            LogWarning(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_GRP_VCH (Group Voice Channel Request) denied, traffic in progress, dstId = %u", m_rfTSBK.getDstId());
            writeRF_TSDU_Deny(P25_DENY_RSN_PTT_COLLIDE, (grp) ? TSBK_IOSP_GRP_VCH : TSBK_IOSP_UU_VCH);
            m_p25->checkAndReject();
            m_rfTSBK.setLCO(lco);
            return false;
        }

        if (m_p25->m_netState != RS_NET_IDLE && m_rfTSBK.getDstId() == m_p25->m_netLastDstId) {
            LogWarning(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_GRP_VCH (Group Voice Channel Request) denied, traffic in progress, dstId = %u", m_rfTSBK.getDstId());
            writeRF_TSDU_Deny(P25_DENY_RSN_PTT_COLLIDE, (grp) ? TSBK_IOSP_GRP_VCH : TSBK_IOSP_UU_VCH);
            m_p25->checkAndReject();
            m_rfTSBK.setLCO(lco);
            return false;
        }

        // don't transmit grants if the destination ID's don't match and the network TG hang timer is running
        if (m_p25->m_rfLastDstId != 0U) {
            if (m_p25->m_rfLastDstId != m_rfTSBK.getDstId() && (m_p25->m_networkTGHang.isRunning() && !m_p25->m_networkTGHang.hasExpired())) {
                m_rfTSBK.setLCO(lco);
                return false;
            }
        }

        if (!hasDstIdGranted(m_rfTSBK.getDstId())) {
            if (m_voiceChTable.empty()) {
                if (grp) {
                    LogWarning(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_GRP_VCH (Group Voice Channel Request) queued, no channels available, dstId = %u", m_rfTSBK.getDstId());
                    writeRF_TSDU_Queue(P25_QUE_RSN_CHN_RESOURCE_NOT_AVAIL, TSBK_IOSP_GRP_VCH);
                    m_p25->checkAndReject();
                    m_rfTSBK.setLCO(lco);
                    return false;
                }
                else {
                    LogWarning(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_UU_VCH (Unit-to-Unit Voice Channel Request) queued, no channels available, dstId = %u", m_rfTSBK.getDstId());
                    writeRF_TSDU_Queue(P25_QUE_RSN_CHN_RESOURCE_NOT_AVAIL, TSBK_IOSP_UU_VCH);
                    m_p25->checkAndReject();
                    m_rfTSBK.setLCO(lco);
                    return false;
                }
            }
            else {
                uint32_t chNo = m_voiceChTable.at(0);
                auto it = std::find(m_voiceChTable.begin(), m_voiceChTable.end(), chNo);
                m_voiceChTable.erase(it);

                m_grantChTable[m_rfTSBK.getDstId()] = chNo;
                m_rfTSBK.setGrpVchNo(chNo);

                m_grantTimers[m_rfTSBK.getDstId()] = Timer(1000U, GRANT_TIMER_TIMEOUT);
                m_grantTimers[m_rfTSBK.getDstId()].start();

                m_voiceGrantChCnt++;
                setSiteChCnt(m_voiceChCnt + m_voiceGrantChCnt);
            }
        }
        else {
            uint32_t chNo = m_grantChTable[m_rfTSBK.getDstId()];
            m_rfTSBK.setGrpVchNo(chNo);

            m_grantTimers[m_rfTSBK.getDstId()].start();
        }
    }

    if (grp) {
        if (m_verbose) {
            LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_GRP_VCH (Group Voice Channel Grant), emerg = %u, encrypt = %u, prio = %u, chNo = %u, srcId = %u, dstId = %u",
                m_rfTSBK.getEmergency(), m_rfTSBK.getEncrypted(), m_rfTSBK.getPriority(), m_rfTSBK.getGrpVchNo(), m_rfTSBK.getSrcId(), m_rfTSBK.getDstId());
        }

        // transmit group grant
        m_rfTSBK.setLCO(TSBK_IOSP_GRP_VCH);
        writeRF_TSDU_SBF(true, true);
    }
    else {
        if (m_verbose) {
            LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_UU_VCH (Unit-to-Unit Voice Channel Grant), emerg = %u, encrypt = %u, prio = %u, chNo = %u, srcId = %u, dstId = %u",
                m_rfTSBK.getEmergency(), m_rfTSBK.getEncrypted(), m_rfTSBK.getPriority(), m_rfTSBK.getGrpVchNo(), m_rfTSBK.getSrcId(), m_rfTSBK.getDstId());
        }

        // transmit private grant
        m_rfTSBK.setLCO(TSBK_IOSP_UU_VCH);
        writeRF_TSDU_SBF(true, true);
    }

    m_rfTSBK.setLCO(lco);
    return true;
}

/// <summary>
/// Helper to write a unit to unit answer request packet.
/// </summary>
/// <param name="srcId"></param>
/// <param name="dstId"></param>
void TrunkPacket::writeRF_TSDU_UU_Ans_Req(uint32_t srcId, uint32_t dstId)
{
    uint8_t lco = m_rfTSBK.getLCO();

    if (m_verbose) {
        LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_UU_ANS (Unit-to-Unit Answer Request), srcId = %u, dstId = %u", srcId, dstId);
    }

    m_rfTSBK.setLCO(TSBK_IOSP_UU_ANS);
    m_rfTSBK.setSrcId(srcId);
    m_rfTSBK.setDstId(dstId);
    m_rfTSBK.setVendorSkip(true);
    writeRF_TSDU_SBF(false);

    m_rfTSBK.setLCO(lco);
    m_rfTSBK.setVendorSkip(false);
}

/// <summary>
/// Helper to write a acknowledge packet.
/// </summary>
/// <param name="srcId"></param>
/// <param name="service"></param>
/// <param name="noNetwork"></param>
void TrunkPacket::writeRF_TSDU_ACK_FNE(uint32_t srcId, uint32_t service, bool noNetwork)
{
    uint8_t lco = m_rfTSBK.getLCO();
    uint8_t mfId = m_rfTSBK.getMFId();

    m_rfTSBK.setLCO(TSBK_IOSP_ACK_RSP);
    m_rfTSBK.setMFId(P25_MFG_STANDARD);
    m_rfTSBK.setService(service);

    if (m_verbose) {
        LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_ACK_RSP (Acknowledge Response), AIV = %u, serviceType = $%02X, srcId = %u",
            m_rfTSBK.getAIV(), m_rfTSBK.getService(), srcId);
    }

    writeRF_TSDU_SBF(noNetwork);

    m_rfTSBK.setLCO(lco);
    m_rfTSBK.setMFId(mfId);
}

/// <summary>
/// Helper to write a deny packet.
/// </summary>
/// <param name="reason"></param>
/// <param name="service"></param>
void TrunkPacket::writeRF_TSDU_Deny(uint8_t reason, uint8_t service)
{
    uint8_t lco = m_rfTSBK.getLCO();

    if (m_verbose) {
        LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_DENY_RSP (Deny Response), AIV = %u, reason = $%02X, srcId = %u, dstId = %u", 
            m_rfTSBK.getAIV(), reason, m_rfTSBK.getSrcId(), m_rfTSBK.getDstId());
    }

    m_rfTSBK.setLCO(TSBK_OSP_DENY_RSP);
    m_rfTSBK.setService(service);
    m_rfTSBK.setResponse(reason);
    writeRF_TSDU_SBF(false);

    m_rfTSBK.setLCO(lco);
}

/// <summary>
/// Helper to write a group affiliation response packet.
/// </summary>
/// <param name="srcId"></param>
/// <param name="dstId"></param>
bool TrunkPacket::writeRF_TSDU_Grp_Aff_Rsp(uint32_t srcId, uint32_t dstId)
{
    bool ret = false;

    m_rfTSBK.setLCO(TSBK_IOSP_GRP_AFF);
    m_rfTSBK.setResponse(P25_RSP_ACCEPT);
    m_rfTSBK.setPatchSuperGroupId(m_patchSuperGroup);

    // validate the source RID
    if (!acl::AccessControl::validateSrcId(srcId)) {
        LogWarning(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_GRP_AFF (Group Affiliation Response) denial, RID rejection, srcId = %u", srcId);
        m_rfTSBK.setResponse(P25_RSP_DENY);
    }

    // validate the source RID is registered
    if (!hasSrcIdUnitReg(srcId) && m_verifyReg) {
        LogWarning(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_GRP_AFF (Group Affiliation Response) denial, RID not registered, srcId = %u", srcId);
        m_rfTSBK.setResponse(P25_RSP_DENY);
    }

    // validate the talkgroup ID
    if (m_rfTSBK.getGroup()) {
        if (!acl::AccessControl::validateTGId(dstId)) {
            LogWarning(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_GRP_AFF (Group Affiliation Response) denial, TGID rejection, dstId = %u", dstId);
            m_rfTSBK.setResponse(P25_RSP_REFUSED);
        }
    }

    if (m_rfTSBK.getResponse() == P25_RSP_ACCEPT) {
        if (m_verbose) {
            LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_GRP_AFF (Group Affiliation Response), anncId = %u, srcId = %u, dstId = %u",
                m_patchSuperGroup, srcId, dstId);
        }

        ::ActivityLog("P25", true, "received group affiliation request from %u to %s %u", srcId, "TG ", dstId);
        ret = true;

        // update dynamic affiliation table
        m_grpAffTable[srcId] = dstId;
    }

    writeRF_TSDU_SBF(false);
    return ret;
}

/// <summary>
/// Helper to write a unit registration response packet.
/// </summary>
/// <param name="srcId"></param>
void TrunkPacket::writeRF_TSDU_U_Reg_Rsp(uint32_t srcId)
{
    m_rfTSBK.setLCO(TSBK_IOSP_U_REG);
    m_rfTSBK.setResponse(P25_RSP_ACCEPT);

    // validate the system ID
    if (m_rfTSBK.getSysId() != m_siteData.sysId()) {
        LogWarning(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_U_REG (Unit Registration Response) denial, SYSID rejection, sysId = $%03X", m_rfTSBK.getSysId());
        m_rfTSBK.setResponse(P25_RSP_DENY);
    }

    // validate the source RID
    if (!acl::AccessControl::validateSrcId(srcId)) {
        LogWarning(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_U_REG (Unit Registration Response) denial, RID rejection, srcId = %u", srcId);
        m_rfTSBK.setResponse(P25_RSP_DENY);
    }

    if (m_rfTSBK.getResponse() == P25_RSP_ACCEPT) {
        if (m_verbose) {
            LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_U_REG (Unit Registration Response), srcId = %u, sysId = $%03X, netId = $%05X", srcId,
                m_rfTSBK.getSysId(), m_rfTSBK.getNetId());
        }

        ::ActivityLog("P25", true, "received unit registration request from %u", srcId);

        // update dynamic unit registration table
        if (!hasSrcIdUnitReg(srcId)) {
            m_unitRegTable.push_back(srcId);
        }
    }

    // because Motorola -- we'll set both Source and Destination to the Source ID
    // for the U_REG_RSP apparently should have the SUID set this way...
    m_rfTSBK.setSrcId(srcId/*P25_WUID_REG*/);
    m_rfTSBK.setDstId(srcId);

    writeRF_TSDU_SBF(true);

    // validate the source RID
    if (!acl::AccessControl::validateSrcId(srcId)) {
        denialInhibit(srcId); // inhibit source radio automatically
    }
}

/// <summary>
/// Helper to write a unit de-registration acknowledge packet.
/// </summary>
/// <param name="srcId"></param>
void TrunkPacket::writeRF_TSDU_U_Dereg_Ack(uint32_t srcId)
{
    m_rfTSBK.setLCO(TSBK_OSP_U_DEREG_ACK);

    if (m_verbose) {
        LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_ISP_U_DEREG_REQ (Unit Deregistration Request) srcId = %u, sysId = $%03X, netId = $%05X",
            srcId, m_rfTSBK.getSysId(), m_rfTSBK.getNetId());
    }

    ::ActivityLog("P25", true, "received unit deregistration request from %u", srcId);

    // remove dynamic unit registration table entry
    if (std::find(m_unitRegTable.begin(), m_unitRegTable.end(), srcId) != m_unitRegTable.end()) {
        auto it = std::find(m_unitRegTable.begin(), m_unitRegTable.end(), srcId);
        m_unitRegTable.erase(it);
    }

    // remove dynamic affiliation table entry
    try {
        m_grpAffTable.at(srcId);
        m_grpAffTable.erase(srcId);
    }
    catch (...) {
        // stub
    }

    m_rfTSBK.setSrcId(P25_WUID_SYS);
    m_rfTSBK.setDstId(srcId);

    writeRF_TSDU_SBF(false);
}

/// <summary>
/// Helper to write a queue packet.
/// </summary>
/// <param name="reason"></param>
/// <param name="service"></param>
void TrunkPacket::writeRF_TSDU_Queue(uint8_t reason, uint8_t service)
{
    uint8_t lco = m_rfTSBK.getLCO();

    if (m_verbose) {
        LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_OSP_QUE_RSP (Queue Response), AIV = %u, reason = $%02X, srcId = %u, dstId = %u", 
            m_rfTSBK.getAIV(), reason, m_rfTSBK.getSrcId(), m_rfTSBK.getDstId());
    }

    m_rfTSBK.setLCO(TSBK_OSP_QUE_RSP);
    m_rfTSBK.setService(service);
    m_rfTSBK.setResponse(reason);
    writeRF_TSDU_SBF(false);

    m_rfTSBK.setLCO(lco);
}

/// <summary>
/// Helper to write a network TSDU from the RF data queue.
/// </summary>
/// <param name="data"></param>
void TrunkPacket::writeNet_TSDU_From_RF(uint8_t* data)
{
    ::memset(data, 0x00U, P25_TSDU_FRAME_LENGTH_BYTES);

    // Generate Sync
    Sync::addP25Sync(data);

    // Generate NID
    m_p25->m_nid.encode(data, P25_DUID_TSDU);

    // Regenerate TSDU Data
    m_rfTSBK.setLastBlock(true); // always set last block -- this a Single Block TSDU
    m_rfTSBK.encode(data, true);

    // Add busy bits
    m_p25->addBusyBits(data, P25_TSDU_FRAME_LENGTH_BYTES, true, false);

    // Set first busy bits to 1,1
    m_p25->setBusyBits(data, P25_SS0_START, true, true);
}

/// <summary>
/// Helper to write a network P25 TDU w/ link control packet.
/// </summary>
void TrunkPacket::writeNet_TDULC()
{
    uint8_t buffer[P25_TDULC_FRAME_LENGTH_BYTES + 2U];
    ::memset(buffer, 0x00U, P25_TDULC_FRAME_LENGTH_BYTES + 2U);

    buffer[0U] = TAG_EOT;
    buffer[1U] = 0x00U;

    // Generate Sync
    Sync::addP25Sync(buffer + 2U);

    // Generate NID
    m_p25->m_nid.encode(buffer + 2U, P25_DUID_TDULC);

    // Regenerate TDULC Data
    m_netTDULC.encode(buffer + 2U);

    // Add busy bits
    m_p25->addBusyBits(buffer + 2U, P25_TDULC_FRAME_LENGTH_BITS, true, true);

    m_p25->writeQueueNet(buffer, P25_TDULC_FRAME_LENGTH_BYTES + 2U);

    if (m_verbose) {
        LogMessage(LOG_NET, P25_TDULC_STR ", lc = $%02X, srcId = %u", m_netTDULC.getLCO(), m_netTDULC.getSrcId());
    }

    if (m_debug) {
        Utils::dump(2U, "!!! *TX P25 Network Frame - P25_DUID_TDULC", buffer + 2U, P25_TDULC_FRAME_LENGTH_BYTES);
    }

    if (m_p25->m_voice->m_netFrames > 0) {
        ::ActivityLog("P25", false, "network end of transmission, %.1f seconds, %u%% packet loss", 
            float(m_p25->m_voice->m_netFrames) / 50.0F, (m_p25->m_voice->m_netLost * 100U) / m_p25->m_voice->m_netFrames);
    }
    else {
        ::ActivityLog("P25", false, "network end of transmission, %u frames", m_p25->m_voice->m_netFrames);
    }

    if (m_network != NULL)
        m_network->resetP25();

    m_p25->m_netTimeout.stop();
    m_p25->m_networkWatchdog.stop();
    m_netTDULC.reset();
    m_p25->m_netState = RS_NET_IDLE;
    m_p25->m_tailOnIdle = true;
}

/// <summary>
/// Helper to write a network single-block P25 TSDU packet.
/// </summary>
void TrunkPacket::writeNet_TSDU()
{
    uint8_t buffer[P25_TSDU_FRAME_LENGTH_BYTES + 2U];
    ::memset(buffer, 0x00U, P25_TSDU_FRAME_LENGTH_BYTES + 2U);

    buffer[0U] = TAG_DATA;
    buffer[1U] = 0x00U;

    // Generate Sync
    Sync::addP25Sync(buffer + 2U);

    // Generate NID
    m_p25->m_nid.encode(buffer + 2U, P25_DUID_TSDU);

    // Regenerate TSDU Data
    m_netTSBK.setLastBlock(true); // always set last block -- this a Single Block TSDU
    m_netTSBK.encode(buffer + 2U, true);

    // Add busy bits
    m_p25->addBusyBits(buffer + 2U, P25_TSDU_FRAME_LENGTH_BYTES, true, false);

    // Set first busy bits to 1,1
    m_p25->setBusyBits(buffer + 2U, P25_SS0_START, true, true);

    m_p25->writeQueueNet(buffer, P25_TSDU_FRAME_LENGTH_BYTES + 2U);

    if (m_debug) {
        Utils::dump(2U, "!!! *TX P25 Network Frame - P25_DUID_TSDU", buffer + 2U, P25_TSDU_FRAME_LENGTH_BYTES);
    }

    if (m_network != NULL)
        m_network->resetP25();
}

/// <summary>
/// Helper to automatically inhibit a source ID on a denial.
/// </summary>
/// <param name="srcId"></param>
void TrunkPacket::denialInhibit(uint32_t srcId)
{
    if (!m_p25->m_inhibitIllegal) {
        return;
    }

    // this check should have already been done -- but do it again anyway
    if (!acl::AccessControl::validateSrcId(srcId)) {
        LogWarning(LOG_P25, P25_TSDU_STR ", denial, system auto-inhibit RID, srcId = %u", srcId);
        writeRF_TSDU_Ext_Func(P25_EXT_FNCT_INHIBIT, P25_WUID_SYS, srcId);
    }
}

/// <summary>
///
/// </summary>
/// <param name="tsbk"></param>
void TrunkPacket::resetStatusCommand(const lc::TSBK& tsbk)
{
    // reset status control data if the status current mode is set and the LCO isn't CALL ALERT
    if (m_statusCmdEnable && ((m_rfTSBK.getLCO() != TSBK_IOSP_CALL_ALRT) && (m_rfTSBK.getLCO() != TSBK_IOSP_EXT_FNCT))) {
        resetStatusCommand();
    }
}

/// <summary>
///
/// </summary>
void TrunkPacket::preprocessStatusCommand()
{
    if (m_statusCmdEnable) {
        m_statusSrcId = m_rfTSBK.getSrcId();
        m_statusValue = m_rfTSBK.getStatus();

        if (m_statusValue != 0U) {
            if ((m_statusValue == m_statusRadioCheck) ||
                (m_statusValue == m_statusRadioInhibit) || (m_statusValue == m_statusRadioUninhibit) ||
                (m_statusValue == m_statusRadioForceReg) || (m_statusValue == m_statusRadioForceDereg)) {
                if (m_verbose) {
                    LogMessage(LOG_RF, P25_TSDU_STR ", TSBK_IOSP_STS_UPDT (Status Update), command mode, statusCurrentStatus = $%02X", m_statusValue);
                }
            }
            else {
                resetStatusCommand();
            }
        }
    }
}

/// <summary>
///
/// </summary>
/// <param name="srcId"></param>
/// <param name="dstId"></param>
/// <returns></returns>
bool TrunkPacket::processStatusCommand(uint32_t srcId, uint32_t dstId)
{
    // is status command mode enabled with status data?
    if (m_statusCmdEnable && (m_statusValue != 0U)) {
        // if the status srcId isn't the CALL ALERT srcId ignore
        if (m_statusSrcId == srcId) {
            if ((m_statusRadioCheck != 0U) && (m_statusValue == m_statusRadioCheck)) {
                writeRF_TSDU_Ext_Func(P25_EXT_FNCT_CHECK, srcId, dstId);
            }
            else if ((m_statusRadioInhibit != 0U) && (m_statusValue == m_statusRadioInhibit)) {
                writeRF_TSDU_Ext_Func(P25_EXT_FNCT_INHIBIT, P25_WUID_SYS, dstId);
            }
            else if ((m_statusRadioUninhibit != 0U) && (m_statusValue == m_statusRadioUninhibit)) {
                writeRF_TSDU_Ext_Func(P25_EXT_FNCT_UNINHIBIT, P25_WUID_SYS, dstId);
            }
            else if ((m_statusRadioForceReg != 0U) && (m_statusValue == m_statusRadioForceReg)) {
                // update dynamic unit registration table
                if (!hasSrcIdUnitReg(srcId)) {
                    m_unitRegTable.push_back(srcId);
                }

                writeRF_TSDU_Grp_Aff_Rsp(srcId, dstId);
            }
            else if ((m_statusRadioForceDereg != 0U) && (m_statusValue == m_statusRadioForceDereg)) {
                writeRF_TSDU_U_Dereg_Ack(srcId);
            }
            else {
                LogError(LOG_P25, P25_TSDU_STR ", unhandled command mode, statusCurrentStatus = $%02X, srcId = %u, dstId = %u", m_statusValue, srcId, dstId);
                resetStatusCommand();
            }

            writeRF_TSDU_ACK_FNE(srcId, TSBK_IOSP_CALL_ALRT, false);
            return true;
        }
        else {
            if (m_verbose) {
                LogWarning(LOG_P25, P25_TSDU_STR ", TSBK_IOSP_STS_UPDT (Status Update), illegal attempt by srcId = %u to access status command", srcId);
            }
        }
    }

    resetStatusCommand();
    return false;
}

/// <summary>
/// Helper to add the idle status bits on P25 frame data.
/// </summary>
/// <param name="data"></param>
/// <param name="length"></param>
/// <param name="b1"></param>
/// <param name="b2"></param>
void TrunkPacket::addIdleBits(uint8_t* data, uint32_t length, bool b1, bool b2)
{
    assert(data != NULL);

    for (uint32_t ss0Pos = P25_SS0_START; ss0Pos < length; ss0Pos += (P25_SS_INCREMENT * 5U)) {
        uint32_t ss1Pos = ss0Pos + 1U;
        WRITE_BIT(data, ss0Pos, b1);
        WRITE_BIT(data, ss1Pos, b2);
    }
}
