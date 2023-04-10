/*
    循环数组实现的阻塞队列
 */
#ifndef BLOCK_QUEUE_H
#define BLOCK_QUEUE_H

#include <pthread.h>
#include <stdlib.h>
#include <sys/time.h>

#include <iostream>

#include "locker.h"
using namespace std;

template <class T>
class block_queue {
public:
    block_queue(int max_size = 1000) {
        if (max_size <= 0) {
            exit(-1);
        }

        m_max_size = max_size;      // 阻塞队列最大长度
        m_array = new T[max_size];  // 队列数组，存放数据
        m_size = 0;                 // 当前大小
        m_front = -1;               // 队首索引
        m_back = -1;                // 队尾索引
    }

    // 清空队列
    void clear() {
        m_lock.lock();
        m_size = 0;
        m_front = -1;
        m_back = -1;
        m_lock.unlock();
    }

    ~block_queue() {
        m_lock.lock();
        if (m_array != NULL) {
            delete[] m_array;
        }
        m_lock.unlock();
    }

    // 判断队列是否满了
    bool full() {
        m_lock.lock();
        // 当前大小已达最大容量
        if (m_size >= m_max_size) {
            m_lock.unlock();
            return true;
        }
        m_lock.unlock();
        return false;
    }

    // 判断队列是否为空
    bool empty() {
        m_lock.lock();
        if (0 == m_size) {
            m_lock.unlock();
            return true;
        }
        m_lock.unlock();
        return false;
    }
    // 返回队首元素
    bool front(T &value) {
        m_lock.lock();
        if (0 == m_size) {
            m_lock.unlock();
            return false;
        }
        value = m_array[m_front];
        m_lock.unlock();
        return true;
    }

    // 返回队尾元素
    bool back(T &value) {
        m_lock.lock();
        if (0 == m_size) {
            m_lock.unlock();
            return false;
        }
        value = m_array[m_back];
        m_lock.unlock();
        return true;
    }

    int size() {
        // 之所以要用tmp变量临时存储m_size是为了加锁退锁
        int tmp = 0;

        m_lock.lock();
        tmp = m_size;

        m_lock.unlock();
        return tmp;
    }

    int max_size() {
        int tmp = 0;

        m_lock.lock();
        tmp = m_max_size;

        m_lock.unlock();
        return tmp;
    }
    // 往队列添加元素，需要将所有使用队列的线程先唤醒
    // 当有元素push进队列,相当于生产者生产了一个元素
    // 若当前没有线程等待条件变量,则唤醒无意义
    bool push(const T &item) {
        m_lock.lock();

        // 已达上限
        if (m_size >= m_max_size) {
            m_cond.broadcast();  // 唤醒所有因条件变量阻塞的等待线程，让它们赶快消费
            m_lock.unlock();
            return false;
        }

        m_back = (m_back + 1) % m_max_size;  // 循环队列队尾索引
        m_array[m_back] = item;              // 放入队尾

        m_size++;  // 队列长度+1

        m_cond.signal();  // 唤醒一个线程，让它消费
        m_lock.unlock();
        return true;
    }

    // pop时,如果当前队列没有元素,将会等待条件变量
    bool pop(T &item) {
        m_lock.lock();
        while (m_size <= 0) {
            // 条件变量还不满足，阻塞当前线程并等待
            if (!m_cond.wait(m_lock.get())) {
                m_lock.unlock();
                return false;
            }
        }
        // 由于front和back都是从-1开始，而back从0号索引开始放入
        // 故front每次滞后一个单位，要先自增一，再获取元素
        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_lock.unlock();
        return true;
    }

    // 增加了超时处理  ms_timeout:等待时间(单位：ms)
    bool pop(T &item, int ms_timeout) {
        struct timespec t = {0, 0};
        struct timeval now = {0, 0};
        gettimeofday(&now, NULL);  // 获取当前时间
        m_lock.lock();
        if (m_size <= 0) {
            // 需要等待到的时间是当前时间+等待时长
            t.tv_sec = now.tv_sec + ms_timeout / 1000;
            t.tv_nsec = (ms_timeout % 1000) * 1000;
            // 规定时间内没有生产者生产出产品
            if (!m_cond.timedwait(m_lock.get(), t)) {
                m_lock.unlock();
                return false;
            }
        }

        m_front = (m_front + 1) % m_max_size;
        item = m_array[m_front];
        m_size--;
        m_lock.unlock();
        return true;
    }

private:
    locker m_lock;  // 互斥锁
    cond m_cond;    // 条件变量

    T *m_array;      // 队列数组，存放数据
    int m_size;      // 当前队列大小
    int m_max_size;  // 队列最大长度
    int m_front;     // 队首索引
    int m_back;      // 队尾索引
};

#endif
