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
  * @file Mutex.h
  * @ingroup threading
  * @file Mutex.cpp
  * @ingroup threading
  */
#if !defined(__MUTEX_H__)
#define __MUTEX_H__

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <pthread.h>
#endif // defined(_WIN32)

#if defined(_WIN32)
typedef HANDLE pthread_mutex_t;
#endif // defined(_WIN32)

#define HOST_SW_API 

// ---------------------------------------------------------------------------
//  Class Declaration
// ---------------------------------------------------------------------------

/**
 * @brief Synchronization primitive that can be used to protect shared data from being
 *  simultaneously accessed by multiple threads
 * @ingroup threading
 */
class HOST_SW_API Mutex {
public:
    /**
     * @brief Initializes a new instance of the Mutex class.
     */
    Mutex() noexcept;
    /**
     * @brief Finalizes a instance of the Mutex class.
     */
    ~Mutex() noexcept;

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    /**
     * @brief Locks the mutex, blocks if the mutex is not available.
     */
    void lock();

    /**
     * @brief Unlocks the mutex.
     */
    void unlock();

private:
    pthread_mutex_t m_mutex;
};

// ---------------------------------------------------------------------------
//  Class Declaration
// ---------------------------------------------------------------------------

/**
 * @brief Mutex wrapper that provides a convenient RAII-style mechanism for owning a 
 *  mutex for the duration of a scoped block.
 * @ingroup threading
 */
class HOST_SW_API LockGuard {
public:
    /**
     * @brief Initializes a new instance of the LockGuard class.
     */
    explicit LockGuard(Mutex& mtx) :
        m_mutex(mtx)
    {
        m_mutex.lock();
    }
    /**
     * @brief Finalizes a instance of the LockGuard class.
     */
    ~LockGuard() noexcept
    {
        m_mutex.unlock();
    }

    LockGuard(const LockGuard&) = delete;
    LockGuard& operator=(const LockGuard&) = delete;

private:
    Mutex& m_mutex;
};

#endif // __THREAD_H__
