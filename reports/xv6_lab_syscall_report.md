# xv6 Labs 2021 实验报告（二）
## Lab syscall: System Calls

作者：Shalom0919  
实验分支：`syscall`  
本地提交：`b312e1d`  
代码仓库：[github.com/Shalom0919/xv6_labs](https://github.com/Shalom0919/xv6_labs)  
完成日期：2026-07-29  
官方评分：**35/35**

---

# 1. 实验概述

本实验基于 MIT 6.S081 Fall 2021 的 `xv6-labs-2021` 仓库 `syscall` 分支，目标是理解用户程序通过 RISC-V `ecall` 进入内核、系统调用分派、参数读取和结果返回的完整路径，并新增以下两个系统调用：

- `trace(mask)`：按位掩码跟踪当前进程及其后代的系统调用。
- `sysinfo(info)`：向用户空间返回空闲物理内存字节数和当前进程数。

官方实验说明：[MIT 6.S081 - Lab: System calls](https://pdos.csail.mit.edu/6.828/2021/labs/syscall.html)

## 1.1 实验环境

| 项目 | 配置 |
|---|---|
| 宿主环境 | Windows + WSL2 Ubuntu，x86_64 |
| 目标架构 | RISC-V 64 |
| 编译工具链 | `riscv64-linux-gnu-gcc` |
| 模拟器 | `qemu-system-riscv64` |
| 官方基线 | `origin/syscall` |
| 实验成果 | `syscall` 分支，提交 `b312e1d` |

## 1.2 系统调用路径

用户程序调用 `trace()` 或 `sysinfo()` 后，`user/usys.pl` 生成的汇编桩把系统调用编号写入寄存器 `a7`，再执行 `ecall`。内核陷入处理代码最终调用 `syscall()`，通过编号索引分派表，执行 `sys_trace()` 或 `sys_sysinfo()`。返回值写入陷阱帧的 `a0`，用户程序恢复执行后即可获得结果。

# 2. 用户接口与系统调用注册

## 2.1 用户态声明和汇编桩

在 `user/user.h` 中预声明 `struct sysinfo` 并增加函数原型，避免用户程序依赖内核实现细节：

```c
struct sysinfo;

int trace(int);
int sysinfo(struct sysinfo *);
```

在 `user/usys.pl` 中增加入口。构建阶段会据此生成执行 `ecall` 的 `user/usys.S`：

```perl
entry("trace");
entry("sysinfo");
```

## 2.2 编号与内核分派

在 `kernel/syscall.h` 中为两个调用分配唯一编号：

```c
#define SYS_trace   22
#define SYS_sysinfo 23
```

在 `kernel/syscall.c` 中声明内核处理函数并加入分派表：

```c
extern uint64 sys_trace(void);
extern uint64 sys_sysinfo(void);

static uint64 (*syscalls[])(void) = {
  /* existing entries */
  [SYS_trace]   sys_trace,
  [SYS_sysinfo] sys_sysinfo,
};
```

同时在 `Makefile` 的 `UPROGS` 中加入 `_trace` 和 `_sysinfotest`，使它们被写入 xv6 文件系统镜像。

## 2.3 分析

系统调用必须同时完成用户声明、汇编桩、调用编号和内核分派四层注册。缺少任意一层都会分别表现为编译失败、链接失败、未知系统调用或无法进入实现函数。本实验通过这条链路验证了 xv6 将用户 API 与内核服务解耦的方式。

# 3. trace 系统调用

## 3.1 保存跟踪掩码

在 `struct proc` 中增加进程私有的 `trace_mask`。创建进程槽时初始化为 0，释放进程时清零，防止进程槽复用后继承旧状态：

```c
struct proc {
  /* existing fields */
  int trace_mask;
};
```

`sys_trace()` 使用 `argint()` 读取第 0 个参数，并保存到当前进程：

```c
uint64
sys_trace(void)
{
  int mask;

  if(argint(0, &mask) < 0)
    return -1;
  myproc()->trace_mask = mask;
  return 0;
}
```

## 3.2 fork 继承

实验要求调用者随后创建的所有子进程继续使用相同掩码，因此在 `fork()` 中复制该字段：

```c
safestrcpy(np->name, p->name, sizeof(p->name));
np->trace_mask = p->trace_mask;
```

掩码属于进程状态而不是全局状态，所以未调用 `trace()` 的其他进程不会受到影响。

## 3.3 返回前输出

`syscall()` 完成分派后检查对应位。如果第 `num` 位为 1，就输出 PID、调用名称和返回值：

```c
p->trapframe->a0 = syscalls[num]();
if(p->trace_mask & (1 << num))
  printf("%d: syscall %s -> %d\n",
         p->pid, syscall_names[num],
         (int)p->trapframe->a0);
```

输出发生在系统调用完成之后，因此 `a0` 已包含真实返回值。名称数组使用系统调用编号作为索引，避免复杂的条件分支。

## 3.4 结果分析

执行 `trace 32 grep hello README` 时，32 等于 `1 << SYS_read`，运行结果仅包含 `read`。官方测试还验证了跟踪全部调用、不跟踪任何调用以及 `fork()` 后代继承掩码，四项均为 `OK`。

# 4. sysinfo 系统调用

## 4.1 统计空闲物理内存

物理内存分配器以单向链表维护空闲页。`freemem()` 在持有 `kmem.lock` 时遍历链表，每个节点代表一个 `PGSIZE` 字节的页面：

```c
uint64
freemem(void)
{
  uint64 bytes = 0;
  struct run *r;

  acquire(&kmem.lock);
  for(r = kmem.freelist; r; r = r->next)
    bytes += PGSIZE;
  release(&kmem.lock);
  return bytes;
}
```

锁保证统计期间链表不会被其他 CPU 上的 `kalloc()` 或 `kfree()` 修改。

## 4.2 统计活动进程

`nproc()` 遍历固定长度的进程表，并分别获取每个进程的锁。状态不为 `UNUSED` 的槽均计入结果，包括 `USED`、`SLEEPING`、`RUNNABLE`、`RUNNING` 和 `ZOMBIE`：

```c
uint64
nproc(void)
{
  uint64 n = 0;
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->state != UNUSED)
      n++;
    release(&p->lock);
  }
  return n;
}
```

---

## 4.3 安全返回用户空间

`sys_sysinfo()` 读取用户指针，在内核栈上构造结果，再通过 `copyout()` 写入当前进程页表：

```c
uint64
sys_sysinfo(void)
{
  uint64 addr;
  struct sysinfo info;
  struct proc *p = myproc();

  if(argaddr(0, &addr) < 0)
    return -1;
  info.freemem = freemem();
  info.nproc = nproc();
  if(copyout(p->pagetable, addr,
             (char *)&info, sizeof(info)) < 0)
    return -1;
  return 0;
}
```

内核不能直接解引用用户指针。`copyout()` 会检查虚拟地址映射和访问范围，所以官方测试传入非法地址时能够正确返回 `-1`，而不会破坏内核。

## 4.4 结果分析

`sysinfotest` 验证了四类行为：正常调用、非法用户地址、`sbrk()` 分配和释放页面后的空闲内存变化，以及 `fork()` 前后的进程数变化。手工执行和官方评分均输出 `sysinfotest: OK`。

# 5. 运行结果

## 5.1 xv6 内手工验证

手工启动 xv6 后依次执行 `trace 32 grep hello README` 和 `sysinfotest`。跟踪输出只包含 `read`，系统信息测试完成。

![Lab syscall 手工运行结果](../screenshots/lab-syscall-manual-run.png)

## 5.2 官方评分

执行 `make grade` 后，所有功能测试及 `time.txt` 检查均通过，最终得分为 **35/35**。

![Lab syscall 官方评分结果](../screenshots/lab-syscall-make-grade.png)

# 6. 工具链兼容处理

2021 年代码在当前 2026 环境中遇到两类非实验逻辑问题：

- Python 3.13 及以上已删除 `pipes` 模块，因此评分器将 `pipes.quote` 等价替换为 `shlex.quote`。
- 新版 GCC 增加了递归和函数指针类型诊断；官方基础代码在 `-Werror` 下无法构建，因此只关闭 `-Winfinite-recursion` 和 `-Wincompatible-pointer-types` 两项新诊断。
- 显式指定 `-march=rv64gc`，保证新版 RISC-V 工具链使用与 xv6 2021 相符的 ISA 扩展集合。

这些修改只影响宿主机评分器和编译兼容性，不改变 `trace`、`sysinfo` 或 xv6 原有运行语义。

## 6.1 复现与评分验证

在 PowerShell 中进入 WSL：

```powershell
wsl -d Ubuntu
```

随后执行：

```bash
cd /mnt/c/Users/Aemeath/Helper/2026/OS_Design/xv6-labs-2021
git switch syscall
make clean
make grade
echo $?
```

通过标准是输出末尾出现 `Score: 35/35`，并且紧接着执行的 `echo $?` 输出 `0`。需要单独验证时，可执行：

```bash
./grade-lab-syscall "trace 32 grep"
./grade-lab-syscall "trace all grep"
./grade-lab-syscall "trace nothing"
./grade-lab-syscall "trace children"
./grade-lab-syscall sysinfotest
```
![1](image-1.png)
## 6.2 实验总结

本实验完成了两个新系统调用及其全部注册路径。`trace` 展示了系统调用分派、进程私有状态和 `fork()` 继承；`sysinfo` 展示了内核数据结构统计、锁保护和用户/内核地址空间之间的数据复制。官方评分结果为 **35/35**，表明实现满足功能、隔离性、继承关系、非法地址处理和资源统计要求。
