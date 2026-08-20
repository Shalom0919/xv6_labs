# xv6 Labs 2021 实验报告（十）
## Lab mmap: Memory-mapped Files

作者：Shalom0919  
实验分支：`mmap`  
本地提交：`a69ed4e`  
代码仓库：[github.com/Shalom0919/xv6_labs](https://github.com/Shalom0919/xv6_labs)  
完成日期：2026-08-17  
官方评分：**140/140**

---

# 1. 实验概述

本实验基于 MIT 6.S081 Fall 2021 的 `xv6-labs-2021` 仓库 `mmap` 分支，为 xv6 增加文件支持的 `mmap()` 和 `munmap()`。实现允许进程把文件内容映射到虚拟地址空间，通过缺页异常按需载入页面，并根据 `MAP_SHARED` 或 `MAP_PRIVATE` 决定解除映射时是否写回文件。

主要目标如下：

- 每个进程维护 VMA 表；`mmap()` 只登记地址范围和文件引用，不提前分配物理页。
- 首次访问映射页时，由缺页处理分配页面并从正确的文件偏移读入数据。
- `munmap()` 支持整体、开头或结尾解除映射，并对共享可写页执行日志化写回。
- 在 `fork()`、`exit()` 和 `exec()` 中正确复制或释放 VMA 与文件引用。

官方实验说明：[MIT 6.S081 - Lab: mmap](https://pdos.csail.mit.edu/6.828/2021/labs/mmap.html)

## 1.1 实验环境

| 项目 | 配置 |
|---|---|
| 宿主环境 | Windows + WSL2 Ubuntu，x86_64 |
| 客体系统 | xv6-riscv，RISC-V 64 |
| 页大小 | 4096 bytes |
| 编译工具链 | `riscv64-linux-gnu-gcc` |
| 官方基线 | `origin/mmap`，提交 `2255a4c` |
| 实验成果 | `mmap` 分支，提交 `a69ed4e` |
| 官方评分 | `make grade`，140/140 |

## 1.2 修改文件

| 文件 | 主要作用 |
|---|---|
| `kernel/proc.h` | 定义 VMA 并加入进程结构 |
| `kernel/proc.c` | 映射、缺页载入、写回、解除映射与生命周期管理 |
| `kernel/trap.c` | 将用户页异常交给 VMA 缺页处理 |
| `kernel/sysfile.c`、`kernel/exec.c` | 实现调用入口并在程序替换前释放旧映射 |
| 系统调用与用户接口文件 | 注册调用号、分发表和用户桩函数 |
| `Makefile`、`gradelib.py` | 构建 `mmaptest` 并兼容当前工具链 |

# 2. VMA 数据结构与地址布局

## 2.1 每进程 VMA 表

每个进程最多维护 16 个映射区域。VMA 只保存描述信息，物理页是否存在由页表决定：

```c
#define NVMA 16

struct vma {
  int valid;
  uint64 addr;
  uint64 length;
  int prot;
  int flags;
  struct file *file;
  uint64 offset;
};

struct proc {
  ...
  struct vma vmas[NVMA];
};
```

`file` 保存经过 `filedup()` 增加引用计数后的文件对象。因此用户在 `mmap()` 后关闭原文件描述符，映射仍然可以继续缺页读取和写回；文件被 `unlink()` 后，inode 也会因 VMA 引用继续存活。

## 2.2 高地址向下分配

普通程序映像、栈和 heap 从低地址向上增长。映射区从 `TRAPFRAME` 下方按页向低地址分配：

| 地址方向 | 区域 |
|---|---|
| 高地址 | `TRAMPOLINE`、`TRAPFRAME` |
| 向下增长 | mmap VMA 区域 |
| 未使用间隔 | 防止 mmap 与 heap 相撞 |
| 向上增长 | heap、用户栈、程序段 |

每次映射取所有有效 VMA 的最低起始地址作为上界，再减去页对齐后的映射长度。`growproc()` 同时检查新的 heap 末端不得越过最低 VMA。这一布局不把懒映射空洞计入 `p->sz`，因此原有 `uvmcopy()` 和普通地址空间释放逻辑不需要遍历未映射页。

# 3. mmap() 与懒分配

## 3.1 参数与权限检查

系统调用入口读取六个参数，并限制为实验要求的子集：`addr` 和 `offset` 必须为 0，长度必须为正，flags 必须恰好为 `MAP_SHARED` 或 `MAP_PRIVATE`。任何映射都需要读取原始文件；共享可写映射还要求文件以可写方式打开。

```c
if(addr != 0 || length <= 0 || offset != 0)
  return -1;

return vmamap(myproc(), length, prot, flags, f, offset);
```

`MAP_PRIVATE | PROT_WRITE` 可以映射只读打开的文件，因为页面修改不会回写；`MAP_SHARED | PROT_WRITE` 若文件不可写则立即失败。这由官方 read-only 与 private 测试覆盖。

## 3.2 只登记，不读取

`vmamap()` 找到空闲 VMA 和无冲突地址后，仅填写元数据并执行 `filedup()`：

```c
v->valid = 1;
v->addr = addr;
v->length = PGROUNDUP(length);
v->prot = prot;
v->flags = flags;
v->file = filedup(f);
v->offset = offset;
return addr;
```

此时用户页表中没有对应 PTE，也没有分配物理内存。大文件映射的建立成本与文件大小无关，只有实际访问的页面才占用内存。这同时允许映射长度大于当前物理内存。

# 4. 缺页处理与文件载入

## 4.1 trap 路由

RISC-V 的 instruction、load 和 store page fault 分别使用 `scause` 12、13、15。`usertrap()` 将这三类异常交给 `vmafault()`；只有异常地址位于有效 VMA 且访问类型符合权限时才恢复执行，否则按非法用户访问终止进程。

```c
} else if((r_scause() == 12 || r_scause() == 13 ||
           r_scause() == 15) &&
          vmafault(p, r_stval(), r_scause()) == 0){
  // A file-backed VMA page was populated lazily.
}
```

权限检查发生在分配之前。例如对只读映射执行 store 会返回失败，而不是把页面重新映射为可写。若对应 PTE 已有效但仍产生权限异常，也直接失败，避免 `mappages()` 的 remap panic。

## 4.2 分配、读取和映射

缺页地址先向下按页对齐，然后分配并清零一页。文件偏移由 VMA 起始偏移加上映射内页偏移得到：

```c
va = PGROUNDDOWN(faultva);
mem = kalloc();
memset(mem, 0, PGSIZE);

fileoff = v->offset + (va - v->addr);
ilock(v->file->ip);
n = readi(v->file->ip, 0, (uint64)mem, fileoff, PGSIZE);
iunlock(v->file->ip);
```

文件末尾不足一页时，`readi()` 只读取存在的字节，其余部分保留为 0。PTE 权限由 `prot` 转换为 `PTE_R`、`PTE_W`、`PTE_X` 和 `PTE_U`。RISC-V 不允许 write-only 叶 PTE，因此可写页同时设置 `PTE_R`。

若读取或页表映射失败，代码释放刚分配的物理页并让进程收到访问失败，不留下孤立页面。

# 5. munmap() 与共享写回

## 5.1 部分解除映射

`vmaunmap()` 验证目标范围属于同一 VMA，并且从区域开头、结尾或整体解除映射。它逐页检查页表，只处理已经因缺页访问而存在的 PTE；从未访问的懒页面无需调用 `uvmunmap()`，从而避免原函数对不存在映射的 panic。

整体解除映射时执行 `fileclose()` 并清空 VMA。若只移除开头，则同时增加 `addr` 和文件 `offset`；若只移除结尾，则缩短 `length`：

```c
if(addr == v->addr && span == v->length){
  fileclose(v->file);
  memset(v, 0, sizeof(*v));
} else if(addr == v->addr){
  v->addr += span;
  v->offset += span;
  v->length -= span;
} else {
  v->length -= span;
}
```

更新 offset 是开头解除映射的关键：剩余区域的新起始页仍必须对应原文件中向后移动相同长度的位置。

## 5.2 MAP_SHARED 写回

对于已经映射的 `MAP_SHARED | PROT_WRITE` 页面，解除映射前把物理页内容写回 VMA 对应的文件偏移。`MAP_PRIVATE` 页面直接释放，不修改文件。

单页大小为 4096 bytes，可能超过一次 xv6 日志事务允许的安全写入量。实现复用 `filewrite()` 的上限公式，把页面拆分为最多 3072 bytes 的事务：每段分别执行 `begin_op()`、inode 锁、`writei(user_src=0)` 和 `end_op()`。这样既不修改共享 `struct file` 的 `off`，又不会耗尽日志空间。

# 6. fork、exit 与 exec 生命周期

## 6.1 fork 继承

`fork()` 在复制普通低地址内存后复制所有有效 VMA，并对每个文件执行 `filedup()`：

```c
for(i = 0; i < NVMA; i++){
  if(p->vmas[i].valid){
    np->vmas[i] = p->vmas[i];
    np->vmas[i].file = filedup(p->vmas[i].file);
  }
}
```

映射页位于高地址，不在 `p->sz` 范围内，因此不会被 `uvmcopy()` 立即复制。子进程第一次访问时独立分配物理页并从同一文件读取。实验明确允许父子进程不共享同一个物理页，这种设计保持实现简单，同时满足 `fork_test`。

## 6.2 退出和程序替换

`exit()` 在关闭普通文件描述符前调用 `vmaunmapall()`，依次写回共享映射、释放物理页并减少 VMA 文件引用。父进程之后执行 `wait()` 并最终释放页表时，不会遇到遗留的高地址叶 PTE。

`exec()` 成功构造新程序页表后、替换旧页表前同样调用 `vmaunmapall()`。这补充了官方基础测试之外的生命周期路径，避免带着映射执行 `exec()` 时泄漏文件引用或物理页。若新程序构造失败，旧映射保持不变。

# 7. 实验结果、复现与总结

## 7.1 官方评分结果

![Lab mmap 官方评分结果](../screenshots/lab-mmap-make-grade.png)

图 1　官方 `grade-lab-mmap` 评分结果

| 测试组 | 得分 | 结果与覆盖内容 |
|---|---:|---|
| 基础文件映射 | 20/20 | 懒加载文件内容正确 |
| private、read-only、read/write | 30/30 | 私有修改和访问权限正确 |
| dirty、partial unmap、two files | 30/30 | 共享写回、部分解除和多 VMA 正确 |
| `fork_test` | 40/40 | 子进程继承映射和文件引用正确 |
| `usertests` | 19/19 | 108.2 秒，完整回归通过 |
| `time.txt` | 1/1 | 格式符合要求 |
| 合计 | 140/140 | 官方评分脚本退出码为 0 |

所有 `mmaptest` 子项在 6.3 秒内完成。完整 `usertests` 通过，说明新加入的高地址映射、缺页异常分支、退出清理和 heap 边界检查没有破坏原有进程、内存与文件系统行为。

## 7.2 官方评分复现

```sh
wsl -d Ubuntu
cd /mnt/c/Users/Aemeath/Helper/2026/OS_Design/xv6-labs-2021
git switch mmap
make clean
make grade
echo $?
```

通过标准是最后输出 `Score: 140/140`，并且 `echo $?` 输出 `0`。手工运行时可以执行 `make qemu`，进入 xv6 shell 后运行 `mmaptest`；最终应输出 `mmaptest: all tests succeeded`。

## 7.3 实验总结

本实验把虚拟内存、异常处理、文件系统和进程生命周期连接成一条完整路径。VMA 是逻辑承诺，页表 PTE 是实际驻留状态；`mmap()` 建立承诺，缺页异常按需兑现，`munmap()` 负责写回和释放。正确性不仅取决于单次映射，还取决于文件关闭、部分解除、`fork()`、`exit()` 和 `exec()` 后引用计数与页表状态始终匹配。最终所有专项测试和回归测试通过，官方得分 140/140。
