#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "stddef.h"

int
main(int argc, char *argv[]) 
{
  int parent_fd[2]; // 父进程写，子进程读
  int child_fd[2];  // 子进程写，父进程读
  pipe(parent_fd);
  pipe(child_fd);

  if (fork() == 0) // 子进程
  {
    close(parent_fd[1]);// 关闭父进程写端
    close(child_fd[0]);// 关闭子进程读端
    char buf[10];// 缓冲区大小
    read(parent_fd[0], buf, sizeof buf);// 从父进程读取数据
    int id = getpid();// 获取子进程的ID
    printf("%d: received %s\n", id, buf);// 打印子进程ID和接收到的数据
    write(child_fd[1], "pong", 4);// 向子进程发送数据
    close(parent_fd[0]);// 关闭父进程读端
    close(child_fd[1]);// 关闭子进程写端
  }
  else // 父进程
  {
    close(parent_fd[0]); // 关闭父进程读端
    close(child_fd[1]); // 关闭子进程写端
    char buf[10];
    write(parent_fd[1], "ping", 4);// 向父进程发送数据
    wait(0);// 等待子进程结束
    read(child_fd[0], buf, sizeof buf);// 从子进程读取数据
    int id = getpid();// 获取父进程的ID
    printf("%d: received %s\n", id, buf);// 打印父进程ID和接收到的数据
    close(parent_fd[1]);// 关闭父进程写端
    close(child_fd[0]);// 关闭子进程读端

  }
  exit(0);
}