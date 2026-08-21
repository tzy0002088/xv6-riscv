// init: The initial user-level program

#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/spinlock.h"
#include "kernel/sleeplock.h"
#include "kernel/fs.h"
#include "kernel/file.h"
#include "user/user.h"
#include "kernel/fcntl.h"

char *argv[] = {"sh", 0};
// 执行用户态的第一个进程，这个进程由内核态建立并跳过来
int
main(void)
{
  int pid, wpid;

  // 系统调用
  if (open("console", O_RDWR) < 0) {
    mknod("console", CONSOLE, 0);
    open("console", O_RDWR);
  }
  dup(0); // stdout
  dup(0); // stderr

  for (;;) {
    printf("init: starting sh\n");
    pid = fork();
    if (pid < 0) {
      printf("init: fork failed\n");
      exit(1);
    }
    if (pid == 0) {
      exec("sh", argv); // fork 系统调用，完全复制了一份父进程的页表，子进程 a0 设置为 0
      printf("init: exec sh failed\n");
      exit(1);
    }

    // init 进程返回了
    for (;;) {
      // this call to wait() returns if the shell exits,
      // or if a parentless process exits.
      wpid = wait((int *)0); // 等着 init 的子进程退出，这里是 shell 进程
      if (wpid == pid) { // 这个 pid 是子进程的 pid, 如果等于 sh 这个进程，说明 shell 进程退出了
        // the shell exited; restart it. // shell 退出了
        break;
      } else if (wpid < 0) {
        printf("init: wait returned an error\n"); // 异常
        exit(1);
      } else {
        // it was a parentless process; do nothing.
        // shell 进程退出了，但是 shell 的子进程还未退出，然后 shell 进程把自己的子进程挂到了 init 进程身上
      }
    }
  }
}
