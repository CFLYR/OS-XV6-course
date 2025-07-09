#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"

// 递归查找函数
void
find(const char *path, const char *target)
{
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  // 打开目录
  if((fd = open(path, 0)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }

  // 获取目录信息
  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }

  // 如果是文件，直接判断名字
  if(st.type == T_FILE){
    // 获取最后一级文件名
    const char *name = path;
    for(const char *q = path; *q; q++)
      if(*q == '/')
        name = q + 1;
    if(strcmp(name, target) == 0){
      printf("%s\n", path);
    }
    close(fd);
    return;
  }

  // 如果是目录，递归遍历
  if(st.type == T_DIR){
    // 构造路径前缀
    int len = strlen(path);
    if(len + 1 + DIRSIZ + 1 > sizeof(buf)){
      printf("find: path too long\n");
      close(fd);
      return;
    }
    strcpy(buf, path);
    p = buf + len;
    *p++ = '/';

    // 遍历目录项
    while(read(fd, &de, sizeof(de)) == sizeof(de)){
      if(de.inum == 0)
        continue;
      // 跳过 . 和 ..
      if(strcmp(de.name, ".") == 0 || strcmp(de.name, "..") == 0)
        continue;
      // 拼接完整路径
      memmove(p, de.name, DIRSIZ);
      p[DIRSIZ] = 0;
      // 获取子项 stat 信息
      if(stat(buf, &st) < 0)
        continue;
      if(st.type == T_DIR){
        // 递归子目录
        find(buf, target);
      } else if(st.type == T_FILE){
        // 判断文件名
        if(strcmp(de.name, target) == 0){
          printf("%s\n", buf);
        }
      }
    }
  }
  close(fd);
}

int
main(int argc, char *argv[])
{
  if(argc != 3){
    fprintf(2, "Usage: find <path> <filename>\n");
    exit(1);
  }
  find(argv[1], argv[2]);
  exit(0);
}