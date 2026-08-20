# xv6 Labs 2021 实验报告（六）
## Lab thread: Multithreading

作者：Shalom0919  
实验分支：`thread`  
本地提交：`361efe4`  
代码仓库：[github.com/Shalom0919/xv6_labs](https://github.com/Shalom0919/xv6_labs)  
完成日期：2026-08-17  
官方评分：**60/60**

---

# 1. 实验概述

本实验基于 MIT 6.S081 Fall 2021 的 `xv6-labs-2021` 仓库 `thread` 分支，围绕用户级线程与 POSIX 多线程同步完成三个任务：实现 RISC-V 用户线程上下文切换；修复并行哈希表的竞态并保持并行性能；使用互斥锁和条件变量实现可重复使用的 barrier。

三个任务分别体现了并发系统中的三类核心问题：

- `uthread`：线程切换时，哪些寄存器状态必须跨函数调用保存和恢复。
- `ph`：多个线程更新共享链表时，如何避免 lost update，并缩小锁粒度保留并行性。
- `barrier`：如何用条件变量让一轮中的所有线程会合，同时正确区分连续的多轮同步。

官方实验说明：[MIT 6.S081 - Lab: Multithreading](https://pdos.csail.mit.edu/6.828/2021/labs/thread.html)

## 1.1 实验环境

| 项目 | 配置 |
|---|---|
| 宿主环境 | Windows + WSL2 Ubuntu，x86_64 |
| 目标架构 | RISC-V 64，xv6 运行于 QEMU |
| 编译工具链 | `riscv64-linux-gnu-gcc` 与宿主 `gcc -pthread` |
| 官方基线 | `origin/thread`，提交 `7e0a45c` |
| 实验成果 | `thread` 分支，提交 `361efe4` |
| 自动评分 | `make grade`，60/60 |

## 1.2 修改文件

| 文件 | 主要作用 |
|---|---|
| `user/uthread.c` | 定义线程上下文，初始化新线程栈和入口，调用切换函数 |
| `user/uthread_switch.S` | 保存旧线程、恢复新线程的 callee-saved 寄存器 |
| `notxv6/ph.c` | 为每个哈希桶增加互斥锁 |
| `notxv6/barrier.c` | 实现带轮次号的可重用 barrier |
| `answers-thread.txt` | 分析并行 `put()` 丢键的竞态原因 |
| `time.txt` | 记录实验用时 |
| `Makefile`、`gradelib.py` | 适配当前 RISC-V 工具链和 Python 版本 |

# 2. 用户级线程上下文切换

## 2.1 上下文结构设计

每个线程拥有独立栈、运行状态和保存的寄存器上下文：

```c
struct thread_context {
  uint64 ra;
  uint64 sp;
  uint64 s0;
  uint64 s1;
  /* s2 ... s9 */
  uint64 s10;
  uint64 s11;
};

struct thread {
  char stack[STACK_SIZE];
  int state;
  struct thread_context context;
};
```

根据 RISC-V 调用约定，`ra`、`sp` 和 `s0`～`s11` 必须在切换后保持。`a0`～`a7`、`t0`～`t6` 属于 caller-saved 寄存器，调用 `thread_switch()` 的 C 代码不能假设它们在函数返回后仍保留，所以不需要作为线程持久上下文保存。

`thread_switch(old, new)` 被普通 C 函数调用。调用本身把返回地址写入 `ra`，因此保存旧 `ra` 就等价于记录旧线程恢复后应继续执行的位置；恢复新 `ra` 后执行 `ret`，控制流便进入新线程上次暂停的位置。

## 2.2 新线程初始化

新线程从未执行过 `thread_switch()`，需要人工构造第一次恢复所需的最小上下文：

```c
memset(&t->context, 0, sizeof(t->context));
t->context.ra = (uint64)func;
t->context.sp = ((uint64)t->stack + STACK_SIZE) & ~15;
t->state = RUNNABLE;
```

栈从数组高地址向低地址增长，初始 `sp` 指向栈顶，并按 RISC-V ABI 要求做 16 字节对齐。初始 `ra` 设置为线程入口函数；调度器第一次恢复该线程后，汇编中的 `ret` 直接跳转到 `func`。

实现还检查线程槽是否耗尽，避免越过 `all_thread` 数组后写坏内存。

## 2.3 调度器衔接

调度器找到下一个 `RUNNABLE` 线程后更新状态和全局当前线程指针，再切换上下文：

```c
next_thread->state = RUNNING;
t = current_thread;
current_thread = next_thread;
thread_switch(&t->context, &next_thread->context);
```

这里必须先更新 `current_thread`。切换后 CPU 已在新线程上运行，新线程调用 `thread_yield()` 时应当看到自己的线程控制块。旧线程以后被重新调度时，`thread_switch()` 才从原调用点返回。

# 3. RISC-V 汇编切换实现

`user/uthread_switch.S` 中的实现只依赖两个参数：`a0` 指向旧上下文，`a1` 指向新上下文。寄存器以固定偏移顺序保存：

```asm
thread_switch:
    sd ra,   0(a0)
    sd sp,   8(a0)
    sd s0,  16(a0)
    sd s1,  24(a0)
    sd s2,  32(a0)
    sd s3,  40(a0)
    sd s4,  48(a0)
    sd s5,  56(a0)
    sd s6,  64(a0)
    sd s7,  72(a0)
    sd s8,  80(a0)
    sd s9,  88(a0)
    sd s10, 96(a0)
    sd s11, 104(a0)

    ld ra,   0(a1)
    ld sp,   8(a1)
    ld s0,  16(a1)
    /* 依次恢复 s1 至 s11 */
    ret
```

保存和恢复顺序必须与 C 结构体字段布局完全一致，每个 `uint64` 占 8 字节。切换 `sp` 后，后续执行已经使用新线程栈；最后 `ret` 使用刚恢复的 `ra`，完成控制流转移。

实验程序中 A、B、C 三个线程每轮打印一次并主动 `yield`。实际输出始终按 C、A、B 循环，到第 99 轮后依次退出，说明程序计数位置、栈以及被调用者保存寄存器均能跨 300 余次切换保持正确。

# 4. 并行哈希表

## 4.1 丢键竞态分析

原始 `put()` 以单链表头插法向桶中加入节点。若两个不同的 key 映射到同一桶，两个线程可能同时读取旧的 `table[i]`，分别令新节点的 `next` 指向同一个旧表头，之后又先后写 `table[i]`。最后一次表头写入覆盖前一次写入，其中一个新节点从链表中不可达，因此测试报告 key missing。

该问题不是内存分配失败，也不是不同 key 被当成相同 key；本质是“读旧表头—构造新节点—写新表头”这一复合操作缺少互斥，产生 lost update。分析记录写入 `answers-thread.txt`。

## 4.2 每桶锁方案

使用 `NBUCKET` 个互斥锁，而不是一把全局锁：

```c
struct entry *table[NBUCKET];
pthread_mutex_t bucket_lock[NBUCKET];

void put(int key, int value)
{
  int i = key % NBUCKET;
  pthread_mutex_lock(&bucket_lock[i]);
  /* 在锁内完成查找、更新或头插 */
  ...
  pthread_mutex_unlock(&bucket_lock[i]);
}
```

锁覆盖查找、更新或插入的完整临界区，确保同一桶内的链表结构串行修改。不同桶之间没有共享链表节点，因而仍可并行操作。`get()` 也使用对应桶锁完成遍历；当前程序不删除节点，返回的节点在解锁后仍有效。

所有 bucket mutex 在创建工作线程之前初始化。这样既消除数据竞争，又不会引入锁初始化与并发访问之间的竞态。

## 4.3 性能结果

手工测试使用相同的 100000 次插入工作量：

| 模式 | 时间 | 吞吐量 | 丢失 key |
|---|---:|---:|---:|
| `./ph 1` | 5.336 s | 18,742 puts/s | 0 |
| `./ph 2` | 2.957 s | 33,816 puts/s | 0 |

两线程相对单线程的插入吞吐提升为 `33816 / 18742 ≈ 1.80` 倍，高于官方 `ph_fast` 要求的 1.25 倍。结果表明每桶锁让发生哈希冲突的操作互斥，同时保留了不同桶之间的大部分并行度。

# 5. 可重用 Barrier

## 5.1 同步状态与实现

barrier 需要让每一轮的所有线程到齐后同时继续，并重复执行多轮。共享状态包括已到达线程数 `nthread`、轮次号 `round`、互斥锁和条件变量。初始化时显式把计数与轮次设为 0。

```c
static void barrier(void)
{
  int current_round;

  pthread_mutex_lock(&bstate.barrier_mutex);
  current_round = bstate.round;
  bstate.nthread++;

  if(bstate.nthread == nthread){
    bstate.nthread = 0;
    bstate.round++;
    pthread_cond_broadcast(&bstate.barrier_cond);
  } else {
    while(current_round == bstate.round)
      pthread_cond_wait(&bstate.barrier_cond,
                        &bstate.barrier_mutex);
  }
  pthread_mutex_unlock(&bstate.barrier_mutex);
}
```

最后到达的线程把到达数清零、推进轮次并广播唤醒所有等待者。其他线程在 `pthread_cond_wait()` 中原子释放 mutex 并睡眠，醒来后重新获得 mutex。

## 5.2 为什么必须使用 round 和 while

只检查 `nthread` 不足以区分相邻两轮：某个快线程可能进入下一轮并修改计数，而上一轮的慢线程才刚被唤醒。每个等待者保存进入 barrier 时的 `current_round`，只有全局轮次发生变化才允许离开，因此唤醒属于哪一轮是明确的。

条件等待必须放在 `while` 中而不是 `if` 中。POSIX 条件变量允许虚假唤醒；同时，被广播唤醒的线程重新竞争 mutex 后也需要再次确认谓词。`while(current_round == bstate.round)` 使正确性建立在受锁保护的状态谓词上，而不是建立在“收到一次信号”上。

广播必须在持锁状态下更新轮次之后执行，保证醒来的线程重新加锁时一定观察到新的 `round`。

# 6. 实验结果

## 6.1 手工运行验证

`uthread` 在 xv6 中完成全部线程切换；宿主机上的 `ph` 单线程和双线程测试均无丢键，`barrier 2` 输出 `OK; passed`：

![Lab thread 手工运行结果](../screenshots/lab-thread-manual-run.png)

图 1　用户线程、并行哈希表与 barrier 的实际运行结果

## 6.2 官方评分

在 `thread` 分支执行 `make clean && make grade`，评分脚本依次检查用户线程输出、竞态分析文本、哈希表安全性、两线程性能、barrier 和用时记录：

![Lab thread 官方评分结果](../screenshots/lab-thread-make-grade.png)

图 2　官方 `grade-lab-thread` 评分结果

各项得分为：`uthread` 20/20、竞态分析 5/5、`ph_safe` 10/10、`ph_fast` 10/10、`barrier` 14/14、`time` 1/1，合计 **60/60**。

# 7. 兼容性与复现方法

当前较新的 RISC-V 工具链会对实验基线代码产生递归与指针类型相关警告，并可能默认生成 xv6 链接脚本不接受的指令扩展属性。本实验在 `Makefile` 中补充兼容性选项和明确的 `rv64gc` 架构参数；评分库中把 Python 3 已移除的 `pipes.quote` 替换为等价的 `shlex.quote`。这些修改不改变实验算法或评分逻辑。

复现官方评分：

```sh
git switch thread
make clean
make grade
echo $?
```

通过标准是最后输出 `Score: 60/60`，且退出码为 0。单独复核三个任务可执行：

```sh
make qemu       # 进入 xv6 后运行 uthread
make ph barrier
./ph 1
./ph 2
./barrier 2
```

## 8. 实验总结

本实验完成了从寄存器级上下文切换到共享数据互斥、再到多轮条件同步的完整并发实践。用户线程部分说明上下文切换的本质是保存可恢复的控制流与 ABI 状态；哈希表部分说明锁的正确粒度既决定安全性，也决定并行性能；barrier 部分则说明条件变量必须围绕受锁保护的状态谓词使用，并用 generation/round 区分多轮事件。最终实现通过官方全部测试，得分 60/60。
