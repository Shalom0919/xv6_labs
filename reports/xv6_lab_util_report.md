# xv6 Labs 2021 实验报告（一）

## Lab util: Xv6 and Unix Utilities

作者：Shalom0919  
实验分支：`util`  
本地提交：`1ef53b1`  
代码仓库：[github.com/Shalom0919/xv6_labs](https://github.com/Shalom0919/xv6_labs)  
完成日期：2026-07-29  
官方评分：**100/100**

---

# 1. 实验概述

本实验基于 MIT 6.S081 Fall 2021 的 `xv6-labs-2021` 仓库与 `util` 分支，目标是熟悉 xv6 的编译、启动、用户程序集成方式，以及进程、管道和文件系统相关系统调用。按照官方要求完成以下五个用户态工具：

- `sleep`：按指定 tick 数休眠。
- `pingpong`：父子进程通过两个管道交换一个字节。
- `primes`：用进程和管道实现并发素数筛。
- `find`：递归查找目录树中的同名文件。
- `xargs`：逐行读取标准输入，组装参数并执行命令。

官方实验说明：[MIT 6.S081 - Lab: Xv6 and Unix utilities](https://pdos.csail.mit.edu/6.828/2021/labs/util.html)

## 1.1 实验环境

| 项目 | 配置 |
|---|---|
| 宿主环境 | Windows + WSL2 Ubuntu，x86_64 |
| 编译器 | riscv64-linux-gnu-gcc 15.2.0 |
| 模拟器 | QEMU system-riscv64 10.2.1 |
| Python | Python 3.14.4，用于官方评分器 |
| 官方基线 | `origin/util`，提交 `f654383` |
| 实验成果 | `util`，提交 `1ef53b1` |

## 1.2 Makefile 集成

五个程序加入 `UPROGS`，从而在构建 `fs.img` 时被复制到 xv6 文件系统：

```makefile
UPROGS=\
    ...
    $U/_sleep\
    $U/_pingpong\
    $U/_primes\
    $U/_find\
    $U/_xargs\
```

# 2. sleep：定时休眠

## 2.1 设计

程序检查命令行参数数量，用 `atoi` 将字符串转换为整数，再调用 xv6 已有的 `sleep` 系统调用。参数缺失时打印用法并以非零状态退出；正常路径最后显式调用 `exit(0)`。

## 2.2 关键代码

```c
int
main(int argc, char *argv[])
{
  if(argc != 2){
    fprintf(2, "usage: sleep ticks\n");
    exit(1);
  }

  sleep(atoi(argv[1]));
  exit(0);
}
```

## 2.3 结果分析

官方测试分别验证无参数处理、休眠后正常返回，以及是否确实进入内核 `sys_sleep`。三个测试均通过。手工执行 `sleep 2` 后，shell 在相应 tick 数后继续执行 `echo sleep-ok`。

# 3. pingpong：双向进程通信

## 3.1 设计

父子进程间建立两个方向相反的管道。父进程先向子进程发送 1 字节，子进程阻塞读取后输出 `received ping` 并回写；父进程收到回包后输出 `received pong`。两端及时关闭无用文件描述符，父进程最后用 `wait` 回收子进程。

## 3.2 关键代码

```c
int parent_to_child[2];
int child_to_parent[2];
char byte = 'x';
pipe(parent_to_child);
pipe(child_to_parent);

int pid = fork();
if(pid == 0){
  close(parent_to_child[1]);
  close(child_to_parent[0]);
  read(parent_to_child[0], &byte, 1);
  printf("%d: received ping\n", getpid());
  write(child_to_parent[1], &byte, 1);
  exit(0);
}

close(parent_to_child[0]);
close(child_to_parent[1]);
write(parent_to_child[1], &byte, 1);
read(child_to_parent[0], &byte, 1);
printf("%d: received pong\n", getpid());
wait(0);
```

## 3.3 结果分析

实际输出中子进程先打印 ping，父进程后打印 pong。顺序由管道阻塞语义保证，不依赖调度时序。官方 `pingpong` 测试通过。

# 4. primes：并发素数筛

## 4.1 设计

主进程把整数 2 至 35 写入第一个管道。每一级筛选进程将收到的第一个整数视为素数并打印，再创建下一级进程，把不能被该素数整除的整数写入右侧管道。左侧写端全部关闭后，`read` 返回 0，EOF 逐级传播；每级父进程用 `wait` 保证整个管线退出后再结束。

## 4.2 关键代码

```c
static void
sieve(int input)
{
  int prime;
  if(read(input, &prime, sizeof(prime)) != sizeof(prime)){
    close(input);
    exit(0);
  }
  printf("prime %d\n", prime);

  int output[2];
  pipe(output);
  int pid = fork();
  if(pid == 0){
    close(input);
    close(output[1]);
    sieve(output[0]);
  }

  close(output[0]);
  int value;
  while(read(input, &value, sizeof(value)) == sizeof(value))
    if(value % prime != 0)
      write(output[1], &value, sizeof(value));
  close(output[1]);
  wait(0);
  exit(0);
}
```

## 4.3 结果分析

运行结果依次得到 `2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 31`，与 2 至 35 范围内的理论素数集合一致。关闭不用的管道端点是避免资源耗尽和保证 EOF 到达的关键。

# 5. find：递归目录遍历

## 5.1 设计

程序使用 `open` 和 `fstat` 判断路径类型。普通文件比较 basename；目录则逐项读取 `struct dirent`，拼接子路径并递归。跳过空目录项以及 `.`、`..`，避免递归环；同时检查 512 字节路径缓冲区边界。

## 5.2 关键代码

```c
while(read(fd, &de, sizeof(de)) == sizeof(de)){
  if(de.inum == 0)
    continue;
  memmove(name, de.name, DIRSIZ);
  name[DIRSIZ] = 0;
  if(strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
    continue;
  find(child, target);
}
```

## 5.3 结果分析

官方测试覆盖当前目录和多层递归目录，两项均通过。目录项名称最多为 `DIRSIZ` 字节，显式补零后再使用 `strcmp`，避免把非终止字符数组误当作普通 C 字符串。

# 6. xargs：标准输入到参数向量

## 6.1 设计

程序每次从标准输入读取一个字符，遇到换行符后把该行按空格或制表符切分，将得到的参数追加到原始命令 `argv`。随后 `fork`：子进程 `exec`，父进程 `wait` 后继续处理下一行。参数数组始终预留最后一个空指针，并受 `MAXARG` 限制。

## 6.2 关键代码

```c
while(read(0, &ch, 1) == 1){
  if(ch == '\n'){
    run_line(line, length, argc - 1, argv + 1);
    length = 0;
  } else if(length < sizeof(line) - 1){
    line[length++] = ch;
  } else {
    fprintf(2, "xargs: input line too long\n");
    exit(1);
  }
}
```

```c
int pid = fork();
if(pid == 0){
  exec(argv[0], argv);
  fprintf(2, "xargs: exec %s failed\n", argv[0]);
  exit(1);
}
wait(0);
```

## 6.3 结果分析

`echo hello xv6 | xargs echo prefix` 输出 `prefix hello xv6`，说明固定参数与输入参数顺序正确。官方 `xargstest.sh` 通过 `find | xargs grep` 找到三处 `hello`，验证了逐行执行、参数切分和父子进程同步。

# 7. 运行结果截图

## 7.1 xv6 内手工运行

下面截图来自本次实际执行 `make qemu` 的原始日志，包含内核启动和五个工具的运行结果。

![Lab util xv6 内手工运行结果](../screenshots/lab-util-manual-run.png)

图 1：xv6 内依次验证 sleep、pingpong、primes、find 和 xargs。

## 7.2 官方评分

下面截图来自本次实际执行 `make grade` 的原始日志末段。所有测试均为 `OK`，最终得分 `100/100`。

![Lab util 官方 make grade 结果](../screenshots/lab-util-make-grade.png)

图 2：MIT 官方评分器测试结果。

| 测试项 | 分值 | 结果 |
|---|---:|:---:|
| sleep, no arguments | 5 | OK |
| sleep, returns | 5 | OK |
| sleep, makes syscall | 10 | OK |
| pingpong | 20 | OK |
| primes | 20 | OK |
| find, current directory | 10 | OK |
| find, recursive | 10 | OK |
| xargs | 19 | OK |
| time | 1 | OK |
| 总分 | 100 | 100/100 |

# 8. 2026 工具链兼容问题

首次构建后 QEMU 串口没有启动输出。指令跟踪表明入口地址 `0x80000010` 被 GCC 15 默认架构汇编为 `c.mul`。当前编译器默认启用了 Zcb、V、B 等新 RISC-V 扩展，而 xv6 虚拟 CPU 没有启用这些扩展，因此在启动早期触发非法指令。

解决办法是在 Makefile 中显式限定目标 ISA，并处理现代 GCC 新增但不影响 xv6 逻辑的告警：

```makefile
CFLAGS = -Wall -Werror -Wno-infinite-recursion \
         -Wno-incompatible-pointer-types -O -fno-omit-frame-pointer -ggdb
CFLAGS += -march=rv64gc
ASFLAGS = -march=rv64gc
```

Python 3.14 已移除 `pipes.quote`，评分器中将其替换为等价的 `shlex.quote`。以上调整仅用于保证 2021 教学代码在 2026 工具链上可复现，不修改评分条件和实验逻辑。

# 9. 如何自行验证官方评分

在 PowerShell 中执行：

```powershell
wsl -d Ubuntu
cd /mnt/c/Users/Aemeath/Helper/2026/OS_Design/xv6-labs-2021
git checkout util
make clean
make grade
```

通过标准有两个：

1. 命令退出码为 0；
2. 输出最后一行是 `Score: 100/100`，且各项均显示 `OK`。

若只验证某一项，可执行：

```bash
./grade-lab-util sleep
./grade-lab-util pingpong
./grade-lab-util primes
./grade-lab-util find
./grade-lab-util xargs
```
![1](image.png)
# 10. 结论

本实验完成了 xv6 用户态程序的完整开发闭环：Makefile 集成、RISC-V 交叉编译、QEMU 启动、系统调用组合、官方测试和手工运行验证。五个必做任务全部完成，官方评分为 100/100。实现过程中最关键的正确性条件是关闭不用的文件描述符、理解管道阻塞与 EOF、回收子进程、跳过 `.` 和 `..`，以及严格控制路径和参数数组边界。