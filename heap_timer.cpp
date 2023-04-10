#include "heap_timer.h"

TimeHeap::TimeHeap(int capacity) : m_capacity(capacity), m_size(0) {
    m_timers = new HeapTimer*[m_capacity];
    if (m_timers == nullptr) {
        throw std::exception();
    }

    for (int i = 0; i < m_capacity; i++) {
        m_timers[i] = nullptr;
    }
}

TimeHeap::TimeHeap(HeapTimer** timers, int size, int capacity)
    : m_size(size), m_capacity(capacity) {
    // 参数不对
    if (m_capacity < size) {
        throw std::exception();
    }

    m_timers = new HeapTimer*[m_capacity];

    // 内存不足
    if (m_timers == nullptr) {
        throw std::exception();
    }

    // 拷贝
    for (int i = 0; i < size; i++) {
        m_timers[i] = timers[i];
    }

    // 从最后一个节点的父节点开始遍历，下沉操作
    for (int i = size / 2 - 1; i >= 0; i--) {
        shift_down(i);
    }
}

TimeHeap::~TimeHeap() {
    // for (int i = 0; i < m_size; i++) {
    //     if (!m_timers[i]) {
    //         delete m_timers[i];
    //     }
    // }
    if (m_timers != nullptr) {
        delete[] m_timers;
    }
}

// 对堆结点进行下滤，确保第k个节点满足最小堆性质
void TimeHeap::shift_down(int k) {
    HeapTimer* timer = m_timers[k];
    int i = k * 2 + 1;
    while (i < m_size) {
        // 使用较小的子节点
        if (i < m_size - 1 && m_timers[i]->expire > m_timers[i + 1]->expire) {
            ++i;
        }
        // 子树的根节点值大于较小值，根节点下沉
        if (timer->expire > m_timers[i]->expire) {
            m_timers[i]->index = k;
            m_timers[k] = m_timers[i];
            k = i;
        } else {
            // tmp节点的值最小，符合
            break;
        }
    }
    // 将最初的节点放到合适的位置
    m_timers[k] = timer;
    timer->index = k;
}

// 添加定时器，先放在数组末尾，在进行上滤使其满足最小堆
void TimeHeap::add_timer(HeapTimer* timer) {
    if (timer == nullptr) {
        return;
    }

    // 空间不足，将堆空间扩大为原来的2倍
    if (m_size >= m_capacity) {
        reallocate();
    }

    // 获取新节点下标
    int i = m_size;
    ++m_size;

    // 父节点
    int parent = (i - 1) / 2;

    // 父节点就是它自己，说明它是第一个元素，直接放入
    if (i == parent) {
        m_timers[i] = timer;
        timer->index = i;
        return;
    }

    // 由于新结点在最后，因此将其进行上滤，以符合最小堆
    while (parent >= 0) {
        if (m_timers[parent]->expire > timer->expire) {
            m_timers[parent]->index = i;
            m_timers[i] = m_timers[parent];
            i = parent;
        } else {
            break;
        }
    }
    m_timers[i] = timer;
    timer->index = i;
}

// 删除指定定时器
void TimeHeap::del_timer(HeapTimer* timer) {
    if (timer == nullptr) {
        return;
    }
    // 仅仅将回调函数置空，虽然节省删除的开销，但会造成数组膨胀
    timer->callback = nullptr;
}

// 调整指定定时器在堆中的位置
void TimeHeap::adjust_timer(HeapTimer* timer) {
    if (timer == nullptr) {
        return;
    }

    // 从timer所在位置进行一次下沉操作即可
    shift_down(timer->index);
    timer->callback = nullptr;
}

// 删除堆顶定时器
void TimeHeap::pop_timer() {
    if (m_size <= 0) {
        return;
    }
    if (m_timers[0] != nullptr) {
        delete m_timers[0];
        // 将最后一个定时器赋给堆顶
        --m_size;
        m_timers[m_size]->index = 0;
        m_timers[0] = m_timers[m_size];

        // 对新的根节点进行下滤，保证最小堆
        shift_down(0);
    }
}

// 从时间堆中寻找到时间的结点
void TimeHeap::tick() {
    HeapTimer* timer = m_timers[0];
    time_t cur = time(NULL);

    // 不断判断堆顶是否时间已到
    while (m_size > 0) {
        if (timer == nullptr) {
            break;
        }
        // 未到时间，则停止
        if (timer->expire > cur) {
            break;
        }
        if (m_timers[0]->callback != nullptr) {
            m_timers[0]->callback(m_timers[0]->user_data);
        }
        // 无论是否调用了callback，都会在调用结束后清除堆顶计时器
        pop_timer();
        timer = m_timers[0];
    }
}

// 空间不足时，将空间扩大为原来的2倍
void TimeHeap::reallocate() {
    m_capacity *= 2;
    HeapTimer** timers = new HeapTimer*[m_capacity];
    // 内存不够分配了，抛出异常
    if (timers == nullptr) {
        throw std::exception();
    }
    for (int i = 0; i < m_size; i++) {
        timers[i] = m_timers[i];
    }
    if (m_timers != nullptr) {
        delete[] m_timers;
    }
    m_timers = timers;
}
