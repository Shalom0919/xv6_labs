# xv6 Labs 2021 实验报告（四）
## Lab traps: Traps

作者：Shalom0919  
实验分支：`traps`  
本地提交：`c6baf70`  
代码仓库：[github.com/Shalom0919/xv6_labs](https://github.com/Shalom0919/xv6_labs)  
完成日期：2026-07-29  
官方评分：**85/85**

---

# 1. 实验概述

本实验基于 MIT 6.S081 Fall 2021 的 `xv6-labs-2021` 仓库 `traps` 分支，围绕 RISC-V 函数调用约定、内核栈帧、用户态陷阱和时钟中断展开。实验内容分为三个部分：

- 阅读 `user/call.asm`，分析参数寄存器、内联优化、返回地址和端序。
- 实现内核 `backtrace()`，沿帧指针链输出调用路径，并接入 `sys_sleep()` 与 `panic()`。
- 实现 `sigalarm()` 和 `sigreturn()`，支持按 CPU tick 周期进入用户处理函数、完整恢复中断现场并阻止处理器重入。

官方实验说明：[MIT 6.S081 - Lab: Traps](https://pdos.csail.mit.edu/6.828/2021/labs/traps.html)

## 1.1 实验环境

| 项目 | 配置 |
|---|---|
| 宿主环境 | Windows + WSL2 Ubuntu，x86_64 |
| 目标架构 | RISC-V 64 |
| 编译工具链 | `riscv64-linux-gnu-gcc` |
| 模拟器 | `qemu-system-riscv64` |
| 官方基线 | `origin/traps`，提交 `219a8d7` |
| 实验成果 | `traps` 分支，提交 `c6baf70` |

## 1.2 陷阱处理路径

用户程序执行系统调用时，汇编桩把调用号放入 `a7` 并执行 `ecall`。`trampoline.S` 的 `uservec` 将用户寄存器保存到当前进程的 `trapframe`，切换到内核页表和内核栈，然后进入 `usertrap()`。系统调用由 `syscall()` 分派；时钟中断由 `devintr()` 识别并返回 2。返回用户态前，`usertrapret()` 与 `userret` 根据 `trapframe->epc` 和保存的寄存器恢复执行。

报警功能利用的正是这条路径：时钟中断到达时保存完整用户陷阱帧，并将返回地址 `epc` 改为处理函数地址；处理函数调用 `sigreturn()` 后，内核再把原陷阱帧恢复。

# 2. RISC-V 汇编分析

执行 `make fs.img` 会生成与当前编译器产物一致的 `user/call.asm`。本机反汇编中 `main` 的关键部分如下：

```asm
0000000000000024 <main>:
  2c: 4635                 li    a2,13
  2e: 45b1                 li    a1,12
  30: 00000517             auipc a0,0x0
  34: 7d850513             addi  a0,a0,2008
  38: 00000097             auipc ra,0x0
  3c: 616080e7             jalr  1558(ra) # 64e <printf>
  40: 4501                 li    a0,0
```

## 2.1 参数、内联与返回地址

RISC-V 调用约定使用 `a0` 至 `a7` 传递前八个整数或指针参数。调用 `printf("%d %d\n", f(8)+1, 13)` 时，格式字符串、12 和 13 分别位于 `a0`、`a1` 和 `a2`，因此 13 保存在 `a2`。

`main` 中没有对 `f` 的调用，`f` 中也没有对 `g` 的调用。编译器在优化阶段内联了两个简单函数，并把 `f(8)+1` 常量折叠为 12。`printf` 位于地址 `0x64e`。`jalr` 指令位于 `0x3c`，执行后将下一条指令地址写入 `ra`，所以 `ra=0x40`。

## 2.2 端序与可变参数

下列程序输出 `He110 World`：

```c
unsigned int i = 0x00646c72;
printf("H%x Wo%s", 57616, &i);
```

57616 的十六进制表示是 `e110`。RISC-V 为小端序，整数 `i` 在内存中排列为 `72 6c 64 00`，对应以零结尾的字符串 `rld`。若机器改为大端序，应将 `i` 改为 `0x726c6400`；57616 表示的数值不变，因此不需要修改。

对于 `printf("x=%d y=%d", 3)`，`y` 后的结果是不确定值。格式串要求两个整数，调用者却只提供一个，`printf` 会把下一个参数寄存器中的遗留内容当作参数读取。这属于缺少可变参数造成的未定义行为，不能假定某个固定结果。

完整问答已保存到 `answers-traps.txt`。

# 3. 内核调用栈回溯

## 3.1 读取帧指针

编译选项保留帧指针。RISC-V 内核用 `s0` 保存当前帧指针，因此在 `kernel/riscv.h` 中增加 `r_fp()` 内联函数，通过 `asm volatile("mv %0, s0" : "=r" (x))` 读取当前帧指针。

GCC 生成的每个内核栈帧中，`fp-8` 保存返回地址，`fp-16` 保存调用者的帧指针。xv6 为每个进程分配一个按页对齐、大小为一页的内核栈，这为回溯提供了明确边界。

## 3.2 沿栈帧链遍历

`kernel/printf.c` 中的实现如下：

```c
void
backtrace(void)
{
  uint64 fp = r_fp();
  uint64 stack_bottom = PGROUNDDOWN(fp);
  uint64 stack_top = PGROUNDUP(fp);

  printf("backtrace:\n");
  while(fp > stack_bottom && fp < stack_top){
    uint64 return_address = *(uint64 *)(fp - 8);
    uint64 caller_fp = *(uint64 *)(fp - 16);

    printf("%p\n", return_address);
    if(caller_fp <= fp || caller_fp >= stack_top)
      break;
    fp = caller_fp;
  }
}
```

循环不仅检查当前指针位于内核栈页内，还要求调用者帧指针严格增大且不越过栈顶，避免损坏的帧链导致死循环或访问其他页面。`backtrace()` 被加入 `kernel/defs.h`，并在 `sys_sleep()` 和 `panic()` 中调用。

## 3.3 回溯结果分析

执行 `bttest` 得到三个地址：

```text
0x00000000800021e4
0x00000000800020c0
0x0000000080001d72
```

使用 `riscv64-linux-gnu-addr2line -e kernel/kernel` 解析后，三个地址依次对应 `kernel/sysproc.c:62`、`kernel/syscall.c:144` 和 `kernel/trap.c:76`。这与 `bttest → sleep → ecall → usertrap → syscall → sys_sleep` 的内核调用路径相符。

# 4. 报警系统调用接口

## 4.1 用户接口与系统调用注册

在 `user/user.h` 中增加：

```c
int sigalarm(int, void (*)());
int sigreturn(void);
```

`user/usys.pl` 增加同名汇编桩；`kernel/syscall.h` 分配编号 22 和 23；`kernel/syscall.c` 将编号映射到 `sys_sigalarm` 和 `sys_sigreturn`。`Makefile` 的 traps 用户程序列表加入 `_alarmtest`，使测试程序进入 `fs.img`。

这四层分别负责 C 语言声明、用户态 `ecall` 入口、ABI 调用号和内核分派。任何一层缺失都会造成编译、链接或运行时的未知系统调用错误。

## 4.2 每进程报警状态

在 `struct proc` 中保存报警配置、计数器、重入标记和完整中断现场：

```c
int alarm_interval;
int alarm_ticks;
uint64 alarm_handler;
int alarm_active;
struct trapframe alarm_trapframe;
```

`allocproc()` 初始化所有字段，`freeproc()` 再次清零，防止进程槽复用时遗留旧报警状态。使用完整 `struct trapframe` 而不是只保存 `epc`，是因为被中断程序的通用寄存器、栈指针、返回地址和参数寄存器都可能处于有效计算过程中。

## 4.3 配置报警

`sys_sigalarm()` 读取间隔和用户函数地址，拒绝负间隔，并重置当前计数：

```c
uint64
sys_sigalarm(void)
{
  int interval;
  uint64 handler;
  struct proc *p = myproc();

  if(argint(0, &interval) < 0 ||
     argaddr(1, &handler) < 0)
    return -1;
  if(interval < 0)
    return -1;

  p->alarm_interval = interval;
  p->alarm_handler = handler;
  p->alarm_ticks = 0;
  return 0;
}
```

`sigalarm(0, 0)` 通过把间隔设为 0 停止后续报警。判断报警是否启用时只检查间隔，不检查处理函数地址，因为用户函数完全可能被链接到地址 0，官方测试中的 `periodic` 就可能出现这种情况。

# 5. 报警触发与现场恢复

## 5.1 时钟中断触发用户处理函数

在 `usertrap()` 确认进程未被杀死后，仅对用户态时钟中断计数：

```c
if(which_dev == 2 &&
   p->alarm_interval > 0 &&
   !p->alarm_active){
  p->alarm_ticks++;
  if(p->alarm_ticks >= p->alarm_interval){
    memmove(&p->alarm_trapframe, p->trapframe,
            sizeof(p->alarm_trapframe));
    p->alarm_ticks = 0;
    p->alarm_active = 1;
    p->trapframe->epc = p->alarm_handler;
  }
}
```

到达间隔时，内核先复制完整陷阱帧，再把 `alarm_active` 置位，最后把返回用户态的 `epc` 改为处理函数地址。`usertrapret()` 随后按正常陷阱返回流程进入处理函数，不需要伪造内核调用栈。

`alarm_active` 是防止重入的关键。如果处理函数本身执行时间超过一个报警周期，后续时钟中断不会再次覆盖保存现场，也不会递归进入处理函数。官方 `test2` 专门验证这一条件。

## 5.2 sigreturn 恢复完整现场

用户处理函数结束前主动调用 `sigreturn()`：

```c
uint64
sys_sigreturn(void)
{
  struct proc *p = myproc();
  uint64 interrupted_a0;

  if(!p->alarm_active)
    return -1;

  interrupted_a0 = p->alarm_trapframe.a0;
  memmove(p->trapframe, &p->alarm_trapframe,
          sizeof(p->alarm_trapframe));
  p->alarm_active = 0;
  p->alarm_ticks = 0;
  return interrupted_a0;
}
```

恢复完整陷阱帧后，系统调用分派器仍会把 `sys_sigreturn()` 的返回值写入当前 `trapframe->a0`。因此实现先取出被中断时的 `a0` 并返回它，使分派器写回的仍是原值。若固定返回 0，循环变量或临时计算恰好位于 `a0` 时就会被破坏，`alarmtest` 的寄存器一致性检查可能失败。

恢复完成后清除重入标记并将计数归零，下一周期从头计数。若处理函数内部调用 `sigalarm(0, 0)`，间隔会保持为 0，`sigreturn()` 恢复现场后也不会再次触发。

# 6. 实验结果与分析

## 6.1 xv6 内交互验证

启动 xv6 后依次执行 `bttest` 和 `alarmtest`。回溯输出三层有效内核调用路径；报警测试中，test0 验证至少触发一次，test1 验证周期触发和寄存器恢复，test2 验证长处理函数不会重入，三项均通过。

![Lab traps 交互运行结果](../screenshots/lab-traps-manual-run.png)

图 1　`bttest` 与 `alarmtest` 在 xv6 中的实际运行结果

## 6.2 官方评分

执行官方 `make grade` 后，汇编问答、backtrace、alarmtest test0/test1/test2、完整 `usertests` 和时间文件全部通过，最终得分为 **85/85**。

![Lab traps 官方评分结果](../screenshots/lab-traps-make-grade.png)

图 2　官方评分脚本输出

`usertests: OK` 表明新增陷阱逻辑没有破坏原有系统调用、进程调度、文件系统和虚拟内存行为。官方 backtrace 测试还将三个地址逐一交给 `addr2line`，因此通过不只是输出格式正确，也验证了调用链内容。
![1](image-3.png)
# 7. 兼容处理与复现验证

## 7.1 2026 工具链兼容

官方 2021 基线在当前工具链上需要三项兼容处理：

- Python 新版本移除了 `pipes`，评分器改用等价的 `shlex.quote`。
- 新版 GCC 对官方旧代码新增递归和函数指针类型诊断，只关闭 `-Winfinite-recursion` 与 `-Wincompatible-pointer-types` 两项诊断。
- 显式使用 `-march=rv64gc`，与 xv6 所需的 RISC-V 指令集扩展匹配。

这些修改只解决宿主工具版本差异，不改变 trap、系统调用或报警功能的语义。

## 7.2 官方评分复现

在 Windows PowerShell 中进入 WSL：

```powershell
wsl -d Ubuntu
```

然后运行：

```bash
cd /mnt/c/Users/Aemeath/Helper/2026/OS_Design/xv6-labs-2021
git switch traps
make clean
make grade
echo $?
```

通过标准是输出末尾出现 `Score: 85/85`，且 `echo $?` 输出 0。也可以单独验证各部分：

```bash
./grade-lab-traps "backtrace test"
./grade-lab-traps "alarmtest: test0"
./grade-lab-traps "alarmtest: test1"
./grade-lab-traps "alarmtest: test2"
./grade-lab-traps usertests
```

现场演示时可执行：

```bash
make qemu
```

进入 xv6 shell 后输入 `bttest` 和 `alarmtest`；退出 QEMU 使用 `Ctrl-a`，再按 `x`。

## 8. 实验总结

本实验完成了 RISC-V 汇编问答、内核栈回溯以及用户级周期报警。实现覆盖了函数调用约定、帧指针链、陷阱帧、时钟中断、系统调用 ABI、完整寄存器现场保存与恢复，以及用户处理函数的重入控制。

`backtrace()` 的实质是用 ABI 约定解释内核栈；报警机制的实质则是让内核临时修改陷阱返回现场，把一次普通的中断返回转换为用户处理函数调用，再由 `sigreturn()` 恢复原计算。官方评分 **85/85** 且完整 `usertests` 通过，说明功能实现和原系统兼容性均达到实验要求。
