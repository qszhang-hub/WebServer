# CMake 最低版本号要求
cmake_minimum_required (VERSION 3.0.0)
# 项目名称, 参数值是 server, 该命令表示项目的名称是 server
project(server)
# 目标程序名
aux_source_directory(. DIR_TOOT_SRCS)
# 设置cpp标准
set(CMAKE_CXX_STANDARD 11)
# 显示指定使用的C编译器
# set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g")
# 指定生成目标
add_executable(${PROJECT_NAME} ${DIR_TOOT_SRCS})
