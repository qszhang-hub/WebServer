#include "http_conn.h"

int http_conn::m_epollfd = -1;    // 所有socket上的事件都被注册到同一个epoll中
int http_conn::m_user_count = 0;  // 统计用户数量

// 设置文件描述符非阻塞
int setnonblocking(int fd) {
    int flag = fcntl(fd, F_GETFL);
    flag |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, flag);
}

// 添加文件描述符到epoll中
void addfd(int epollfd, int fd, bool oneshot, bool et) {
    epoll_event event;
    event.data.fd = fd;

    event.events = EPOLLIN | EPOLLRDHUP;  // 水平触发模式 LT

    if (et) {
        event.events |= EPOLLET;
    }

    // 即使设置了边沿触发 ET，也会出现一个socket上的事件多次触发。
    // 比如，本来读完了数据开始处理，但是又有数据发来，这会导致又一个线程被唤醒来处理，
    // 导致两个线程处理同一socket。
    // 为了解决这个问题，可以设置EPOLLONESHOT
    // 一言一概之：保证同一个socket只能被一个线程处理，不会跨越多个线程
    if (oneshot) {
        event.events |= EPOLLONESHOT;
    }

    // 向epoll注册该事件
    epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);

    // 设置文件描述符非阻塞
    setnonblocking(fd);
}

// 从epoll中删除文件描述符
void removefd(int epollfd, int fd) {
    epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, NULL);
    close(fd);
}

// 修改文件描述符 重置EPOLLONESHOT，确保下一次能触发
void modfd(int epollfd, int fd, int ev, int et) {
    epoll_event event;
    event.data.fd = fd;
    event.events = ev | EPOLLONESHOT | EPOLLRDHUP;

    if (et) {
        event.events |= EPOLLET;
    }
    epoll_ctl(epollfd, EPOLL_CTL_MOD, fd, &event);
}

// 初始化新建立的连接
void http_conn::init(int sockfd, const sockaddr_in &addr, bool et, util_timer *timer) {
    m_sockfd = sockfd;
    m_address = addr;
    m_et = et;
    m_timer = timer;

    // 设置端口复用
    int reuse = 1;
    int ret = setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (ret == -1) {
        perror("setsockopt reuseport");
        exit(-1);
    }

    // 添加到epoll对象中
    addfd(m_epollfd, sockfd, true, true);
    m_user_count++;
    init();
}

// 初始化状态机
void http_conn::init() {
    m_check_state = CHECK_STATE_REQUESTLINE;  // 初始化主状态机状态为解析请求首行
    m_checked_idx = 0;                        // 当前解析到的读缓冲区索引
    m_start_line = 0;                         // 当前正在解析的行的起始位置
    m_url = 0;                                // url初始置空
    m_method = GET;                           // 方法初始为GET
    m_version = 0;                            // httpversion初始为0
    m_host = 0;                               // 主机名
    m_linger = false;                         // http是否保持连接
    m_content_length = 0;                     // 内容长度为0
    m_write_idx = 0;
    m_read_idx = 0;

    bytes_have_send = 0;
    bytes_to_send = 0;

    bzero(m_readbuf, READ_BUFFER_SIZE);  // 置空readbuf
    bzero(m_write_buf, READ_BUFFER_SIZE);
    bzero(m_real_file, FILENAME_LEN);
}
// 关闭连接
void http_conn::close_conn() {
    if (m_sockfd != -1) {
        removefd(m_epollfd, m_sockfd);
        m_sockfd = -1;
        m_user_count--;
    }
}

// 一次性读完（非阻塞）
// 循环读取数据，直到无数据可读或对方关闭连接
bool http_conn::read() {
    // 缓冲已满
    if (m_read_idx >= READ_BUFFER_SIZE) {
        return false;
    }

    // 已经读取到的字节
    int bytes = 0;

    if (m_et) {
        // ET模式下，必须要把数据一次读完
        while (true) {
            bytes = recv(m_sockfd, m_readbuf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
            if (bytes == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // 没有数据
                    break;
                }
                return false;
            } else if (bytes == 0) {
                // 对方关闭连接
                return false;
            }
            m_read_idx += bytes;  // 索引后移
        }
    } else {
        // LT模式下，只用读一次即可
        bytes = recv(m_sockfd, m_readbuf + m_read_idx, READ_BUFFER_SIZE - m_read_idx, 0);
        m_read_idx += bytes;

        if (bytes <= 0) {
            return false;
        }
    }
    return true;
}

// 解析一行，根据\r\n判断
http_conn::LINE_STATUS http_conn::parse_line() {
    // m_read_idx表示的是读缓冲区中读入的最后一个字符下标
    // m_checked_idx表示的是当前正在解析的字符的下标

    // 当前字符
    char ch;

    // 一个字符一个字符地遍历，直到读缓冲区中最后一个字符
    for (; m_checked_idx < m_read_idx; ++m_checked_idx) {
        // 获取当前字符
        ch = m_readbuf[m_checked_idx];

        // 如果是\r，判断下一个是不是\n
        if (ch == '\r') {
            // m_checked_idx已经是读入的最后一个字符了
            if (m_checked_idx + 1 == m_read_idx) {
                return LINE_OPEN;  // 行数据还不完整
            } else if (m_readbuf[m_checked_idx + 1] == '\n') {
                m_readbuf[m_checked_idx++] = '\0';  // \r变成字符串分隔符
                m_readbuf[m_checked_idx++] = '\0';  // \n变成字符串分隔符
                return LINE_OK;                     // 行解析成功
            }
            return LINE_BAD;  // 只有\r说明行出错
        }
        // 这种情况出现说明是上面行数据不完整，之前可能解析到了\r，但是还没碰到\n
        // 而这一次解析碰到了\n
        else if (ch == '\n')  // 如果是\n，判断上一个是不是\r
        {
            // m_checked_idx > 0 不可以
            // 即使0号是'\r',1号是'\n'，前面也没数据，不能解析
            if (m_checked_idx > 1 && m_readbuf[m_checked_idx - 1] == '\r') {
                m_readbuf[m_checked_idx - 1] = '\0';  // \r变成字符串分隔符
                m_readbuf[m_checked_idx++] = '\0';    // \n变成字符串分隔符
                return LINE_OK;                       // 解析成功
            }
            return LINE_BAD;  // 只有\n，解析失败
        }
    }
    return LINE_OPEN;  // for循环已经遍历完了，还没有碰到\r和\n时，说明内容还不完整
}

// 解析HTTP请求  主状态机
http_conn::HTTP_CODE http_conn::process_read() {
    // 初始状态
    // line_status表示的是当前行解析状态
    // m_check_state表示的是当前主从状态机解析的状态
    LINE_STATUS line_status = LINE_OK;  // 行解析正常
    HTTP_CODE ret = NO_REQUEST;         // 请求不完整

    char *text = 0;

    // 解析到了一行完整的数据，或者解析到了请求体且数据完整
    while ((m_check_state == CHECK_STATE_CONTENT && line_status == LINE_OK) ||
           ((line_status = parse_line()) == LINE_OK)) {
        // 获取一行数据
        // 该函数不过是返回了该行数据的首下标而已
        // 由于我们在解析一行时已经将后面的\r和\n都处理为'\0'，故可以直接输出。
        text = get_line();

        // 解析了一行之后，接下来新的行首下标就从当前下标开始了
        m_start_line = m_checked_idx;

        // std::cout << "got 1 http line: " << text << std::endl;
        LOG_INFO("%s", text);
        Log::get_instance()->flush();

        // 根据当前状态进行转移
        switch (m_check_state) {
            // 正在分析请求行
            case CHECK_STATE_REQUESTLINE: {
                ret = parse_request_line(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                }

                // 跳出switch，继续下一行解析
                break;
            }
            // 正在分析头部
            case CHECK_STATE_HEADER: {
                ret = parse_headers(text);
                if (ret == BAD_REQUEST) {
                    return BAD_REQUEST;
                } else if (ret == GET_REQUEST) {
                    // 已经得到了完整头部，分析目标文件属性
                    return do_request();
                }

                // 跳出switch，继续下一行解析
                break;
            }
            // 正在分析内容
            case CHECK_STATE_CONTENT: {
                ret = parse_content(text);
                if (ret == GET_REQUEST) {
                    // 已经成功解析内容，分析目标文件属性
                    return do_request();
                }
                line_status = LINE_OPEN;

                // 跳出switch，继续下一行解析
                break;
            }
            default: return INTERNAL_ERROR;  // 除开这三种情况，服务器内部错误
        }
    }
    return NO_REQUEST;
}

// 解析请求首行，获取请求方法，目标URL，HTTP版本
http_conn::HTTP_CODE http_conn::parse_request_line(char *text) {
    // 解析出GET字段
    // GET\t/index.html HTTP/1.1
    // strpbrk(s1, s2);  在s1中查找s2第一次出现的位置
    m_url = strpbrk(text, " \t");  // 第一次出现\t是在GET后面
    // 找不到
    if (!m_url) {
        return BAD_REQUEST;
    }
    // 把\t变成字符串分隔符  此时m_url++后，m_url指向/
    *m_url++ = '\0';
    // GET\0/index.html HTTP/1.1

    char *method = text;  // 再从头开始获得字符串，GET就被取出来了
    // strcasecmp(s1, s2); 不区分大小写比较s1和s2的字典顺序大小，返回0表示相同
    // 确实是GET字段
    if (strcasecmp(method, "GET") == 0) {
        m_method = GET;
    } else {
        // 否则出错
        return BAD_REQUEST;
    }

    // 解析出版本
    // m_version指向'/index.html HTTP/1.1'中的间隔符
    m_version = strpbrk(m_url, " \t");
    // 此时的请求行：/index.html HTTP/1.1

    // 如果找不到间隔符，说明格式错了
    if (!m_version) {
        return BAD_REQUEST;
    }
    // 将间隔符变成字符串分隔符
    *m_version++ = '\0';
    // m_version指向HTTP中的H

    if (strcasecmp(m_version, "HTTP/1.1") != 0) {
        return BAD_REQUEST;  // 语法错误
    }

    // 这里，m_url指向/index.html
    // 有些网页会出现 http://172.25.29.103/index.html
    // 考虑这种情况
    // strncasecmp(s1, s2, n) 只对比s1和s2的前n个字符，看是否相等
    if (strncasecmp(m_url, "http://", 7) == 0) {  // 相等，属于这种情况的，就让m_url后移7位，
        m_url += 7;
        // strchr(s1, ch);用于查找s1中第一次出现字符ch的位置
        // 这里让m_url等于第一次出现/的位置，即跳过了http://...这一部分，直接到/index.html这一部分
        // 跳过了IP地址
        m_url = strchr(m_url, '/');
    }

    // 这里必然是/index.html，如果m_url没有找到'/'，或m_url的第一个值不是/，说明语法有误
    if (!m_url || m_url[0] != '/') {
        return BAD_REQUEST;
    }

    // 接下来解析头部
    // 有限状态机跳转状态为：解析头部
    m_check_state = CHECK_STATE_HEADER;

    // 请求不完整，即还没解析完整个请求，仅仅解析到了头部
    return NO_REQUEST;
}

// 解析请求头
http_conn::HTTP_CODE http_conn::parse_headers(char *text) {
    // 遇到空行，表示头部字段解析完毕
    // 因为请求头部跟请求内容中间有个\r\n间隔
    if (text[0] == '\0') {
        // 如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体，
        // 状态机转移到CHECK_STATE_CONTENT状态
        if (m_content_length != 0) {
            m_check_state = CHECK_STATE_CONTENT;
            return NO_REQUEST;  // 请求不完整
        }
        // 否则说明我们已经得到了一个完整的HTTP请求
        return GET_REQUEST;
    }
    // 处理Connection字段
    // Connection: keep-alive
    // strncasecmp(s1, s2, n); 不区分大小写比较s1和s2前n个字符的字典顺序大小，返回0表示相同
    else if (strncasecmp(text, "Connection:", 11) == 0) {
        text += 11;  // 现在text指向中间的间隔符
        // strspn(s1, s2) 检索字符串 s1 中第一个不在字符串s2中出现的字符下标。
        text += strspn(text, " \t");  // 相当于skip掉间隔符
        // keep-alive说明保持连接
        if (strcasecmp(text, "keep-alive") == 0) {
            m_linger = true;
        }
    }
    // 处理Content-Length头部字段
    else if (strncasecmp(text, "Content-Length:", 15) == 0) {
        text += 15;
        text += strspn(text, " \t");
        m_content_length = atol(text);  // 读取内容长度字段
    }
    // 处理Host头部字段
    else if (strncasecmp(text, "Host:", 5) == 0) {
        text += 5;
        text += strspn(text, " \t");
        m_host = text;  // 读取host字段
    }
    // 除了之前的字段，其余都视为头部解析出错
    else {
        // printf("oop! unknow header %s\n", text);
        LOG_INFO("oop!unknow header: %s", text);
        Log::get_instance()->flush();
    }
    return NO_REQUEST;  // 请求不完整
}

// 解析请求体 我们没有真正解析HTTP请求的消息体，只是判断它是否被完整的读入了
// 甚至我们都没有去读内容体这几行，为了节省效率
// 何以见得？看上面主从状态机while循环，先判断是否解析内容体，再判断：解析一行是否正确
// 故刚解析完头部，状态转为解析内容体后，就没有进入解析一行，就直接跳到这里了
http_conn::HTTP_CODE http_conn::parse_content(char *text) {
    // 如果当前处理的下标+内容长度没有比缓冲区当前下标大，说明有内容体已经读入，请求完整
    // 这里的当前处理的下标是刚处理完请求头部后的下标
    if ((m_checked_idx + m_content_length) <= m_read_idx) {
        // 当前text正好是上次m_checked_idx，因此下标直接写入m_content_length就是内容体的最后
        text[m_content_length] = '\0';
        return GET_REQUEST;  // 请求完整
    }
    return NO_REQUEST;  // 请求不完整
}

// 分析目标文件属性，并对本地文件创建内存映射
http_conn::HTTP_CODE http_conn::do_request() {
    // 把根目录拷贝到m_real_file中
    strcpy(m_real_file, doc_root);

    // 将m_url拼接到dock_root后面
    int len = strlen(doc_root);
    // FILENAME_LEN - len - 1限制了url长度不能超过文件名最大长度
    strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);
    // 通过stat函数将文件属性获取到m_file_stat中
    if (stat(m_real_file, &m_file_stat) == -1) {
        // 获取失败
        return NO_RESOURCE;
    }

    // 判断访问权限  是否有其他人读取权限（read by others）
    if (!(m_file_stat.st_mode & S_IROTH)) {
        return FORBIDDEN_REQUEST;  // 禁止访问
    }

    // 判断是否是目录
    if (S_ISDIR(m_file_stat.st_mode)) {
        return BAD_REQUEST;  // 是目录，不给返回
    }

    // 以只读方式打开文件
    int fd = open(m_real_file, O_RDONLY);
    // 创建内存映射
    m_file_address = (char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

// 对内存映射区执行munmap操作
void http_conn::unmap() {
    if (m_file_address) {
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address = 0;
    }
}

// 写HTTP响应体
bool http_conn::write() {
    int len = 0;
    if (bytes_to_send == 0) {
        // 将要发送的字节为0，这一次响应结束。
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_et);  // 监听输入事件
        init();
        return true;
    }

    while (1) {
        // 聚集写
        // 先从写缓冲区中写入sockfd，再从文件中写入sockfd
        // writev函数用于将多个分散的缓存区中的内容聚集在一起写入一个fd，返回值是此次写入的数据大小
        len = writev(m_sockfd, m_iv, m_iv_count);
        // -1表示写入出错
        if (len == -1) {
            // 如果TCP写缓冲没有空间，即sockfd写入空间不足，则等待下一轮EPOLLOUT事件，
            // 虽然在此期间，服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if (errno == EAGAIN) {
                // 继续监听输出事件
                modfd(m_epollfd, m_sockfd, EPOLLOUT, m_et);
                return true;
            }
            // 不是空间不足造成的，那么说明是调用出错了，取消内存映射
            unmap();
            return false;
        }

        // 要发送的数据长度减去这次发送的
        bytes_to_send -= len;

        // 已经发送的数据长度加上这次发送的
        bytes_have_send += len;

        // 如果0号写入区，即写缓冲区已经读满，开始从1号读入
        if (bytes_have_send >= m_iv[0].iov_len) {
            m_iv[0].iov_len = 0;  // len置空
            // 文件起始位置就是已经发送的所有字节数减去写缓冲区发送的所有字节数剩余部分+m_file_address
            m_iv[1].iov_base = m_file_address + (bytes_have_send - m_write_idx);
            m_iv[1].iov_len = bytes_to_send;
        } else {
            // 0号还没读满，修改下一次读数据的位置
            // 0号文件起始地址增加
            m_iv[0].iov_base = m_write_buf + bytes_have_send;
            // 长度相应减少
            m_iv[0].iov_len = m_iv[0].iov_len - bytes_have_send;
        }
        // 没有数据要发送了
        if (bytes_to_send <= 0) {
            // 解除映射
            unmap();
            // 检测读入
            modfd(m_epollfd, m_sockfd, EPOLLIN, m_et);
            // 若保持连接，就再初始化
            if (m_linger) {
                init();
                return true;
            } else {
                return false;
            }
        }
    }
}

// 往写缓冲中写入待发送的数据 类似printf函数
bool http_conn::add_response(const char *format, ...) {
    if (m_write_idx >= WRITE_BUFFER_SIZE)  // 写缓冲已经满了
    {
        return false;
    }

    va_list arg_list;            // va_list生成一个指针，用于指向可选参数
    va_start(arg_list, format);  // 将指针指向我们的可选参数

    // 写缓冲区剩余可写入容量
    int remain_size = WRITE_BUFFER_SIZE - 1 - m_write_idx;
    // vsnprintf()将格式化数据从可变参数列表写入大小
    // vsnprintf(index, size, format, ...)，后面的format和...可以理解为就是一个printf
    int len = vsnprintf(m_write_buf + m_write_idx, remain_size, format, arg_list);
    if (len >= remain_size)  // 超出写缓冲范围
    {
        return false;
    }
    m_write_idx += len;

    va_end(arg_list);  // 结束可变参数

    LOG_INFO("request:%s", m_write_buf);
    Log::get_instance()->flush();
    return true;
}

// 添加状态行 参数：状态，标题
bool http_conn::add_status_line(int status, const char *title) {
    return add_response("%s %d %s\r\n", "HTTP/1.1", status, title);
}

bool http_conn::add_headers(int content_len) {
    add_content_length(content_len);
    add_content_type();
    add_linger();
    add_blank_line();
    return true;
}

bool http_conn::add_content_length(int content_len) {
    return add_response("Content-Length: %d\r\n", content_len);
}

bool http_conn::add_linger() {
    return add_response("Connection: %s\r\n", (m_linger == true) ? "keep-alive" : "close");
}

bool http_conn::add_blank_line() {
    return add_response("%s", "\r\n");
}

bool http_conn::add_content(const char *content) {
    return add_response("%s", content);
}

bool http_conn::add_content_type() {
    return add_response("Content-Type:%s\r\n", "text/html");
}

// 根据服务器处理HTTP请求的结果，决定返回给客户端的内容
bool http_conn::process_write(HTTP_CODE read_ret) {
    switch (read_ret) {
        case INTERNAL_ERROR:  // 服务器内部错误
        {
            add_status_line(500, error_500_title);
            add_headers(strlen(error_500_form));
            if (!add_content(error_500_form)) {
                return false;
            }
            break;
        }
        case BAD_REQUEST: {  // 客户请求语法错误
            add_status_line(400, error_400_title);
            add_headers(strlen(error_400_form));
            if (!add_content(error_400_form)) {
                return false;
            }
            break;
        }
        case NO_RESOURCE: {  // 服务器没有资源
            add_status_line(404, error_404_title);
            add_headers(strlen(error_404_form));
            if (!add_content(error_404_form)) {
                return false;
            }
            break;
        }
        case FORBIDDEN_REQUEST: {  // 客户对资源没有足够的访问权限
            add_status_line(403, error_403_title);
            add_headers(strlen(error_403_form));
            if (!add_content(error_403_form)) {
                return false;
            }
            break;
        }
        case FILE_REQUEST: {  // 获取文件成功
            add_status_line(200, ok_200_title);
            // 如果文件大小不为空
            if (m_file_stat.st_size != 0) {
                add_headers(m_file_stat.st_size);

                // 添加首行后不用添加内容，内容由我们写入

                // 分别填写两个缓冲区的起始位置和长度

                // 0号存放写缓冲区和长度
                m_iv[0].iov_base = m_write_buf;
                // 由于之前向写缓冲写入了状态行和首部，因此当前的写缓冲区索引就是长度
                m_iv[0].iov_len = m_write_idx;
                // 1号存放客户请求的目标文件被mmap到内存中的起始位置和长度
                m_iv[1].iov_base = m_file_address;
                m_iv[1].iov_len = m_file_stat.st_size;

                // 缓冲区个数为2
                m_iv_count = 2;

                // 要发送的字节数=响应头部大小+文件大小
                bytes_to_send = m_write_idx + m_file_stat.st_size;
                return true;
            } else {
                // 若为空，就生成一个空html
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
        }
        default: return false;
    }

    // 除了文件成功获取外，其他情况写入了写缓冲，这里也要反馈
    // 只写入写缓冲区
    m_iv[0].iov_base = m_write_buf;
    m_iv[0].iov_len = m_write_idx;
    m_iv_count = 1;

    // 要发送的字节数=响应头部大小
    bytes_to_send = m_write_idx;
    return true;
}

// 由线程池中的工作线程调用，处理http请求的入口函数
// 每个工作线程负责解析请求并生成响应
void http_conn::process() {
    // 解析http请求
    HTTP_CODE read_ret = process_read();
    if (read_ret == NO_REQUEST) {
        // 修改事件为读事件
        modfd(m_epollfd, m_sockfd, EPOLLIN, m_et);
        return;
    }

    // 生成http响应
    bool write_ret = process_write(read_ret);
    if (!write_ret) {
        close_conn();
    }
    // 修改事件为写事件
    modfd(m_epollfd, m_sockfd, EPOLLOUT, m_et);
}
