#pragma once
#include <iostream>
#include "server.h"
#include "stdio.h"
using namespace std;

int main(int argc, char* argv[]) {
    if (argc < 3) {
        cout << "./main port path" << endl;
        return -1;
    }

    unsigned short port = atoi(argv[1]);

    chdir(argv[2]);//切换到工作要访问的目录

    int lfd = initListenFd(port);//创建服务端用于监听的套接字

    epollRun(lfd);//运行epoll反应堆

    return 0;
}
