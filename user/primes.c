#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

// 递归筛选素数的函数
void primes(int pfd[2]) __attribute__((noreturn));

void
primes(int pfd[2])
{
  close(pfd[1]); // 关闭读入pipe的写端，只保留读端

  int prime;
  // 读取第一个数，作为当前进程要筛选的素数
  if(read(pfd[0], &prime, sizeof(prime)) == 0){
    // 如果没有数据，说明已经结束，关闭读端并退出
    close(pfd[0]);
    exit(0);
  }
  // 打印当前素数
  printf("prime %d\n", prime);

  // 创建下一个管道，传递给下一个进程
  int newp[2];
  pipe(newp);

  int pid = fork();
  if(pid == 0){
    // 子进程递归处理下一个素数
    close(pfd[0]); // 子进程不再需要上一个pipe的读端
    primes(newp);  // 递归调用
  } else {
    // 父进程负责筛选，把不能被prime整除的数写入新管道
    int num;
    while(read(pfd[0], &num, sizeof(num))){
      if(num % prime != 0){
        write(newp[1], &num, sizeof(num));
      }
    }
    // 关闭所有用过的文件描述符
    close(pfd[0]);
    close(newp[1]);
    // 等待子进程结束
    wait(0);
    exit(0);
  }
}

int
main(void)
{
  int p[2];
  pipe(p); // 创建第一个管道

  int pid = fork();
  if(pid == 0){
    // 子进程递归筛选素数
    primes(p);
  } else {
    // 父进程写入2~280
    close(p[0]); // 父进程只写
    for(int i = 2; i <= 280; i++){
      write(p[1], &i, sizeof(i));
    }
    close(p[1]); // 写完关闭写端
    wait(0);     // 等待所有子进程结束
  }
  exit(0);
}