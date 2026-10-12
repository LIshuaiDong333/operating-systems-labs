#include "kernel/types.h"
#include "user/user.h"

int main() {
    // 创建两个管道
    int f2c[2];   // 父到子的管道
    int c2f[2];   // 子到父的管道
    
    if (pipe(f2c) < 0 || pipe(c2f) < 0) {
        fprintf(2, "pipe creation failed\n");
        exit(1);
    }

    // 在fork之前获取父进程ID
    int parent_pid = getpid();
    
    int pid = fork();
    if (pid < 0) {
        fprintf(2, "fork failed\n");
        exit(1);
    }
    
    if (pid == 0) {
        close(f2c[1]); // 关闭父到子管道的写端
        close(c2f[0]); // 关闭子到父管道的读端
        
        char buf[1];
        // 从父进程读取数据
        if (read(f2c[0], buf, sizeof(buf)) != sizeof(buf)) {
            fprintf(2, "child read error\n");
            exit(1);
        }
        close(f2c[0]); // 关闭父到子管道的读端
        
        // 父进程ID已经在fork前获取
        printf("%d: received ping from pid %d\n", getpid(), parent_pid);
        
        // 向父进程写入数据
        if (write(c2f[1], buf, sizeof(buf)) != sizeof(buf)) {
            fprintf(2, "child write error\n");
            exit(1);
        }
        close(c2f[1]); // 关闭子到父管道的写端
        
        exit(0);
    } else {
        close(f2c[0]);   // 关闭父到子管道的读端
        close(c2f[1]);   // 关闭子到父管道的写端
        
        char buf[1] = {'a'};
        // 向子写数据
        if (write(f2c[1], buf, sizeof(buf)) != sizeof(buf)) {
            fprintf(2, "parent write error\n");
            exit(1);
        }
        close(f2c[1]);  // 关闭父到子管道的写端
        
        // 从子读数据
        if (read(c2f[0], buf, sizeof(buf)) != sizeof(buf)) {
            fprintf(2, "parent read error\n");
            exit(1);
        }
        close(c2f[0]);  // 关闭子到父管道的读端
        
        printf("%d: received pong from pid %d\n", getpid(), pid);
        
        exit(0);
    }
}