
// 测试自己编写的系统调用

#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    printf("自定义系统调用返回值（CPU ID）：%d\n", test_call());
    exit(1);
}

