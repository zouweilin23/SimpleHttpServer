#define _DEFAULT_SOURCE
#include "server.h"
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <assert.h>
#include <sys/sendfile.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <pthread.h>
#include <ctype.h>
//每个监听的epoll端口基本信息结构体
struct Fdinfo
{
    int fd;
    int epfd;
    pthread_t tid;
};

//初始化监听的套接字
int initListenFd(unsigned short port) {

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd == -1) {
        perror("socket");
        return -1;
    }
    //设置端口复用
    int opt = 1;
    int ret = setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    if (ret == -1) {
        perror("setsockopt");
        return -1;
    }

    //ip 地址 端口绑定
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;
    ret = bind(lfd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret == -1) {
        perror("bind");
        return -1;
    }
    ret = listen(lfd, 128);
    if (ret == -1) {
        perror("listen");
        return -1;
    }
    return lfd;//返回lfd文件描述符  供其他函数作为参数调用
}

//启动epoll
int epollRun(int lfd) {
    //将lfd加入epoll
    int epfd = epoll_create(1);
    if (epfd == -1) {
        perror("epoll_create");
        return -1;
    }
    printf("创建epoll成功\n");
    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = lfd;

    int ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfd, &ev);
    if (ret == -1){
        perror("epoll_ctl");
        return -1;
    }
    struct epoll_event evs[1024];//存放触发事件的epoll结构体
    int size = sizeof(evs)/(sizeof(struct epoll_event));
    while (1) {
        int num = epoll_wait(epfd, evs, size, -1);
        for (int i = 0; i < num; i++) {
            struct Fdinfo* info = (struct Fdinfo*)malloc(sizeof(struct Fdinfo));
            info->epfd = epfd;
            info->fd = evs[i].data.fd;
            int fd = evs[i].data.fd;
            if (fd == lfd) {
                pthread_create(&info->tid, NULL, acceptClient, info);
            }
            else {
                pthread_create(&info->tid, NULL, recvHttpRequest, info);
            }
            pthread_detach(info->tid);
        }
    }
    return 0;

}

//和客户端建立连接 并将其加入epoll监听事件
//int acceptClient(int fd, int epfd);
void* acceptClient(void* arg) { //arg 为struct Fdinfo类型
    struct Fdinfo* info = (struct Fdinfo*)arg;

    //建立连接
    int cfd = accept(info->fd, NULL, NULL);
    if (cfd == -1) {
        perror("accept");
        return NULL;
    }

    //设置cfd为非阻塞 epoll边缘模式
    int flag = fcntl(cfd, F_GETFL);
    fcntl(cfd, F_SETFL, flag | O_NONBLOCK);

    //添加cfd到epoll
    struct epoll_event ev;
    ev.data.fd = cfd;
    ev.events = EPOLLIN | EPOLLET;
    int ret = epoll_ctl(info->epfd, EPOLL_CTL_ADD, cfd, &ev);
    if (ret == -1) {
        perror("epoll_ctl");
        return NULL;
    }
    printf("acceptClient pthread:%ld\n", info->tid);
    free(info);
    return NULL;
}
//接收http请求
//int recvHttpRequest(int cfd, int epfd);
void* recvHttpRequest(void* arg) {
    //先将套接字缓冲区中的数据读到字符数组中，后调用parseRequestLine解析数组中的http请求
    struct Fdinfo* info = (struct Fdinfo*)arg;
    char buffer[4096] = { 0 };
    char temp[1024] = { 0 };
    int len = 0;
    int total = 0;
    //读取缓冲区数据
    while ((len = recv(info->fd, temp, sizeof(temp),0))>0) {
        if ((total + len) < sizeof(buffer)) {
            memcpy(buffer + total, temp, len);
        }
        total += len;
        memset(temp, 0, sizeof(temp));
    }
    //读取数据后开始解析
    if (len == 0) {
        //客户端断开
        epoll_ctl(info->epfd, EPOLL_CTL_DEL, info->fd, NULL);
        close(info->fd);
    }
    else if (len == -1&&errno==EAGAIN) {
        //缓冲区数据读取完毕 调用具体的解析函数
        char* pt = strstr(buffer, "\r\n");
        int reqlen = pt - buffer;
        buffer[reqlen] = '\0';//截断 这里只解析http请求中的请求行
        printf("开始解析http请求行...\n");
        parseRequestLine(buffer, info->fd);
        printf("http请求行解析完毕...\n");
    }
    else {
        perror("recv");
    }
    printf("recvMsg pthread:%ld\n", info->tid);
    free(info);
    return NULL;
}
//解析请求行
int parseRequestLine(const char* line, int cfd) {
    char method[12] = { 0 };
    char path[1024] = { 0 };
    sscanf(line, "%[^ ] %[^ ]", method, path);
    printf("method: %s, path: %s\n", method, path);
    if (strcasecmp(method, "get") != 0) {
        return -1;
    }
    decodeMsg(path, path);//处理文件名中特殊字符  乱码
    //处理http请求中的路径
    char* file = NULL;
    if (strcmp(path, "/") == 0) {
        file = "./";
    }
    else {
        file = path + 1;
    }
    printf("请求行里的文件路径为：%s\n", file);

    //判断文件类型 根据不同的文件类型 发送不同的html到浏览器
    struct stat st;
    int ret = stat(file, &st);
    if (ret == -1) {
        sendHeadMsg(cfd, 404, "Not Found", getFileType(".html"), -1);
        sendFile("404.html", cfd);
        return 0;
    }
    //目录
    if (S_ISDIR(st.st_mode)) {
        sendHeadMsg(cfd, 200, "OK", getFileType(".html"), st.st_size);
        sendDir(file, cfd);//发送目录的html形式
    }
    else {//文件 则直接发送文件数据到浏览器
        sendHeadMsg(cfd, 200, "OK", getFileType(file), st.st_size);
        sendFile(file, cfd);
    }
    return 0;
}

//发送文件给客户端
int sendFile(const char* fileName, int cfd) {
    int fd = open(fileName, O_RDONLY);
    if (fd == -1) {
        perror("open");
        return -1;
    }
    
    //高效率发送文件方式  sendfile
    off_t offset = 0;
    int size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    while (offset < size) {
        int ret = sendfile(cfd, fd, &offset, size - offset);
        printf("已发送文件的%d字节数据给客户端\n", ret);
        if (ret == -1 ) {
            if (errno == EAGAIN) {
                printf("对方缓冲区已满，请稍后重试...\n");
            }
            else {
                perror("sendfile");
                close(fd);
                return 0;
            }
        }
    }
    close(fd);
    return size;
}

//发送响应头（状态行+响应头）发送文件前需要先发送响应头  HTTP/1.1 200 OK
int sendHeadMsg(int cfd, int status, const char* descr, const char* type, int length) {
    char buffer[4096] = { 0 };
    sprintf(buffer, "http/1.1 %d %s\r\n", status, descr);
    sprintf(buffer + strlen(buffer), "content-type: %s\r\n", type);
    sprintf(buffer + strlen(buffer), "content-length: %d\r\n\r\n", length);

    //发送http响应给浏览器
    send(cfd, buffer, strlen(buffer), 0);
    return 0;
}

//要发送的文件类型
const char* getFileType(const char* name) {
    // a.jpg a.mp4 a.html
    // 自右向左查找‘.’字符, 如不存在返回NULL
    const char* dot = strrchr(name, '.');
    if (dot == NULL)
        return "text/plain; charset=utf-8";	// 纯文本
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0)
        return "text/html; charset=utf-8";
    if (strcmp(dot, ".jpg") == 0 || strcmp(dot, ".jpeg") == 0)
        return "image/jpeg";
    if (strcmp(dot, ".gif") == 0)
        return "image/gif";
    if (strcmp(dot, ".png") == 0)
        return "image/png";
    if (strcmp(dot, ".css") == 0)
        return "text/css";
    if (strcmp(dot, ".au") == 0)
        return "audio/basic";
    if (strcmp(dot, ".wav") == 0)
        return "audio/wav";
    if (strcmp(dot, ".avi") == 0)
        return "video/x-msvideo";
    if (strcmp(dot, ".mov") == 0 || strcmp(dot, ".qt") == 0)
        return "video/quicktime";
    if (strcmp(dot, ".mpeg") == 0 || strcmp(dot, ".mpe") == 0)
        return "video/mpeg";
    if (strcmp(dot, ".vrml") == 0 || strcmp(dot, ".wrl") == 0)
        return "model/vrml";
    if (strcmp(dot, ".midi") == 0 || strcmp(dot, ".mid") == 0)
        return "audio/midi";
    if (strcmp(dot, ".mp3") == 0)
        return "audio/mpeg";
    if (strcmp(dot, ".ogg") == 0)
        return "application/ogg";
    if (strcmp(dot, ".pac") == 0)
        return "application/x-ns-proxy-autoconfig";

    return "text/plain; charset=utf-8";
}

/*
<html>
    <head>
        <title>test</title>
    </head>
    <body>
        <table>
            <tr>
                <td></td> 文件名
                <td></td> 文件大小
            </tr>
            <tr>
                <td></td>
                <td></td>
            </tr>
        </table>
    </body>
</html>
*/

//发送目录 如果get请求中的文件路径为目录，则需要发送上面的html代码形式给浏览器
//该函数用于创建发送给浏览器的html代码
int sendDir(const char* dirName, int cfd) {
    char buffer[4096] = { 0 };
    sprintf(buffer, "<html><head><title>%s</title></head><body><table>", dirName);

    //读取目录
    struct dirent** namelist;
    int num = scandir(dirName, &namelist, NULL, alphasort);
    for (int i = 0; i < num; i++) {
        //将得到的目录中的每个文件信息写入到html中
        char* name = namelist[i]->d_name;
        struct stat st;
        char subpath[1024] = { 0 };
        sprintf(subpath, "%s/%s", dirName, name);//拼接路径
        stat(subpath, &st);
        if (S_ISDIR(st.st_mode)) {
            // a标签 <a href="">name</a>
            sprintf(buffer + strlen(buffer), "<tr><td><a href=\"%s/\">%s</a></td><td>%ld</td></tr>", name, name, st.st_size);
        }
        else {
            sprintf(buffer + strlen(buffer), "<tr><td><a href=\"%s\">%s</a></td><td>%ld</td></tr>", name, name, st.st_size);
        }
        send(cfd, buffer, strlen(buffer), 0);
        memset(buffer, 0, sizeof(buffer));
        free(namelist[i]);
    }
    sprintf(buffer, "</table></body></html>");
    send(cfd, buffer, strlen(buffer), 0);
    free(namelist);
    return 0;
}

//文件名中的特殊字符处理
int hexToDec(char c) {
    //将字符转换为整形
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return 0;
}
void decodeMsg(char* to, char* from) {
    for (; *from != '\0'; ++from, ++to) {
        // isxdigit -> 判断字符是不是16进制格式, 取值在 0-f
        // Linux%E5%86%85%E6%A0%B8.jpg
        // 将16进制的数 -> 十进制 将这个数值赋值给了字符 int -> char
        // B2 == 178
        // 将3个字符, 变成了一个字符, 这个字符就是原始数据
        if (from[0] == '%' && isxdigit(from[1]) && isxdigit(from[2])) {
            *to = hexToDec(from[1]) * 16 + hexToDec(from[2]);
            from += 2;
        }
        else {
            *to = *from;
        }
    }
    *to = '\0';
}
