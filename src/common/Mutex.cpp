// SPDX-License-Identifier: GPL-2.0-only
/*
 * Digital Voice Modem - Common Library
 * GPLv2 Open Source. Use is subject to license terms.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 *  Copyright (C) 2024 Bryan Biedenkapp, N2PLL
 *
 */
#include "Mutex.h"

// ---------------------------------------------------------------------------
//  Public Class Members
// ---------------------------------------------------------------------------

/* Initializes a new instance of the Mutex class. */

Mutex::Mutex() noexcept :
#ifdef _WIN32
    m_mutex()
#else
    m_mutex(PTHREAD_MUTEX_INITIALIZER)
#endif
{
#ifdef _WIN32
    m_mutex = ::CreateMutex(NULL, FALSE, NULL);
#endif
}

/* Finalizes a instance of the Mutex class. */

Mutex::~Mutex() noexcept
{
#ifdef _WIN32
    ::CloseHandle(m_mutex);
#endif
}

/* Locks the mutex, blocks if the mutex is not available. */

void Mutex::lock()
{
#ifdef _WIN32
    ::WaitForSingleObject(m_mutex, INFINITE);
#else
    ::pthread_mutex_lock(&m_mutex);
#endif
}

/* Unlocks the mutex. */

void Mutex::unlock()
{
#ifdef _WIN32
    ::ReleaseMutex(m_mutex);
#else
    ::pthread_mutex_unlock(&m_mutex);
#endif
}
