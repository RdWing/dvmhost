// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital Voice Modem - Common Library
 * GPLv2 Open Source. Use is subject to license terms.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 *  Copyright (C) 2018 Jonathan Naylor, G4KLX
 *  Copyright (C) 2022,2024 Bryan Biedenkapp, N2PLL
 *
 */
#include "nxdn/channel/SACCH.h"
#include "nxdn/edac/Convolution.h"
#include "nxdn/NXDNDefines.h"
#include "edac/CRC.h"
#include "Log.h"
#include "Utils.h"

using namespace nxdn;
using namespace nxdn::defines;
using namespace nxdn::channel;

#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

const uint32_t INTERLEAVE_TABLE[] = {
    0U, 5U, 10U, 15U, 20U, 25U, 30U, 35U, 40U, 45U, 50U, 55U,
    1U, 6U, 11U, 16U, 21U, 26U, 31U, 36U, 41U, 46U, 51U, 56U,
    2U, 7U, 12U, 17U, 22U, 27U, 32U, 37U, 42U, 47U, 52U, 57U,
    3U, 8U, 13U, 18U, 23U, 28U, 33U, 38U, 43U, 48U, 53U, 58U,
    4U, 9U, 14U, 19U, 24U, 29U, 34U, 39U, 44U, 49U, 54U, 59U
};

const uint32_t PUNCTURE_LIST[] = { 5U, 11U, 17U, 23U, 29U, 35U, 41U, 47U, 53U, 59U, 65U, 71U };

// ---------------------------------------------------------------------------
//  Public Class Members
// ---------------------------------------------------------------------------

/* Initializes a new instance of the SACCH class. */

SACCH::SACCH() :
    m_ran(0U),
    m_structure(ChStructure::SR_SINGLE),
    m_data(nullptr)
{
    m_data = new uint8_t[NXDN_SACCH_CRC_LENGTH_BYTES];
    ::memset(m_data, 0x00U, NXDN_SACCH_CRC_LENGTH_BYTES);
}

/* Initializes a copy instance of the SACCH class. */

SACCH::SACCH(const SACCH& data) :
    m_ran(0U),
    m_structure(ChStructure::SR_SINGLE),
    m_data(nullptr)
{
    copy(data);
}

/* Finalizes a instance of SACCH class. */

SACCH::~SACCH()
{
    delete[] m_data;
}

/* Equals operator. */

SACCH& SACCH::operator=(const SACCH& data)
{
    if (&data != this) {
        ::memcpy(m_data, data.m_data, NXDN_SACCH_CRC_LENGTH_BYTES);

        m_ran = m_data[0U] & 0x3FU;
        m_structure = (ChStructure::E)((m_data[0U] >> 6) & 0x03U);
    }

    return *this;
}

/* Decode a slow associated control channel. */

bool SACCH::decode(const uint8_t* data)
{
    assert(data != nullptr);

    uint8_t buffer[NXDN_SACCH_FEC_LENGTH_BYTES];
    ::memset(buffer, 0x00U, NXDN_SACCH_FEC_LENGTH_BYTES);

    // deinterleave
    for (uint32_t i = 0U; i < NXDN_SACCH_FEC_LENGTH_BITS; i++) {
        uint32_t n = INTERLEAVE_TABLE[i] + NXDN_FSW_LENGTH_BITS + NXDN_LICH_LENGTH_BITS;
        bool b = READ_BIT(data, n);
        WRITE_BIT(buffer, i, b);
    }

#if DEBUG_NXDN_SACCH
    Utils::dump(2U, "SACCH::decode(), SACCH Raw", buffer, NXDN_SACCH_FEC_LENGTH_BYTES);
#endif

    // depuncture
    uint8_t puncture[90U];
    ::memset(puncture, 0x00U, 90U);

    uint32_t n = 0U, index = 0U;
    for (uint32_t i = 0U; i < NXDN_SACCH_FEC_LENGTH_BITS; i++) {
        if (n == PUNCTURE_LIST[index]) {
            puncture[n++] = 1U;
            index++;
        }

        bool b = READ_BIT(buffer, i);
        puncture[n++] = b ? 2U : 0U;
    }

    for (uint32_t i = 0U; i < 8U; i++) {
        puncture[n++] = 0U;
    }

    // decode convolution
    edac::Convolution conv;
    conv.start();

    n = 0U;
    for (uint32_t i = 0U; i < (NXDN_SACCH_CRC_LENGTH_BITS + 4U); i++) {
        uint8_t s0 = puncture[n++];
        uint8_t s1 = puncture[n++];

        if (!conv.decode(s0, s1)) {
            LogError(LOG_NXDN, "SACCH::decode(), failed to decode convolution, i = %u, n = %u", i);
            return false;
        }
    }

    conv.chainback(m_data, NXDN_SACCH_CRC_LENGTH_BITS);

#if DEBUG_NXDN_SACCH
    Utils::dump(2U, "Decoded SACCH", m_data, NXDN_SACCH_CRC_LENGTH_BYTES);
#endif

    // check CRC-6
    bool ret = edac::CRC::checkCRC6(m_data, NXDN_SACCH_LENGTH_BITS);
    if (!ret) {
        LogError(LOG_NXDN, "SACCH::decode(), failed CRC-6 check");
        return false;
    }

    m_ran = m_data[0U] & 0x3FU;
    m_structure = (ChStructure::E)((m_data[0U] >> 6) & 0x03U);

    return true;
}

/* Encode a slow associated control channel. */

void SACCH::encode(uint8_t* data) const
{
    assert(data != nullptr);

    m_data[0U] &= 0xC0U;
    m_data[0U] |= m_ran;

    m_data[0U] &= 0x3FU;
    m_data[0U] |= (m_structure << 6) & 0xC0U;

    uint8_t buffer[NXDN_SACCH_CRC_LENGTH_BYTES];
    ::memset(buffer, 0x00U, NXDN_SACCH_CRC_LENGTH_BYTES);

    for (uint32_t i = 0U; i < NXDN_SACCH_LENGTH_BITS; i++) {
        bool b = READ_BIT(m_data, i);
        WRITE_BIT(buffer, i, b);
    }

    edac::CRC::addCRC6(buffer, NXDN_SACCH_LENGTH_BITS);

#if DEBUG_NXDN_SACCH
        Utils::dump(2U, "Encoded SACCH", buffer, NXDN_SACCH_CRC_LENGTH_BYTES);
#endif

    // encode convolution
    uint8_t convolution[NXDN_SACCH_FEC_CONV_LENGTH_BYTES];
    ::memset(convolution, 0x00U, NXDN_SACCH_FEC_CONV_LENGTH_BYTES);

    edac::Convolution conv;
    conv.encode(buffer, convolution, NXDN_SACCH_CRC_LENGTH_BITS);

    // puncture
    uint8_t puncture[NXDN_SACCH_FEC_LENGTH_BYTES];
    ::memset(puncture, 0x00U, NXDN_SACCH_FEC_LENGTH_BYTES);

    uint32_t n = 0U, index = 0U;
    for (uint32_t i = 0U; i < NXDN_SACCH_FEC_CONV_LENGTH_BITS; i++) {
        if (i != PUNCTURE_LIST[index]) {
            bool b = READ_BIT(convolution, i);
            WRITE_BIT(puncture, n, b);
            n++;
        } else {
            index++;
        }
    }

    // interleave
    for (uint32_t i = 0U; i < NXDN_SACCH_FEC_LENGTH_BITS; i++) {
        uint32_t n = INTERLEAVE_TABLE[i] + NXDN_FSW_LENGTH_BITS + NXDN_LICH_LENGTH_BITS;
        bool b = READ_BIT(puncture, i);
        WRITE_BIT(data, n, b);
    }

#if DEBUG_NXDN_SACCH
    Utils::dump(2U, "SACCH::encode(), SACCH Puncture and Interleave", data, NXDN_SACCH_FEC_LENGTH_BYTES);
#endif
}

/* Gets the raw SACCH data. */

void SACCH::getData(uint8_t* data) const
{
    assert(data != nullptr);

    uint32_t offset = 8U;
    for (uint32_t i = 0U; i < (NXDN_SACCH_LENGTH_BITS - 8); i++, offset++) {
        bool b = READ_BIT(m_data, offset);
        WRITE_BIT(data, i, b);
    }
}

/* Sets the raw SACCH data. */

void SACCH::setData(const uint8_t* data)
{
    assert(data != nullptr);

    uint32_t offset = 8U;
    for (uint32_t i = 0U; i < (NXDN_SACCH_LENGTH_BITS - 8); i++, offset++) {
        bool b = READ_BIT(data, i);
        WRITE_BIT(m_data, offset, b);
    }
}

// ---------------------------------------------------------------------------
//  Private Class Members
// ---------------------------------------------------------------------------

/* Internal helper to copy the the class. */

void SACCH::copy(const SACCH& data)
{
    m_data = new uint8_t[NXDN_SACCH_CRC_LENGTH_BYTES];
    ::memcpy(m_data, data.m_data, NXDN_SACCH_CRC_LENGTH_BYTES);

    m_ran = m_data[0U] & 0x3FU;
    m_structure = (ChStructure::E)((m_data[0U] >> 6) & 0x03U);
}
