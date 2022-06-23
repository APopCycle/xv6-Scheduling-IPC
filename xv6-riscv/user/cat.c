#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

char buf[512];

void cat(int fd) {
  int n; // 读入或写入的字节数
    
  while((n = read(fd, buf, sizeof(buf))) > 0) { // 调用read，只要还有字节读入就一直读下去
    if (write(1, buf, n) != n) { // 如果写入的字节数不等于读入的字节数，则写入错误
      fprintf(2, "cat: write error\n"); // 报错
      exit(1); // 退出
    }
  }
  if(n < 0){ // 读入的字节数小于0，则读入错误
    fprintf(2, "cat: read error\n"); // 报错
    exit(1); // 退出
  }
  // 如果n=0，证明该文件已读取完成 -> 函数运行完成
}

int
main(int argc, char *argv[])
{
  int fd, i;

  if(argc <= 1){
    cat(0);
    exit(0);
  }

  for(i = 1; i < argc; i++){
    if((fd = open(argv[i], 0)) < 0){
      fprintf(2, "cat: cannot open %s\n", argv[i]);
      exit(1);
    }
    cat(fd);
    close(fd);
  }
  exit(0);
}
