# xv6 Labs 2021 实验报告（三）
## Lab pgtbl: Page Tables

作者：Shalom0919  
实验分支：`pgtbl`  
本地提交：`e0666e9`  
代码仓库：[github.com/Shalom0919/xv6_labs](https://github.com/Shalom0919/xv6_labs)  
完成日期：2026-07-29  
官方评分：**46/46**

---

# 1. 实验概述

本实验基于 MIT 6.S081 Fall 2021 的 `xv6-labs-2021` 仓库 `pgtbl` 分支，通过修改进程页表和 RISC-V 页表项，理解 Sv39 三级页表、用户和内核地址空间隔离以及硬件访问位。实验包括三个部分：

- 在 `USYSCALL` 映射只读共享页，使 `ugetpid()` 无需陷入内核。
- 实现 `vmprint()`，递归输出三级页表结构。
- 实现 `pgaccess()`，读取并清除页表项的 `PTE_A` 访问位。

官方实验说明：[MIT 6.S081 - Lab: Page tables](https://pdos.csail.mit.edu/6.828/2021/labs/pgtbl.html)

## 1.1 实验环境

| 项目 | 配置 |
|---|---|
| 宿主环境 | Windows + WSL2 Ubuntu，x86_64 |
| 目标架构 | RISC-V 64，Sv39 页表 |
| 编译工具链 | `riscv64-linux-gnu-gcc` |
| 模拟器 | `qemu-system-riscv64` |
| 官方基线 | `origin/pgtbl`，提交 `1e6b2de` |
| 实验成果 | `pgtbl` 分支，提交 `e0666e9` |

## 1.2 Sv39 地址转换

Sv39 将虚拟地址划分为三级 9 位索引和 12 位页内偏移。`walk()` 从最高层页表开始，用 `PX(level, va)` 逐级定位页表项；非叶子 PTE 指向下一层页表，包含 `PTE_R`、`PTE_W` 或 `PTE_X` 的有效 PTE 则是最终映射。`PTE2PA()` 从页表项中提取物理页地址。

# 2. USYSCALL 只读共享页

## 2.1 进程共享页生命周期

在 `struct proc` 中保存每个进程独立的共享页：

```c
struct usyscall *usyscall;
```

`allocproc()` 在 PID 分配完成后申请物理页并初始化 PID。任何分配失败都调用原有 `freeproc()` 回收已经获得的资源：

```c
if((p->usyscall =
    (struct usyscall *)kalloc()) == 0){
  freeproc(p);
  release(&p->lock);
  return 0;
}
p->usyscall->pid = p->pid;
```

`freeproc()` 对应调用 `kfree()` 并将指针清零，保证进程退出和进程槽复用时不会泄漏页面。

## 2.2 建立只读用户映射

`proc_pagetable()` 在 `USYSCALL` 虚拟地址建立映射：

```c
if(mappages(pagetable, USYSCALL, PGSIZE,
            (uint64)p->usyscall,
            PTE_R | PTE_U) < 0){
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmfree(pagetable, 0);
  return 0;
}
```

权限仅包含 `PTE_R | PTE_U`：用户态可以读取，但没有 `PTE_W`，不能修改 PID。失败路径撤销已经建立的 `TRAPFRAME` 和 `TRAMPOLINE` 映射。释放页表时使用 `uvmunmap(..., 0)` 只删除映射，物理页由 `freeproc()` 统一释放。

## 2.3 加速原理与扩展分析

用户库中的 `ugetpid()` 直接读取固定地址：

```c
struct usyscall *u =
  (struct usyscall *)USYSCALL;
return u->pid;
```

普通 `getpid()` 需要执行 `ecall`、保存用户寄存器、切换页表并进入内核分派；`ugetpid()` 只执行普通内存读取。官方测试连续创建 64 个子进程，逐一比较 `getpid()` 与 `ugetpid()`，结果为 `OK`，说明每个进程都映射了自己的正确 PID。

类似方法可以加速 `uptime()`：内核在共享页维护 tick 快照，用户直接读取。由于 tick 会被中断处理程序并发更新，需要使用原子值或序列计数器，避免用户观察到不一致数据。其他频繁读取、体积小且不敏感的进程属性也可采用相同方式。

# 3. vmprint 页表打印

## 3.1 递归遍历

`vmprintwalk()` 遍历每级页表的 512 个 PTE，只打印有效项。没有读、写、执行权限的有效 PTE 是非叶子节点，需要递归访问下一层：

```c
static void
vmprintwalk(pagetable_t pagetable, int depth)
{
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) == 0)
      continue;

    for(int j = 0; j < depth; j++)
      printf(" ..");
    printf("%d: pte %p pa %p\n",
           i, pte, PTE2PA(pte));

    if((pte & (PTE_R | PTE_W | PTE_X)) == 0)
      vmprintwalk((pagetable_t)PTE2PA(pte),
                  depth + 1);
  }
}
```

入口函数先用 `%p` 打印根页表地址，再以深度 1 调用递归函数。`exec()` 完成新地址空间替换后，仅在 `p->pid == 1` 时调用 `vmprint(p->pagetable)`，因此启动过程只打印 `init` 的页表。

## 3.2 输出解释

缩进中的每个 `..` 表示深入一级页表。输出中的 `pte` 是完整页表项，`pa` 是通过 `PTE2PA()` 提取的物理地址。对 `init` 进程：

| 虚拟页 | 内容与权限 |
|---|---|
| page 0 | `init` 的程序代码和数据 |
| page 1 | 用户栈保护页，`PTE_U` 被清除，用户态不可读写 |
| page 2 | 用户栈 |
| USYSCALL | 只读 `struct usyscall`，保存 PID |
| TRAPFRAME | 仅内核使用的进程陷阱状态 |
| TRAMPOLINE | 用户态和内核态切换所需的 trampoline 代码 |

最高地址区域倒数第三页是 `USYSCALL`，其上依次为 `TRAPFRAME` 和 `TRAMPOLINE`。官方 `pte printout` 测试还验证了每行物理地址确实由对应 PTE 正确提取。

# 4. pgaccess 访问页检测

## 4.1 RISC-V 访问位

RISC-V 页表遍历硬件访问页面时会设置 PTE 的第 6 位，因此增加：

```c
#define PTE_A (1L << 6)
```

`pgaccess(base, npages, mask)` 检查从 `base` 开始的若干页面，并把第 `i` 页的访问状态放入结果整数第 `i` 位。实现将上限设为 32 页，与 `uint32` 位图相匹配。

## 4.2 系统调用实现

```c
uint64
sys_pgaccess(void)
{
  uint64 start, user_mask;
  int npages;
  uint32 mask = 0;
  struct proc *p = myproc();

  if(argaddr(0, &start) < 0 ||
     argint(1, &npages) < 0 ||
     argaddr(2, &user_mask) < 0)
    return -1;
  if(npages < 0 || npages > 32)
    return -1;

  for(int i = 0; i < npages; i++){
    pte_t *pte =
      walk(p->pagetable, start + i * PGSIZE, 0);
    if(pte == 0 || (*pte & PTE_V) == 0)
      return -1;
    if(*pte & PTE_A){
      mask |= 1U << i;
      *pte &= ~PTE_A;
    }
  }

  sfence_vma();
  if(copyout(p->pagetable, user_mask,
             (char *)&mask, sizeof(mask)) < 0)
    return -1;
  return 0;
}
```

结果先在内核变量中构造，最后通过 `copyout()` 安全写回用户空间。读取后清除 `PTE_A`，使下一次调用只反映此后发生的访问；`sfence_vma()` 使失效后的页表状态与后续地址转换保持一致。

## 4.3 结果分析

`pgtbltest` 首先调用一次 `pgaccess()` 清除旧状态，然后只访问第 1、2、30 页。第二次返回的位图必须等于 `(1 << 1) | (1 << 2) | (1 << 30)`。测试结果为 `pgaccess_test: OK`，证明位序、页间步长、访问位读取和清除均正确。

# 5. 运行与评分结果

## 5.1 xv6 内手工验证

启动 xv6 时成功输出 `init` 的三级页表；执行 `pgtbltest` 后，`ugetpid_test` 和 `pgaccess_test` 均为 `OK`。

![Lab pgtbl 手工运行结果](../screenshots/lab-pgtbl-manual-run.png)

## 5.2 官方评分

执行 `make grade` 后，功能测试、页表格式、问答文件、完整 `usertests` 和时间文件全部通过，最终得分为 **46/46**。

![Lab pgtbl 官方评分结果](../screenshots/lab-pgtbl-make-grade.png)

# 6. 兼容处理、复现与总结

## 6.1 2026 工具链兼容

实验逻辑之外进行了三项宿主工具兼容处理：

- Python 3.13 及以上删除了 `pipes`，评分器改用等价的 `shlex.quote`。
- 新版 GCC 对官方基础代码新增递归和函数指针类型诊断，只关闭 `-Winfinite-recursion` 与 `-Wincompatible-pointer-types` 两项诊断。
- 显式使用 `-march=rv64gc`，与 xv6 2021 使用的 RISC-V ISA 扩展相匹配。

这些修改不改变页表映射、访问位或 xv6 原有运行语义。

## 6.2 复现与评分验证

```powershell
wsl -d Ubuntu
```

```bash
cd /mnt/c/Users/Aemeath/Helper/2026/OS_Design/xv6-labs-2021
git switch pgtbl
make clean
make grade
echo $?
```

通过标准是末尾出现 `Score: 46/46`，并且 `echo $?` 输出 `0`。单独验证主要功能可执行：

```bash
./grade-lab-pgtbl pgtbltest
./grade-lab-pgtbl "pte printout"
```
![1](image-2.png)
## 6.3 实验总结

本实验完成了共享只读页、页表递归打印和访问位检测。实现覆盖了物理页生命周期、页表建立与回滚、用户权限控制、三级页表递归、PTE 位操作和安全用户内存复制。官方评分 **46/46** 且完整 `usertests` 通过，说明新增功能未破坏 xv6 原有内存管理和进程行为。
