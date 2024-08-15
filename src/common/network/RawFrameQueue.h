// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital Voice Modem - Common Library
 * GPLv2 Open Source. Use is subject to license terms.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 *  Copyright (C) 2024 Bryan Biedenkapp, N2PLL
 *
 */
/**
 * @file RawFrameQueue.h
 * @ingroup network_core
 * @file RawFrameQueue.cpp
 * @ingroup network_core
 */
#if !defined(__RAW_FRAME_QUEUE_H__)
#define __RAW_FRAME_QUEUE_H__

#include "common/Defines.h"
#include "common/network/udp/Socket.h"
#include "common/Mutex.h"
#include "common/Utils.h"

namespace network
{
    // ---------------------------------------------------------------------------
    //  Constants
    // ---------------------------------------------------------------------------

    const uint32_t DATA_PACKET_LENGTH = 8192U;

    // ---------------------------------------------------------------------------
    //  Class Declaration
    // ---------------------------------------------------------------------------

    /**
     * @brief Implements the network frame queuing logic.
     * @ingroup network_core
     */
    class HOST_SW_API RawFrameQueue {
    public:
        auto operator=(RawFrameQueue&) -> RawFrameQueue& = delete;
        auto operator=(RawFrameQueue&&) -> RawFrameQueue& = delete;
        RawFrameQueue(RawFrameQueue&) = delete;

        /**
         * @brief Initializes a new instance of the RawFrameQueue class.
         * @param socket Local port used to listen for incoming data.
         * @param debug Flag indicating debug logging should be enabled.
         */
        RawFrameQueue(udp::Socket* socket, bool debug);
        /**
         * @brief Finalizes a instance of the RawFrameQueue class.
         */
        virtual ~RawFrameQueue();

        /**
         * @brief Read message from the received UDP packet.
         * @param[out] messageLength Actual length of message read from packet.
         * @param[out] address IP address data read from.
         * @param[out] addrLen 
         * @return UInt8Array Buffer containing message read.
         */
        UInt8Array read(int& messageLength, sockaddr_storage& address, uint32_t& addrLen);
        /**
         * @brief Write message to the UDP socket.
         * @param[in] message Message buffer to frame and queue.
         * @param length Length of message.
         * @param addr IP address to write data to.
         * @param addrLen 
         * @returns bool True, if message was sent, otherwise false.
         */
        bool write(const uint8_t* message, uint32_t length, sockaddr_storage& addr, uint32_t addrLen);

        /**
         * @brief Cache message to frame queue.
         * @param[in] message Message buffer to frame and queue.
         * @param length Length of message.
         * @param addr IP address to write data to.
         * @param addrLen 
         */
        void enqueueMessage(const uint8_t* message, uint32_t length, sockaddr_storage& addr, uint32_t addrLen);

        /**
         * @brief Flush the message queue.
         */
        bool flushQueue();

    protected:
        sockaddr_storage m_addr;
        uint32_t m_addrLen;
        udp::Socket* m_socket;

        static Mutex m_flushMutex;
        udp::BufferVector m_buffers;

        bool m_debug;

    private:
        /**
         * @brief Helper to ensure buffers are deleted.
         */
        void deleteBuffers();
    };
} // namespace network

#endif // __FRAME_QUEUE_H__