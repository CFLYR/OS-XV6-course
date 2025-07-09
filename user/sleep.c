#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{

  if(argc < 2){// 检测是否提供了参数
    // 如果没有提供参数，打印用法信息并退出
    fprintf(2, "Usage: sleep time...\n");
    exit(1);
  }
  int time = atoi(argv[1]);// 将第一个参数转换为整数
  if(time < 0){
    // 如果时间小于0，打印错误信息并退出
      fprintf(2, "sleep: invalid time %s\n", argv[1]);
      exit(1);
  }
  // 调用系统调用sleep，传入时间参数
  if(sleep(time) < 0){
    // 如果sleep系统调用失败，打印错误信息并退出
    fprintf(2, "sleep: failed to sleep for %d seconds\n", time);
    exit(1);
  }
  exit(0);
}