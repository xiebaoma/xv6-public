#include "types.h"
#include "stat.h"
#include "user.h"

// 命令行参数打印到标准输出

int main(int argc, char *argv[])
{
  int i;

  for (i = 1; i < argc; i++)
    printf(1, "%s%s", argv[i], i + 1 < argc ? " " : "\n");
  exit();
}
