#ifndef LOCKER_H
#define LOCKER_H

#include <pthread.h>
#include <semaphore.h>

#include <exception>
// 线程同步机制封装类

// 互斥锁类
class locker {
private:
    pthread_mutex_t m_mutex;  // 互斥锁

public:
    locker()  // 互斥量构造
    {
        if (pthread_mutex_init(&m_mutex, NULL) != 0) {
            throw std::exception();
        }
    }

    ~locker()  // 互斥量析构
    {
        pthread_mutex_destroy(&m_mutex);
    }

    bool lock()  // 互斥量上锁
    {
        return pthread_mutex_lock(&m_mutex) == 0;
    }

    bool unlock()  // 互斥量解锁
    {
        return pthread_mutex_unlock(&m_mutex) == 0;
    }

    pthread_mutex_t *get()  // 获取互斥量成员
    {
        return &m_mutex;
    }
};

// 条件变量类
class cond {
private:
    pthread_cond_t m_cond;

public:
    cond()  // 构造
    {
        if (pthread_cond_init(&m_cond, NULL) != 0) {
            throw std::exception();
        }
    }

    ~cond()  // 析构
    {
        pthread_cond_destroy(&m_cond);
    }

    bool wait(pthread_mutex_t *mutex) {
        return pthread_cond_wait(&m_cond, mutex) == 0;
    }

    bool timedwait(pthread_mutex_t *mutex, struct timespec t) {
        return pthread_cond_timedwait(&m_cond, mutex, &t) == 0;
    }

    bool signal() {  // 唤醒一个线程
        return pthread_cond_signal(&m_cond) == 0;
    }

    bool broadcast() {  // 唤醒所有线程
        return pthread_cond_broadcast(&m_cond) == 0;
    }
};

// 信号量类
class sem {
private:
    sem_t m_sem;

public:
    sem() {
        if (sem_init(&m_sem, 0, 0) != 0) {
            throw std::exception();
        }
    }

    sem(int num) {
        if (sem_init(&m_sem, 0, num) != 0) {
            throw std::exception();
        }
    }

    ~sem() {
        sem_destroy(&m_sem);
    }

    bool wait()  // 等待信号量  p操作
    {
        return sem_wait(&m_sem) == 0;
    }
    bool post()  // 增加信号量  v操作
    {
        return sem_post(&m_sem) == 0;
    }
};
#endif
