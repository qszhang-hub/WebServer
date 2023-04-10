# Linux下高性能Web服务器

## 项目描述

在Linux环境下，使用C++搭建轻量级高性能Web服务器，服务器支持上万并发访问，服务器后台自动进行日志记录。

------------------------------------------

## 技术栈

线程池  Epoll  Reactor/Proactor  日志系统  线程同步 HTTP 信号系统 Webbench

1.使用线程池技术，避免进程创建与销毁带来的系统开销。利用互斥锁保证线程同步安全。

2.使用单例模式和阻塞队列设计日志系统，支持同步与异步日志记录。

3.使用信号系统定时检测非活跃用户，及时断开非活跃客户端连接，节省服务器资源。

4.使用Epoll技术实现I/O多路复用，支持LT模式和ET模式，同时监听多个SOCKET。

5.支持使用主线程模拟的Proactor模型，支持高效的多线程并发。

6.使用主从状态机解析HTTP请求，避免大量的if-else语句。

7.使用Webbench对服务器进行了压力测试，支持上万并发连接。

------------------------------------------

## 使用指南

### 1.编译代码

已经编写好了两种编译方式：
CMake或Makefile

编译完成后，会在指定目录生成server可执行文件

### 2.指定端口号和工作模式

打开命令行，输入： ./server 9999 1

上述命令行代表启动服务器，端口号设为9999，1代表启用EPOLL的ET模式。若使用0，则代表启用EPOLL的LT模式。

### 3.打开浏览器

输入：<http://localhost:9999/index.html>

即可看到页面

------------------------------------------

## webbench压力测试

详细使用说明见文件："webbench使用说明.txt"

测试环境：

CPU：Intel i5-9300H 2.40GHz

内存：8G+8G

操作系统：WSL2 Ubuntu20.04

测试结果:

1.LT模式：

10000 clients, running 5 sec.

Speed=177408 pages/min, 470067 bytes/sec.

Requests: 14784 susceed, 0 failed.

------------------------------------------

2.ET模式：

10000 clients, running 5 sec.

Speed=187836 pages/min, 497797 bytes/sec.

Requests: 15653 susceed, 0 failed.

------------------------------------------

## 主要参考

1.游双《Linux高性能服务器编程》

2.@qinguoyi 《TinyWebServer
》
