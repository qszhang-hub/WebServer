#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/epoll.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>

#include "http_conn.h"
#include "locker.h"
#include "log.h"
#include "lst_timer.h"
#include "threadpool.h"

#define MAX_FD 65535         // 最大的文件描述符个数
#define MAX_EVENT_NUM 10000  // 一次监听最大的事件数量
#define TIMESLOT 5           // 定时间隔5s

static int pipefd[2];  // 用于主线程与子线程之间的管道通信
static sort_timer_lst timer_lst;
static int epollfd = 0;

// 添加文件描述符到epoll中
extern void addfd(int epollfd, int fd, bool oneshot, bool et);
// 从epoll中删除文件描述符
extern void removefd(int epollfd, int fd);
// 设置非阻塞
extern int setnonblocking(int fd);

// 将收到的信号发送到管道写入端，并保留原来的errno
void sig_handler(int sig) {
    int save_errno = errno;
    send(pipefd[1], (char *)&sig, 1, 0);
    errno = save_errno;
}

// 添加信号捕捉
void addsig(int sig, void(handler)(int)) {
    struct sigaction sa;
    bzero(&sa, sizeof(sa));
    sa.sa_handler = handler;
    sigfillset(&sa.sa_mask);  // 对所有信号集都阻塞 即不考虑其他信号
    assert(sigaction(sig, &sa, NULL) != -1);
}

// 定时处理任务，实际上就是调用tick()函数
void timer_handler() {
    // tick函数从链表中找到那些到期的timer，并进行callback处理
    timer_lst.tick();
    // 因为一次 alarm 调用只会引起一次SIGALARM
    // 信号，所以我们要重新定时，以不断触发 SIGALARM信号。
    alarm(TIMESLOT);
}

// 定时器回调函数，该函数就是httpconn类中的close_conn()函数
void close_conn_call(http_conn *user) {
    LOG_INFO("time out. close fd: %d", user->m_sockfd);
    Log::get_instance()->flush();
    user->close_conn();
}

int main(int argc, char *argv[]) {
    // basename() 将文件路径中所有的前缀目录都删去，只保留最后的文件名
    // 如：/home/root/hello.txt  ->  hello.txt
    if (argc <= 2) {
        std::cout << "请按照如下格式运行：" << basename(argv[0]) << " port_number ET\n";
        std::cout << "其中ET代表是否开启EPOLL的边沿触发，可选1(开启)或0(不开启)\n";
        exit(-1);
    }
    // 初始化日志
    // 由于CMake项目将server生成在build目录下，如果直接写日志会将日志写到build下
    // 这里手动将日志生成在main函数同级目录下
    Log::get_instance()->init("Server.log", 2000, 800000, 0);  // 同步日志模型
    // Log::get_instance()->init("../Server.log", 2000, 800000, 10); // 异步日志模型

    // 获取端口号
    int port = atoi(argv[1]);

    // 获取EPOLL模式
    bool et = atoi(argv[2]) ? true : false;

    cout << "端口号: " << port << ", EPOLL模式: " << (et ? "ET" : "LT") << endl;
    // 对SIGPIPE信号进行处理  忽略它
    // 这是因为，对一个已经关闭了的socket进行写入时，内核就会发出SIGPIPE信号，终止程序
    // 我们不希望程序异常终止，故忽略它
    addsig(SIGPIPE, SIG_IGN);

    // 创建线程池
    threadpool<http_conn> *pool = NULL;
    try {
        pool = new threadpool<http_conn>;
    } catch (...) { exit(-1); }

    // 创建数组保存连接客户的信息
    http_conn *users = new http_conn[MAX_FD];

    // 创建socket
    int lfd = socket(PF_INET, SOCK_STREAM, 0);
    if (lfd == -1) {
        perror("socket");
        exit(-1);
    }

    // 设置端口复用
    // 为什么？如果不设置，服务器关闭后，再重启，之前绑定的端口号还未释放，或程序突然退出而系统未释放端口号，
    // 会导致绑定失败，提示ADDR正在使用中。
    int reuse = 1;
    int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    if (ret == -1) {
        perror("setsockopt reuseport");
        exit(-1);
    }

    // 绑定
    struct sockaddr_in saddr;
    // inet_pton(AF_INET, "172.25.29.103", &saddr.sin_addr.s_addr);
    saddr.sin_addr.s_addr = INADDR_ANY;
    saddr.sin_family = AF_INET;
    saddr.sin_port = htons(port);
    ret = bind(lfd, (struct sockaddr *)&saddr, sizeof(saddr));
    if (ret == -1) {
        perror("bind");
        exit(-1);
    }

    // 监听
    ret = listen(lfd, 5);
    if (ret == -1) {
        perror("listen");
        exit(-1);
    }

    // epoll注册
    epoll_event events[MAX_EVENT_NUM];
    epollfd = epoll_create(5);  // 自一次Linux内核改动后，参数size就被忽略了，只需要比0大即可

    // 将监听的文件描述符添加到epoll中
    addfd(epollfd, lfd, false, et);
    http_conn::m_epollfd = epollfd;

    // 创建管道 socketpair创建的管道是全双工的
    /*
    socketpair()函数用于创建一对无名的、相互连接的套接字。
    如果函数成功，则返回0，创建好的套接字分别是pipefd[0]和pipefd[1]；否则返回-1，错误码保存于errno中。
    基本用法：
    1.
    这对套接字可以用于全双工通信，每一个套接字既可以读也可以写。例如，可以往sv[0]中写，从sv[1]中读；
       或者从sv[1]中写，从sv[0]中读；
    2.
    如果往一个套接字(如sv[0])中写入后，再从该套接字读时会阻塞，只能在另一个套接字中(sv[1])上读成功；
    3.
    读、写操作可以位于同一个进程，也可以分别位于不同的进程，如父子进程。如果是父子进程时，一般会功能分离，
       一个进程用来读，一个用来写。因为文件描述副pipefd[0]和pipefd[1]是进程共享的，所以读的进程要关闭写描述符,
       反之，写的进程关闭读描述符。
    */
    ret = socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    if (ret == -1) {
        perror("socketpair");
        exit(-1);
    }
    setnonblocking(pipefd[1]);             // 设置写端非阻塞
    addfd(epollfd, pipefd[0], false, et);  // 监听读端

    // 设置信号处理函数
    addsig(SIGALRM, sig_handler);  // 当SIGALRM信号到来，就向管道写入端写入SIGALRM
    // SIGTERM信号只能由kill调用产生
    addsig(SIGTERM, sig_handler);  // 当SIGTERM信号到来，就向管道写入端写入SIGTERM

    bool stop_server = false;  // 初始化不关闭服务器

    // client_data记录连接进来的客户端的地址和socket信息
    // 每当客户端进行了读写操作时，就将其定时器重调至3个单位（15s）
    client_data *users_timer = new client_data[MAX_FD];

    bool timeout = false;  // 初始化还未到检测非活跃用户时间
    alarm(TIMESLOT);       // 设置闹钟，每隔5s发送SIGALRM信号

    while (!stop_server) {
        // 等待epoll上的事件发生
        // 返回I/O准备就绪的fd的数量
        int num = epoll_wait(epollfd, events, MAX_EVENT_NUM, -1);
        // 因为我们设置了信号处理函数
        // 在慢系统调用中阻塞时，如果恰好收到信号且信号进行处理返回了，就会发出EINTR的errno
        // 为了避免这样导致系统中断，我们忽略它
        if (num < 0 && errno != EINTR) {
            LOG_ERROR("%s", "epoll failure");
            break;
        }

        // 循环遍历事件数组
        for (int i = 0; i < num; ++i) {
            int sockfd = events[i].data.fd;
            // 有客户端连接进来
            if (sockfd == lfd) {
                struct sockaddr_in client_addr;
                socklen_t client_addr_size = sizeof(client_addr);

                while (1) {
                    int connfd = accept(lfd, (struct sockaddr *)&client_addr, &client_addr_size);
                    if (connfd == -1) {
                        LOG_ERROR("%s:errno is:%d", "accept error", errno);
                        break;
                    }
                    // 目前连接数满了
                    if (http_conn::m_user_count >= MAX_FD) {
                        // 给客户端返回信息，服务器正忙，并关闭连接
                        close(connfd);
                        LOG_ERROR("%s", "Internal server busy");
                        break;
                    }
                    // 将新客户数据初始化，放入数组中
                    users[connfd].init(connfd, client_addr, et);

                    // 初始化client_data数据
                    // 创建定时器，设置回调函数和超时时间，绑定用户数据，将定时器添加到链表中
                    users_timer[connfd].address = client_addr;
                    users_timer[connfd].sockfd = connfd;
                    util_timer *timer = new util_timer;
                    timer->user_data = &users[connfd];
                    timer->callback = close_conn_call;
                    time_t cur = time(NULL);
                    // 初始化超时时间为当前时间后移15s
                    timer->expire = cur + 3 * TIMESLOT;
                    users_timer[connfd].timer = timer;
                    timer_lst.add_timer(timer);
                }
                continue;
            }
            // 对方异常断开或错误事件
            // 服务器端关闭连接，移除对应的定时器
            else if (events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)) {
                util_timer *timer = users_timer[sockfd].timer;
                timer->callback(&users[sockfd]);
                // timer还存在，就删除timer
                if (timer) {
                    timer_lst.del_timer(timer);
                }
            }
            // 监听到有读事件发生
            else if (events[i].events & EPOLLIN) {
                // 如果是管道读端发来的，说明是定时信号
                if (sockfd == pipefd[0]) {
                    char signals[1024] = {0};
                    ret = recv(pipefd[0], signals, sizeof(signals), 0);
                    if (ret == -1) {
                        continue;
                    } else if (ret == 0) {
                        continue;
                    } else {
                        for (int i = 0; i < ret; ++i) {
                            if (signals[i] == SIGALRM) {
                                timeout = true;
                            } else if (signals[i] == SIGTERM) {
                                stop_server = true;
                            }
                        }
                    }
                }
                // 否则是正常的客户端请求
                else {
                    util_timer *timer = users_timer[sockfd].timer;
                    // 读取到完整请求
                    if (users[sockfd].read()) {
                        LOG_INFO("deal with the client(%s)",
                                 inet_ntoa(users[sockfd].get_address()->sin_addr));
                        Log::get_instance()->flush();

                        // 添加进线程池任务队列
                        pool->append(&users[sockfd]);

                        // 若有数据传输，则将定时器往后延迟3个单位(15s)
                        // 并对新的定时器在链表上的位置进行调整
                        if (timer) {
                            time_t cur = time(NULL);
                            timer->expire = cur + 3 * TIMESLOT;
                            timer_lst.adjust_timer(timer);
                            LOG_INFO("%s", "adjust timer once");
                            Log::get_instance()->flush();
                        }
                    }
                    // 读取失败，或对方关闭连接，则结束该用户
                    else {
                        timer->callback(&users[sockfd]);
                        if (timer) {
                            timer_lst.del_timer(timer);
                        }
                    }
                }
            }
            // 监听到写事件发生，这个写入事件是由工作线程处理完之后反馈给我们的
            else if (events[i].events & EPOLLOUT) {
                util_timer *timer = users_timer[sockfd].timer;
                // 成功写入
                if (users[sockfd].write()) {
                    LOG_INFO("send data to the client(%s)",
                             inet_ntoa(users[sockfd].get_address()->sin_addr));
                    Log::get_instance()->flush();
                    // 若有数据传输，则将定时器往后延迟3个单位
                    // 并对新的定时器在链表上的位置进行调整
                    if (timer) {
                        time_t cur = time(NULL);
                        timer->expire = cur + 3 * TIMESLOT;
                        timer_lst.adjust_timer(timer);
                        LOG_INFO("%s", "adjust timer once");
                        Log::get_instance()->flush();
                    }
                }
                // 写入失败
                else {
                    timer->callback(&users[sockfd]);
                    if (timer) {
                        timer_lst.del_timer(timer);
                    }
                }
            }
        }
        if (timeout) {
            timer_handler();
            timeout = false;
        }
    }

    close(epollfd);
    close(lfd);
    close(pipefd[1]);
    close(pipefd[0]);
    delete[] users;
    delete[] users_timer;
    delete pool;
    return 0;
}
