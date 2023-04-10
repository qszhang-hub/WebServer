#ifndef HTTP_CONN_H
#define HTTP_CONN_H

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>

#include "locker.h"
#include "log.h"

class http_conn {
public:
    static int m_epollfd;     // 所有socket上的事件都被注册到同一个epoll中
    static int m_user_count;  // 静态成员变量，统计用户数量
    static const int READ_BUFFER_SIZE = 2048;   // 读缓冲区大小
    static const int WRITE_BUFFER_SIZE = 2048;  // 写缓冲区大小
    static const int FILENAME_LEN = 200;        // 文件名的最大长度

    bool m_et;     // 是否开启ET模式
    int m_sockfd;  // 该http连接的socket

    // 网站根目录
    const char *doc_root = "/home/echo/projects/cpp/WebServer/resources";

    // 定义HTTP响应的一些状态信息
    const char *ok_200_title = "OK";
    const char *error_400_title = "Bad Request";
    const char *error_400_form =
        "Your request has bad syntax or is inherently impossible to satisfy.\n";
    const char *error_403_title = "Forbidden";
    const char *error_403_form = "You do not have permission to get file from this server.\n";
    const char *error_404_title = "Not Found";
    const char *error_404_form = "The requested file was not found on this server.\n";
    const char *error_500_title = "Internal Error";
    const char *error_500_form = "There was an unusual problem serving the requested file.\n";

    // HTTP请求方法，这里只支持GET
    enum METHOD { GET = 0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT };

    /*
        解析客户端请求时，主状态机的状态
        CHECK_STATE_REQUESTLINE:当前正在分析请求行
        CHECK_STATE_HEADER:当前正在分析头部字段
        CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE { CHECK_STATE_REQUESTLINE = 0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT };

    // 从状态机的三种可能状态，即行的读取状态，分别表示
    // 1.读取到一个完整的行 2.行出错 3.行数据尚且不完整
    enum LINE_STATUS { LINE_OK = 0, LINE_BAD, LINE_OPEN };

    /*
        服务器处理HTTP请求的可能结果，报文解析的结果
        NO_REQUEST          :   请求不完整，需要继续读取客户数据
        GET_REQUEST         :   表示获得了一个完成的客户请求
        BAD_REQUEST         :   表示客户请求语法错误
        NO_RESOURCE         :   表示服务器没有资源
        FORBIDDEN_REQUEST   :   表示客户对资源没有足够的访问权限
        FILE_REQUEST        :   文件请求,获取文件成功
        INTERNAL_ERROR      :   表示服务器内部错误
        CLOSED_CONNECTION   :   表示客户端已经关闭连接了
    */
    enum HTTP_CODE {
        NO_REQUEST,
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };

    http_conn(){};
    ~http_conn(){};

    void init(int sockfd, const sockaddr_in &addr, bool et);  // 初始化新建立的连接
    void close_conn();                                        // 关闭连接
    bool read();                                              // 一次性读完（非阻塞）
    bool write();                                             // 一次性写完（非阻塞）
    void process();                                           // 处理客户端请求
    sockaddr_in *get_address() {                              // 获取IP地址
        return &m_address;
    }

private:
    sockaddr_in m_address;             // 通信的地址信息
    char m_readbuf[READ_BUFFER_SIZE];  // 读缓冲区
    int m_read_idx;        // 标识读缓冲区中读入的数据最后一个字节的下标
    int m_checked_idx;     // 当前正在分析的字符在读缓冲区的位置
    int m_start_line;      // 当前正在解析的行的起始位置
    char *m_url;           // 请求目标文件的文件名
    char *m_version;       // 请求目标文件的http协议版本，我们仅支持HTTP1.1
    METHOD m_method;       // 请求方法
    char *m_host;          // 主机名
    bool m_linger;         // 判断http请求是否要保持连接
                           // linger单词含义：萦绕，盘旋，逗留，拖延
    int m_content_length;  // 内容长度
    // 客户请求的目标文件的完整路径，其内容等于doc_root+m_url,doc_root是网站根目录
    char m_real_file[FILENAME_LEN];
    // 目标文件的状态。通过它我们可以判断文件是否存在、是否为目录、是否可读，并获取文件大小等信息
    struct stat m_file_stat;
    char *m_file_address;  // 客户请求的目标文件被mmap到内存中的起始位置
    char m_write_buf[WRITE_BUFFER_SIZE];  // 写缓冲区
    int m_write_idx;                      // 当前写缓冲区索引
                                          //
    // writev函数用于将多个分散的缓存区中的内容聚集在一起写入一个fd
    // 我们将采用writev来执行写操作，所以定义下面两个成员。
    // 为什么要聚集写？因为我们的写缓冲区只存了状态行、首行这些信息
    // 而客户请求的文件被映射到了内存中
    // 最后我们响应的应该是两个东西连续起来
    // 因此需要聚集写入
    struct iovec m_iv[2];
    // 表示被写内存块的数量，与上面的结构体一起使用
    int m_iv_count;

    int bytes_to_send;    // 要发送的字节数
    int bytes_have_send;  // 已经发送的字节数

    CHECK_STATE m_check_state;  // 主状态机当前所处的状态

    HTTP_CODE process_read();                  // 解析HTTP请求
    HTTP_CODE parse_request_line(char *text);  // 解析请求首行
    HTTP_CODE parse_headers(char *text);       // 解析请求头
    HTTP_CODE parse_content(char *text);       // 解析请求体

    LINE_STATUS parse_line();  // 解析一行
    HTTP_CODE do_request();    // 具体处理
    // 类体内直接生成函数体，则默认会设为内联函数，即使不加inline也是。
    // 返回读缓冲区指针后移
    char *get_line() {
        return m_readbuf + m_start_line;
    }
    void init();   // 初始化状态机
    void unmap();  // 取消文件的内存映射

    bool process_write(HTTP_CODE ret);                    // 填充HTTP应答
    bool add_response(const char *format, ...);           // 写入一行响应
    bool add_content(const char *content);                // 写入内容
    bool add_content_type();                              // 写入内容类型
    bool add_status_line(int status, const char *title);  // 写入状态行
    bool add_headers(int content_length);                 // 写入首行
    bool add_content_length(int content_length);          // 写入内容长度
    bool add_linger();                                    // 写入connection是否keepalive
    bool add_blank_line();                                // 添加空行
};

#endif
