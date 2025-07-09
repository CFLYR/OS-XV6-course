#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"

int
main(int argc, char *argv[])
{
  char buf[512];
  char *args[MAXARG];
  int i;

  // 先把命令行参数复制到 args
  for(i = 1; i < argc && i < MAXARG-1; i++){
    args[i-1] = argv[i];
  }

  int n = i - 1; // 已有参数个数

  // 逐行读取标准输入
  int idx = 0;
  char c;
  while(read(0, &c, 1) == 1){
    if(c == '\n'){
      buf[idx] = 0;
      if(idx > 0){
        // 拆分本行参数（以空格分隔）
        int argn = n;
        int j = 0;
        while(j < idx){
          // 跳过前导空格
          while(buf[j] == ' ' && j < idx) j++;
          if(j >= idx) break;
          args[argn++] = &buf[j];
          // 找到下一个空格或结尾
          while(buf[j] != ' ' && buf[j] != 0 && j < idx) j++;
          buf[j++] = 0;
          if(argn >= MAXARG-1) break;
        }
        args[argn] = 0;

        // fork+exec 执行命令
        if(fork() == 0){
          exec(args[0], args);
          fprintf(2, "xargs: exec failed\n");
          exit(1);
        }
        wait(0);
      }
      idx = 0;
    } else {
      if(idx < sizeof(buf)-1)
        buf[idx++] = c;
    }
  }
  exit(0);
}