#ifndef _TIMEHEAP_H_
#define _TIMEHEAP_H_

#include <netinet/in.h>
#include <time.h>

#include <iostream>

#include "http_conn.h"

class HeapTimer;

// 用户数据，绑定socket和定时器
struct client_data {
    sockaddr_in address;
    int sock_fd;
    HeapTimer* timer;
};

// 定时器类
class HeapTimer {
public:
    time_t expire;  // 定时器生效的绝对时间
    // client_data* user_data;
    http_conn* user_data;

public:
    HeapTimer() {}
    void (*callback)(http_conn*);  // 定时器的回调函数
};
// 仿照vector扩容机制实现堆数组
class TimeHeap {
private:
    HeapTimer** m_timers;  // 堆数组，每个元素都是一个计时器指针
    int m_capacity;        // 堆数组容量
    int m_size;            // 堆数组当前包含元素个数
public:
    TimeHeap(int capacity);  // 构造函数1，初始化大小为cap的空数组
    TimeHeap(HeapTimer** timers, int size, int capacity);  // 构造函数2，根据已有数组初始化堆
    ~TimeHeap();

public:
    void shift_down(int hole);  // 对堆结点进行下虑
    void add_timer(HeapTimer* timer);
    void del_timer(HeapTimer* timer);
    void pop_timer();
    void tick();
    // 当堆数组容量不够时，对其进行扩容
    void reallocate();
};

#endif
