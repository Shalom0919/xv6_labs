# xv6 Labs 2021 实验报告（五）
## Lab cow: Copy-on-Write Fork

作者：Shalom0919  
实验分支：`cow`  
本地提交：`a419e42`  
代码仓库：[github.com/Shalom0919/xv6_labs](https://github.com/Shalom0919/xv6_labs)  
完成日期：2026-07-30  
官方评分：**110/110**

---

# 1. 实验概述

本实验基于 MIT 6.S081 Fall 2021 的 `xv6-labs-2021` 仓库 `cow` 分支，实现 Copy-on-Write Fork。原始 xv6 的 `fork()` 会立即为子进程分配物理页并复制父进程全部用户内存；当进程较大，或子进程随后立即执行 `exec()` 时，这些复制会消耗大量时间和物理内存。

COW fork 将复制推迟到实际写入时：父子页表先共同指向相同物理页，并清除可写权限。任一进程写入共享页时，RISC-V 产生 store page fault，内核再为写入者建立私有副本。实验实现包括：

- 修改 `uvmcopy()`，建立父子共享的只读 COW 映射。
- 使用 RISC-V PTE 的 RSW 位区分 COW 页与真正只读页。
- 为每个可分配物理页维护并发安全的引用计数。
- 在 `usertrap()` 中处理 COW 写故障。
- 让内核 `copyout()` 写用户空间时执行相同的 COW 拆分。

官方实验说明：[MIT 6.S081 - Lab: Copy-on-Write Fork](https://pdos.csail.mit.edu/6.828/2021/labs/cow.html)

## 1.1 实验环境

| 项目 | 配置 |
|---|---|
| 宿主环境 | Windows + WSL2 Ubuntu，x86_64 |
| 目标架构 | RISC-V 64，Sv39 |
| 编译工具链 | `riscv64-linux-gnu-gcc` |
| 模拟器 | `qemu-system-riscv64` |
| 官方基线 | `origin/cow`，提交 `c981891` |
| 实验成果 | `cow` 分支，提交 `a419e42` |

## 1.2 COW 状态转换

| 事件 | 页表权限与引用计数 |
|---|---|
| `kalloc()` | 新物理页引用计数设为 1 |
| `fork()` | 原可写页清除 `PTE_W`、设置 `PTE_COW`，子映射引用计数加 1 |
| 多引用页被写 | 分配新页、复制内容、写入者改映射，旧页引用计数减 1 |
| 单引用 COW 页被写 | 不复制，只恢复 `PTE_W` 并清除 `PTE_COW` |
| 进程退出或缩小内存 | 删除映射并递减计数；计数归零才进入空闲链表 |

真正的代码页原本没有 `PTE_W`。`fork()` 不应把这类页面标记成 COW，否则对只读代码的非法写入会被错误地转化为合法私有副本。

# 2. COW 页表标记

## 2.1 使用 RSW 软件保留位

Sv39 PTE 的第 8、9 位为 RSW，硬件不会解释，可由操作系统保存软件状态。本实验使用第 8 位：

```c
#define PTE_COW (1L << 8)
```

页表项的关键状态为：

- 普通可写页：`PTE_V | PTE_U | PTE_W`。
- COW 共享页：保留原读、执行和用户权限，清除 `PTE_W`，设置 `PTE_COW`。
- 真正只读页：没有 `PTE_W`，也没有 `PTE_COW`。

写入 COW 页会触发异常；写入真正只读页同样触发异常，但后者不满足 `PTE_COW` 条件，进程应被终止。

## 2.2 uvmcopy 建立共享映射

原始 `uvmcopy()` 对每一页执行 `kalloc()` 和 `memmove()`。修改后，父子直接共享物理地址：

```c
for(i = 0; i < sz; i += PGSIZE){
  pte = walk(old, i, 0);
  pa = PTE2PA(*pte);
  flags = PTE_FLAGS(*pte);
  if(flags & PTE_W){
    flags = (flags & ~PTE_W) | PTE_COW;
    *pte = PA2PTE(pa) | flags;
  }
  if(mappages(new, i, PGSIZE, pa, flags) != 0)
    goto err;
  krefinc(pa);
}
sfence_vma();
```

只对原可写页设置 COW。已经处于 COW 状态的页会保留该标记，使连续多次 `fork()` 可以继续共享。子页表建立映射成功后增加物理页引用计数。

修改父页表的 `PTE_W` 后执行 `sfence_vma()`，清除可能仍允许写入的旧 TLB 项。否则父进程返回用户态后可能绕过新的只读权限，直接修改共享页。

## 2.3 失败回滚

若建立子页表映射失败，`uvmunmap(new, 0, i / PGSIZE, 1)` 删除已完成的子映射。每次 `kfree()` 会递减引用计数，因此共享页不会被提前释放。

部分父页可能已经变为 COW，但此时引用计数仍为 1。父进程下次写入时走单引用快速路径，直接恢复可写权限，不需要额外复制。这保证 `fork()` 失败后父进程仍可继续运行。

# 3. 物理页引用计数

## 3.1 索引和并发保护

一张物理页可能同时被多个进程页表引用，只有最后一个映射消失时才能释放。本实验按物理页号建立固定数组：

```c
#define NPHYPAGES ((PHYSTOP - KERNBASE) / PGSIZE)
#define PAINDEX(pa) (((uint64)(pa) - KERNBASE) / PGSIZE)

struct {
  struct spinlock lock;
  int count[NPHYPAGES];
} krefs;
```

数组仅覆盖 QEMU 提供给 xv6 的物理内存区间，避免按完整物理地址建立过大的稀疏数组。所有增加、减少和读取操作都受 `krefs.lock` 保护，防止多个 CPU 同时 fork、退出或处理写故障时丢失更新。

## 3.2 分配和释放语义

`kalloc()` 从空闲链表取出页面后，将引用计数从 0 设置为 1：

```c
if(r){
  acquire(&krefs.lock);
  if(krefs.count[PAINDEX(r)] != 0)
    panic("kalloc ref");
  krefs.count[PAINDEX(r)] = 1;
  release(&krefs.lock);
  memset((char*)r, 5, PGSIZE);
}
```

`kfree()` 不再无条件释放。它先递减引用计数，只有计数归零才填充调试字节并加入空闲链表：

```c
acquire(&krefs.lock);
if(krefs.count[PAINDEX(pa)] < 1){
  release(&krefs.lock);
  panic("kfree ref");
}
count = --krefs.count[PAINDEX(pa)];
release(&krefs.lock);

if(count > 0)
  return;

memset(pa, 1, PGSIZE);
/* add pa to kmem.freelist */
```

启动阶段的空闲页尚未经过 `kalloc()`。`freerange()` 在首次调用 `kfree()` 前给每页增加一个临时引用，使其由 1 正常递减到 0，再进入空闲链表。页表页、内核栈和普通用户页仍遵循原有“一次分配、一次释放”的计数路径。

# 4. COW 写故障处理

## 4.1 统一 cowalloc

用户态写故障和内核 `copyout()` 都需要拆分 COW 页，因此将核心逻辑集中在 `cowalloc()`：

```c
va = PGROUNDDOWN(va);
pte = walk(pagetable, va, 0);
if(pte == 0 || (*pte & PTE_V) == 0 ||
   (*pte & PTE_U) == 0 ||
   (*pte & PTE_COW) == 0)
  return -1;
pa = PTE2PA(*pte);
flags = (PTE_FLAGS(*pte) | PTE_W) & ~PTE_COW;
if(krefcnt(pa) == 1){
  *pte = PA2PTE(pa) | flags;
  sfence_vma();
  return 0;
}
if((mem = kalloc()) == 0)
  return -1;
memmove(mem, (char*)pa, PGSIZE);
*pte = PA2PTE(mem) | flags;
kfree((void*)pa);
sfence_vma();
```

当旧页引用计数为 1 时，没有其他页表能观察到写入，可以原地恢复可写权限。引用数大于 1 时才分配和复制，随后当前页表指向新页，并通过 `kfree(old_pa)` 递减旧页计数。

`kalloc()` 失败时返回错误，用户写故障路径会杀死进程；原 PTE 和旧引用计数均保持不变。

## 4.2 usertrap 识别 store page fault

RISC-V 的 store/AMO page fault 原因号为 15。在系统调用和设备中断分支之间增加：

```c
} else if(r_scause() == 15){
  uint64 va = r_stval();
  if(va >= p->sz ||
     cowalloc(p->pagetable, va) < 0)
    p->killed = 1;
}
```

`r_stval()` 提供发生故障的用户虚拟地址。实现同时检查进程大小、PTE 有效位、用户位和 COW 位。写入越界地址、栈保护页或真正只读页都会失败并终止进程。

处理成功后 `usertrapret()` 回到产生故障的原指令。此时 PTE 已指向当前进程的可写页，CPU 重新执行写指令即可完成操作。

# 5. 内核 copyout 路径

## 5.1 为什么硬件不会自动触发

`copyout()` 在内核态通过物理地址直接写用户页。内核使用自己的直接映射，不会以用户 PTE 的只读权限执行这次写入，因此不会产生用户态 store page fault。如果直接写共享页，父子进程会同时看到修改，破坏 COW 隔离。

每次跨页复制前先检查目标 PTE：

```c
va0 = PGROUNDDOWN(dstva);
if(va0 >= MAXVA)
  return -1;

pte = walk(pagetable, va0, 0);
if(pte != 0 && (*pte & PTE_COW) != 0){
  if(cowalloc(pagetable, va0) < 0)
    return -1;
}

pa0 = walkaddr(pagetable, va0);
if(pa0 == 0)
  return -1;
```

拆分完成后重新调用 `walkaddr()`，取得新物理页地址，再执行原有 `memmove()`。这样管道读取、文件读取和系统调用结果写回用户缓冲区都遵循 COW 语义。

## 5.2 非法地址边界分析

首次完整评分中，`cowtest` 全部通过，但 `usertests` 的 `copyout` 非法地址测试触发 `panic: walk`。原因是新增代码在原 `walkaddr()` 之前直接调用 `walk()`；而 `walk()` 对 `va >= MAXVA` 会 panic，原有 `walkaddr()` 则会安全返回 0。

最终实现先检查 `va0 >= MAXVA` 并返回 -1，保留原接口的错误语义。修复后 `usertests: copyin`、`usertests: copyout` 和所有回归测试全部通过。这个问题说明在插入新页表访问时，必须保持原函数的输入校验顺序和失败行为。

# 6. 实验结果与分析

## 6.1 cowtest 交互验证

在 xv6 shell 中运行 `cowtest`：

![Lab cow 交互运行结果](../screenshots/lab-cow-manual-run.png)

图 1　`cowtest` 在 xv6 中的实际输出

测试结果分析：

- `simple` 连续两次通过：进程分配超过一半物理内存后仍能 fork，说明 fork 不再立即复制全部页面；测试重复执行也说明页面最终得到释放。
- `three` 连续三次通过：三个进程对不同范围的共享页写入后内容互不干扰，且多引用页的复制和释放正确。
- `file: ok`：内核通过管道向子进程缓冲区执行 `copyout()` 时，正确拆分了 COW 页，父进程缓冲区未被覆盖。
- `ALL COW TESTS PASSED`：内存压力、隔离性和回收路径全部满足专项测试。

## 6.2 官方评分

执行 `make grade` 后，所有评分项通过，最终得分为 **110/110**：

![Lab cow 官方评分结果](../screenshots/lab-cow-make-grade.png)

图 2　官方评分脚本输出

完整 `usertests` 通过说明 COW 修改没有破坏进程创建、退出、内存增长和收缩、系统调用参数复制、文件系统或其他既有内核行为。

# 7. 兼容处理与复现验证

## 7.1 2026 工具链兼容

官方 2021 基线在当前环境中进行了以下宿主兼容处理：

- Python 新版本移除了 `pipes`，评分器改用等价的 `shlex.quote`。
- 新版 GCC 对旧版 xv6 增加递归和函数指针类型诊断，只关闭对应的两项诊断。
- 显式使用 `-march=rv64gc`，与 xv6 使用的 RISC-V 指令集扩展匹配。

这些修改不改变 COW、页表或物理页分配语义。

## 7.2 官方评分复现

在 PowerShell 中进入 WSL：

```powershell
wsl -d Ubuntu
```

然后运行：

```bash
cd /mnt/c/Users/Aemeath/Helper/2026/OS_Design/xv6-labs-2021
git switch cow
make clean
make grade
echo $?
```

通过标准是末尾出现 `Score: 110/110`，且 `echo $?` 输出 0。单独验证主要评分项可以执行：

```bash
./grade-lab-cow simple
./grade-lab-cow three
./grade-lab-cow file
./grade-lab-cow "usertests: copyout"
./grade-lab-cow "usertests: all tests"
```

现场运行：

```bash
make qemu
```

进入 xv6 shell 后输入 `cowtest`。退出 QEMU 使用 `Ctrl-a`，再按 `x`。

## 8. 实验总结

本实验将 xv6 的 eager fork 改为写时复制 fork。父子页表在 fork 时共享物理页，PTE 软件位记录 COW 状态，硬件只读保护把第一次写入转换为内核可处理的 page fault；引用计数解决共享页生命周期问题，`cowalloc()` 统一用户写故障与内核 `copyout()` 的拆分语义。

实现同时处理了真正只读页、连续 fork、多进程写入、内存不足、最后引用快速路径、TLB 刷新和非法虚拟地址。官方评分 **110/110** 且完整 `usertests` 通过，说明 COW 功能和原系统兼容性均达到实验要求。
