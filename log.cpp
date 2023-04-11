#include "log.h"

#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
using namespace std;

Log *Log::m_log = nullptr;
locker Log::m_lock;

Log::Log() {
    m_lines = 0;
    m_is_async = false;
}

Log::~Log() {
    m_lock.lock();
    if (m_log != nullptr) {
        delete m_log;
    }
    if (m_fp != NULL) {
        fclose(m_fp);
    }
    if (m_log_queue != NULL) {
        delete m_log_queue;
    }
    if (m_buf != nullptr) {
        delete m_buf;
    }
    m_lock.unlock();
}
// 异步需要设置阻塞队列的长度，同步不需要设置
bool Log::init(const char *file_name, int log_buf_size, int max_lines, int max_queue_size) {
    // 如果设置了max_queue_size,则设置为异步
    if (max_queue_size >= 1) {
        m_is_async = true;
        m_log_queue = new block_queue<string>(max_queue_size);
        pthread_t tid;
        // async_log_worker为回调函数,这里表示创建线程异步写日志
        pthread_create(&tid, NULL, async_log_worker, NULL);
    }

    m_log_buf_size = log_buf_size;
    m_buf = new char[m_log_buf_size];
    memset(m_buf, '\0', m_log_buf_size);
    m_max_lines = max_lines;

    // 获取系统当前时间
    time_t t = time(NULL);
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;

    // 去掉工作目录，只保留文件名
    // strrchr(s1, ch)函数在s1中查找字符ch最后一次出现的位置
    const char *p = strrchr(file_name, '/');
    // log文件全名
    char log_full_name[256] = {0};

    if (p == NULL) {
        // log_full_name: "2023_03_15_file_name"
        strcpy(log_name, file_name);
        snprintf(log_full_name, 255, "%d_%02d_%02d_%s", my_tm.tm_year + 1900, my_tm.tm_mon + 1,
                 my_tm.tm_mday, file_name);
    } else {
        // '/'后面是log文件名称
        strcpy(log_name, p + 1);
        // '/'前面是目录名称
        strncpy(dir_name, file_name, p - file_name + 1);
        // log_full_name: "dir_name2023_03_15_log_name"
        snprintf(log_full_name, 255, "%s%d_%02d_%02d_%s", dir_name, my_tm.tm_year + 1900,
                 my_tm.tm_mon + 1, my_tm.tm_mday, log_name);
    }

    m_today = my_tm.tm_mday;

    // 打开日志文件
    m_fp = fopen(log_full_name, "a");
    if (m_fp == NULL) {
        return false;
    }

    return true;
}

void Log::write_log(LOGLEVEL level, const char *format, ...) {
    // 获取当前时间
    struct timeval now = {0, 0};
    gettimeofday(&now, nullptr);
    time_t t = now.tv_sec;
    struct tm *sys_tm = localtime(&t);
    struct tm my_tm = *sys_tm;
    // 日志等级
    char s[16] = {0};
    switch (level) {
        case LOG_LEVEL_DEBUG: strcpy(s, "[debug]:"); break;
        case LOG_LEVEL_INFO: strcpy(s, "[info]:"); break;
        case LOG_LEVEL_WARNING: strcpy(s, "[warn]:"); break;
        case LOG_LEVEL_ERROR: strcpy(s, "[erro]:"); break;
        default: strcpy(s, "[none]:"); break;
    }
    // 写入一个log，行数+1
    m_lock.lock();
    m_lines++;

    // 已经过了一天了，或行数已满,要创建新的日志
    if (m_today != my_tm.tm_mday || m_lines % m_max_lines == 0) {
        char new_log[256] = {0};
        fflush(m_fp);
        fclose(m_fp);
        char tail[16] = {0};

        // tail: "2023_03_15_"
        snprintf(tail, 16, "%d_%02d_%02d_", my_tm.tm_year + 1900, my_tm.tm_mon + 1, my_tm.tm_mday);
        // 新的一天，开启新的日志
        if (m_today != my_tm.tm_mday) {
            // new_log: "dir_name2023_03_15log_name"
            snprintf(new_log, 255, "%s%s%s", dir_name, tail, log_name);
            m_today = my_tm.tm_mday;
            m_lines = 0;
        }
        // 今天的日志已满
        else {
            // new_log: "dir_name2023_03_15log_name_1"
            // 其中.1是代表今天的日志文件下标
            snprintf(new_log, 255, "%s%s%s_%lld", dir_name, tail, log_name, m_lines / m_max_lines);
        }
        m_fp = fopen(new_log, "a");
    }

    m_lock.unlock();

    va_list valst;
    va_start(valst, format);

    string log_str;
    m_lock.lock();

    // 写入的具体时间内容格式
    // m_buf: "2023-03-15 12:47:03.μs [debug]:"
    int n = snprintf(m_buf, 48, "%d-%02d-%02d %02d:%02d:%02d.%06ld %s ", my_tm.tm_year + 1900,
                     my_tm.tm_mon + 1, my_tm.tm_mday, my_tm.tm_hour, my_tm.tm_min, my_tm.tm_sec,
                     now.tv_usec, s);
    // m_buf: "2023-03-15 12:47:03.μs [debug]:close fd"
    int m = vsnprintf(m_buf + n, m_log_buf_size - 1, format, valst);

    // 设置换行符和字符串终止符
    m_buf[n + m] = '\n';
    m_buf[n + m + 1] = '\0';

    log_str = m_buf;

    m_lock.unlock();

    // 如果是异步且阻塞队列未满，则放入阻塞队列
    if (m_is_async && !m_log_queue->full()) {
        m_log_queue->push(log_str);
    } else {
        // 同步，或者异步但是阻塞队列已满，直接写入log文件
        m_lock.lock();
        fputs(log_str.c_str(), m_fp);
        m_lock.unlock();
    }
    va_end(valst);
}

void Log::flush(void) {
    m_lock.lock();
    // 强制刷新写入流缓冲区
    fflush(m_fp);
    m_lock.unlock();
}

void *Log::async_write_log() {
    string single_log;
    // 从阻塞队列中取出一个日志string，写入文件
    while (m_log_queue->pop(single_log)) {
        m_lock.lock();
        fputs(single_log.c_str(), m_fp);
        m_lock.unlock();
    }
    return nullptr;
}
