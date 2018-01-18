#ifndef THREADSAFEQUEUE_H_
#define THREADSAFEQUEUE_H_
//------------------------------------------------------------------------------
//
// Project: Xpo3
// Module: ThreadSafeQueue
// File: ThreadSafeQueue.h
// Author: Mirza Muhic
//
//------------------------------------------------------------------------------
/// @file
/// @brief This file contains the XPO3 ThreadSafeQueue class template.
/// The queue is an optionally bounded FIFO.
///
//------------------------------------------------------------------------------
//
// Copyright (c) Ericsson AB
//
// P R O P R I E T A R Y    &    C O N F I D E N T I A L
//
// The copyright of this document is vested in Ericsson AB without
// whose prior written permission its contents must not be published, adapted or
// reproduced in any form or disclosed or issued to any third party.
//
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// Module include files.
//------------------------------------------------------------------------------

//------------------------------------------------------------------------------
// System include files.
//------------------------------------------------------------------------------
#include <list>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <algorithm>
#include <memory>
#include <chrono>
#include <stdint.h>

//------------------------------------------------------------------------------
// Project include files.
//------------------------------------------------------------------------------

namespace
{
} // namespace


//------------------------------------------------------------------------------
//
template<typename T> class ThreadSafeQueue
//
/// @brief This class provides a wrapper around an std::queue to make it safe
/// for concurrent access by multiple threads. It provides the ability to block
/// a thread on an empty queue while it is waiting for an element to be placed
/// in the queue.
///
//------------------------------------------------------------------------------
{
public:
    explicit ThreadSafeQueue(const uint32_t maxEntries = 0)
        :
        m_mutex(),
        m_queue(),
        m_conditionVariable(),
        m_maxEntries(maxEntries)
    {}

    /// @brief virtual destructor
    virtual ~ThreadSafeQueue()
    {}

    /// @brief Disable unwanted constructors and assignment operators.
    ThreadSafeQueue( const ThreadSafeQueue& ) = delete;
    ThreadSafeQueue( ThreadSafeQueue&& ) = delete;
    ThreadSafeQueue& operator=( ThreadSafeQueue&& ) = delete;
    ThreadSafeQueue& operator=( const ThreadSafeQueue& ) = delete;

    /// @brief Appends all items from a source queue onto this one
    ///
    /// @param srcQueue The queue from which to move all items across
    /// @return number of items moved
    size_t AppendAllItems(ThreadSafeQueue<T>& srcQueue)
    {
        std::lock_guard<std::mutex> srcMutex(srcQueue.m_mutex);
        std::lock_guard<std::mutex> theMutex(m_mutex);
        bool itemsAdded = false;
        size_t numberOfMovedItems = srcQueue.m_queue.size();

        while (!srcQueue.m_queue.empty())
        {
            MakeRoom();
            T item = std::move(srcQueue.m_queue.front());
            srcQueue.m_queue.pop_front();
            m_queue.push_back(std::move(item));
            itemsAdded = true;
        }

        if (itemsAdded)
        {
            m_conditionVariable.notify_one();
        }
        return numberOfMovedItems;
    }

    /// @brief Pushes an item into the queue.
    /// This method retains a valid user copy of the pushed item.
    /// @param item the new item to be pushed onto the queue.
    void Push( const T& item )
    {
        std::lock_guard<std::mutex> theMutex(m_mutex);
        MakeRoom();
        m_queue.push_back(item);
        m_conditionVariable.notify_one();
    }

    /// @brief Pushes an item into the queue.
    /// This method invalidates user copy of the pushed item.
    /// @param item the new item to be pushed onto the queue.
    void Push( T&& item )
    {
        std::lock_guard<std::mutex> theMutex(m_mutex);
        MakeRoom();
        m_queue.push_back(std::move(item));
        m_conditionVariable.notify_one();
    }

    /// @brief Waits indefinetelly on an empty queue, popping the next
    /// item off the queue as it becomes available.
    /// @return item popped off the queue.
    T WaitAndPop()
    {
        std::unique_lock<std::mutex> theMutex(m_mutex);
        m_conditionVariable.wait(theMutex, [this]{ return !m_queue.empty(); });
        T item = std::move(m_queue.front());
        m_queue.pop_front();
        return item;
    }

    /// @brief Waits until either an item is available or a timeout, popping the next
    /// item off the queue as it becomes available.
    /// @param item the item popped off the queue.
    /// @param duration the timeout.
    /// @return true if successful, false if timedout.
    template<class Rep, class Period>
    bool WaitAndPop(T& item, const std::chrono::duration<Rep, Period>& duration)
    {
        std::chrono::duration<Rep, Period> nonConstDuration = duration;
        return WaitAndPop(item, nonConstDuration);
    }

    /// @brief Waits until either an item is available or a timeout
    /// @param duration the timeout.
    /// @return true if successful, false if timedout.
    template<class Rep, class Period>
    bool Wait(const std::chrono::duration<Rep, Period>& duration)
    {
        std::chrono::duration<Rep, Period> nonConstDuration = duration;
        return Wait(nonConstDuration);
    }

    /// @brief Waits until either an item is available or a timeout, popping the next
    /// item off the queue as it becomes available.
    /// @param item the item popped off the queue.
    /// @param duration the timeout on entry, remaining time on exit.
    /// @return true if successful, false if timedout.
    template<class Rep, class Period>
    bool WaitAndPop(T& item, std::chrono::duration<Rep, Period>& duration)
    {
        std::unique_lock<std::mutex> theMutex(m_mutex);
        if (Wait(duration, theMutex))
        {
            item = std::move(m_queue.front());
            m_queue.pop_front();
            return true;
        }
        return false;
    }

    /// @brief Waits until either an item is available or a timeout.
    /// @param duration the timeout on entry, remaining time on exit.
    /// @return true if successful, false if timedout.
    template<class Rep, class Period>
    bool Wait(std::chrono::duration<Rep, Period>& duration)
    {
        std::unique_lock<std::mutex> theMutex(m_mutex);
        return Wait(duration, theMutex);
    }

    /// @brief Tries to pop the next item off the queue if available.
    /// @param item the returned item.
    /// @return true if successful, false if queue is empty.
    bool TryPop( T& item )
    {
        std::lock_guard<std::mutex> theMutex(m_mutex);
        if (m_queue.empty())
        {
            return false;
        }
        item = std::move(m_queue.front());
        m_queue.pop_front();
        return true;
    }

    /// @brief Tries to pop a matching item off the queue if available.
    /// @param item the returned item.
    /// @param condition the condition used to find the item.
    /// @return true if successful, false if a matching item isn't found
    template<typename _Predicate> bool PopIf(T& item, _Predicate condition)
    {
        std::lock_guard<std::mutex> theMutex(m_mutex);
        return PopIfNoLock(item, condition);
    }

    /// @brief Tries to pop a matching item off the queue if available.
    /// @param item the returned item.
    /// @param duration the timeout on entry, remaining time on exit.
    /// @param condition the condition used to find the item.
    /// @return true if successful, false if a matching item isn't found
    template<class Rep, class Period, typename _Predicate>
    bool WaitAndPopIf(T& item, std::chrono::duration<Rep, Period>& duration, _Predicate condition)
    {
        std::unique_lock<std::mutex> theMutex(m_mutex);
        return WaitFor(duration, theMutex, [this, &item, &condition] { return this->PopIfNoLock(item, condition); });
    }

    /// @brief Tests if the queue is empty.
    /// @return true if the queue is empty.
    bool Empty() const
    {
        std::lock_guard<std::mutex> theMutex(m_mutex);
        return m_queue.empty();
    }

    /// @brief Obtains the size (number of items) of the queue.
    /// @return number of items in the queue.
    size_t Size() const
    {
        std::lock_guard<std::mutex> theMutex(m_mutex);
        return m_queue.size();
    }

    /// @brief Clears the queue setting its size to 0.
    void Clear()
    {
        std::lock_guard<std::mutex> theMutex(m_mutex);
        std::list<T>().swap(m_queue);
    }

protected:
    void MakeRoom()
    {
        if (m_maxEntries)
        {
            // Discard oldest entries to ensure maximum size is not exceeded
            while (m_queue.size() >= m_maxEntries)
            {
                m_queue.pop_front();
            }
        }
    }

    /// @brief Waits until either an item is available or a timeout.
    /// @param duration the timeout on entry, remaining time on exit.
    /// @param theMutex the mutex used to protect the queue.
    /// @return true if successful, false if timedout.
    template<class Rep, class Period>
    bool Wait(std::chrono::duration<Rep, Period>& duration, std::unique_lock<std::mutex>& theMutex)
    {
        return WaitFor(duration, theMutex, [this]{ return !m_queue.empty(); });
    }

    /// @brief Waits until either the specified condition is met, or a timeout.
    /// @param duration the timeout on entry, remaining time on exit.
    /// @param theMutex the mutex used to protect the queue.
    /// @param conditionCheck   Function that checks if the condition is met
    /// @return true if successful, false if timedout.
    template<class Rep, class Period, class ConditionCheckFn>
    bool WaitFor(std::chrono::duration<Rep, Period>& duration, std::unique_lock<std::mutex>& theMutex,
                 ConditionCheckFn conditionCheck)
    {
        typedef std::chrono::duration<Rep, Period> DurationType;
        typedef std::chrono::system_clock ClockType; // Ideally steady_clock but this requires GCC 4.7
        auto const timeout = ClockType::now() + std::chrono::duration_cast<ClockType::duration>(duration);
        bool timedOut = !m_conditionVariable.wait_until(theMutex, timeout, conditionCheck);
        auto const endTime = ClockType::now();
        if (timedOut)
        {
            duration = DurationType{0};
            return false;
        }
        duration = std::chrono::duration_cast<DurationType>(timeout - endTime);
        return true;
    }

    /// @see PopIf
    template<typename _Predicate> bool PopIfNoLock(T& item, _Predicate& condition)
    {
        if (m_queue.empty())
        {
            return false;
        }
        auto it = std::find_if(m_queue.begin(), m_queue.end(), condition);
        if (it != m_queue.end())
        {
            item = std::move(*it);
            m_queue.erase(it);
            return true;
        }
        return false;
    }

    // Mutex must be mutable because, for example, invocation of Empty() which is a const
    // member function will result in a (temporary) change in the mutex (and hence the
    // physical state of the object). However, the logical state of the object, in particular
    // that of the underlying queue itself, remains unchanged throughout.
    mutable std::mutex      m_mutex;
    std::list<T>            m_queue;
    std::condition_variable m_conditionVariable;
    uint32_t                m_maxEntries;
};

//------------------------------------------------------------------------------
// End of file
//------------------------------------------------------------------------------
#endif // THREADSAFEQUEUE_H_

