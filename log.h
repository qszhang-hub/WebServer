#ifndef LOG_H
#define LOG_H
#include <string.h>
#include <unistd.h>

#include <iostream>

#include "block_queue.h"
#include "locker.h"

// LOG级别
enum LOGLEVEL {
    LOG_LEVEL_ERROR,    // error
    LOG_LEVEL_WARNING,  // warning
    LOG_LEVEL_DEBUG,    // debug
    LOG_LEVEL_INFO,     // info
};
// log输出位置
// enum LOGTARGET
// {
//     LOG_TARGET_NONE = 0x00,
//     LOG_TARGET_CONSOLE = 0x01, // 控制台输出
//     LOG_TARGET_FILE = 0x10     // 文件输出
// };
// 基于单例模式实现LOG类
class Log {
public:
    static Log *get_instance() {
        // 懒汉模式双重检测锁
        if (m_log == nullptr) {
            m_lock.lock();
            if (m_log == nullptr) {
                m_log = new Log();
            }
            m_lock.unlock();
        }
        return m_log;
    };
    // 初始化日志系统，文件名，日志缓存区大小，最大行数，缓存队列大小（异步用到）
    bool init(const char *file_name, int log_buf_size = 8192, int split_lines = 5000000,
              int max_queue_size = 0);
    void write_log(LOGLEVEL level, const char *format, ...);  // 同步写日志
    void flush(void);  // 刷新缓冲区，让日志写入立刻生效
    // 异步写日志的回调函数
    static void *async_log_worker(void *args) {
        Log::get_instance()->async_write_log();
        return nullptr;
    }

private:
    // 私有化构造函数、析构函数、拷贝构造函数、赋值运算符，防止产生多例
    Log();
    // Log(const Log &){};
    ~Log();
    // const Log &operator=(const Log &){};
    // 异步写入
    void *async_write_log();

private:
    static Log *m_log;     // 唯一实例
    LOGLEVEL m_log_level;  // log级别
    // LOGTARGET m_log_target;           // log输出位置
    static locker m_lock;              // 互斥锁
    long long m_lines;                 // 日志行数
    int m_max_lines;                   // 日志最大行数
    char *m_buf;                       // 日志缓冲区
    int m_log_buf_size;                // 日志缓冲区大小
    bool m_is_async;                   // 是否异步
    FILE *m_fp;                        // 打开log的文件指针
    block_queue<string> *m_log_queue;  // 阻塞队列
    int m_today;                       // 因为按天分类,记录当前时间是那一天
    char dir_name[128];                // 路径名（目录）
    char log_name[64];                 // log文件名
};

// 定义宏，用于快速写入log
#define LOG_DEBUG(format, ...) \
    Log::get_instance()->write_log(LOG_LEVEL_DEBUG, format, ##__VA_ARGS__)
#define LOG_INFO(format, ...) Log::get_instance()->write_log(LOG_LEVEL_INFO, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) \
    Log::get_instance()->write_log(LOG_LEVEL_WARNING, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) \
    Log::get_instance()->write_log(LOG_LEVEL_ERROR, format, ##__VA_ARGS__)

#endif
