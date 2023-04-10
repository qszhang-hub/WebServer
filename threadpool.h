#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <pthread.h>

#include <iostream>
#include <list>

#include "locker.h"

// 线程池类 T是任务类
template <typename T>
class threadpool {
private:
    // 线程的数量
    int m_thread_num;

    // 线程池数组
    pthread_t *m_threads;

    // 工作队列
    std::list<T *> m_workqueue;

    // 工作队列最多允许等待请求数量
    int m_max_requests;

    // 互斥锁
    locker m_queuelocker;

    // 信号量，用于判断是否有任务需要处理
    sem m_queuestat;

    // 是否结束线程
    bool m_stop;

public:
    threadpool(int thread_num = 8, int max_requests = 10000);
    ~threadpool();
    bool append(T *task);

private:
    // 因为所有的成员函数都会默认带一个this参数指向本类
    // 但是多线程的worker不允许多参数，故必须设为静态函数，保证能调用
    // 原理：静态成员函数是没有this指针的
    static void *worker(void *arg);
    void run();
};

template <typename T>
threadpool<T>::threadpool(int thread_num, int max_requests)
    : m_thread_num(thread_num), m_max_requests(max_requests), m_stop(false), m_threads(NULL) {
    if ((thread_num <= 0) | (max_requests <= 0)) {
        throw std::exception();
    }
    m_threads = new pthread_t[m_thread_num];
    // new创建失败
    if (m_threads == nullptr) {
        throw std::exception();
    }

    // 创建thread_num个线程，并设置线程脱离
    for (int i = 0; i < thread_num; ++i) {
        std::cout << "正在创建第 " << i + 1 << "个线程" << std::endl;

        // 由于静态函数无法访问非静态成员，故通过将this指针传递过去作为参数来访问
        if (pthread_create(&m_threads[i], NULL, worker, this) != 0) {
            delete[] m_threads;
            throw std::exception();
        }

        if (pthread_detach(m_threads[i]) != 0) {
            delete[] m_threads;
            throw std::exception();
        }
    }
}

template <typename T>
threadpool<T>::~threadpool() {
    delete[] m_threads;
    m_stop = true;
}

template <typename T>
bool threadpool<T>::append(T *task) {
    m_queuelocker.lock();
    // 工作队列已达上限，不予添加
    if (m_workqueue.size() >= m_max_requests) {
        m_queuelocker.unlock();
        return false;
    }
    m_workqueue.push_back(task);
    m_queuelocker.unlock();
    // 信号量增加
    m_queuestat.post();
    return true;
}

template <typename T>
void *threadpool<T>::worker(void *arg) {
    // 接收一下this指针
    threadpool *pool = (threadpool *)arg;
    pool->run();
    return NULL;
}

template <typename T>
void threadpool<T>::run() {
    while (!m_stop) {
        // 等待工作队列中有任务
        m_queuestat.wait();

        m_queuelocker.lock();
        if (m_workqueue.empty()) {
            m_queuelocker.unlock();
            continue;
        }
        T *task = m_workqueue.front();
        m_workqueue.pop_front();
        m_queuelocker.unlock();
        if (task == NULL) {
            continue;
        }
        task->process();
    }
}
#endif
