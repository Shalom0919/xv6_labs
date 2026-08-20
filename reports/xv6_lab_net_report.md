# xv6 Labs 2021 实验报告（七）
## Lab net: Network Driver

作者：Shalom0919  
实验分支：`net`  
本地提交：`a611d08`  
代码仓库：[github.com/Shalom0919/xv6_labs](https://github.com/Shalom0919/xv6_labs)  
完成日期：2026-08-17  
官方评分：**100/100**

---

# 1. 实验概述

本实验基于 MIT 6.S081 Fall 2021 的 `xv6-labs-2021` 仓库 `net` 分支，为 QEMU 模拟的 Intel E1000 网卡补全发送与接收驱动。xv6 已提供 PCI 发现、E1000 初始化、mbuf 管理以及 Ethernet、ARP、IP、UDP、DNS 协议处理；实验任务集中在 `kernel/e1000.c` 的 `e1000_transmit()` 和 `e1000_recv()`。

驱动通过内存映射寄存器与网卡交互，通过 DMA 描述符环传递数据：

- 发送路径把 mbuf 数据地址写入 TX descriptor，并推进 `E1000_TDT` 通知设备。
- 接收路径从 RX descriptor 取出已由设备填充的 mbuf，补充长度后交给 `net_rx()`。
- 描述符环大小固定为 16，索引必须按模运算循环使用。
- TX mbuf 只能在硬件设置 Descriptor Done 后回收；RX mbuf 交给协议栈后必须立即为设备换入新缓冲区。
- 发送可能来自多个进程，接收由中断触发，因此共享环和寄存器需要自旋锁保护。

官方实验说明：[MIT 6.S081 - Lab: networking](https://pdos.csail.mit.edu/6.828/2021/labs/net.html)

## 1.1 实验环境

| 项目 | 配置 |
|---|---|
| 宿主环境 | Windows + WSL2 Ubuntu，x86_64 |
| 客体系统 | xv6-riscv，RISC-V 64 |
| 模拟器 | QEMU 10.2.1，`virt` 机器与 E1000 设备 |
| 编译工具链 | `riscv64-linux-gnu-gcc` |
| 网络模式 | QEMU user-mode network，xv6 地址 10.0.2.15 |
| 官方基线 | `origin/net`，提交 `1bd9c80` |
| 实验成果 | `net` 分支，提交 `a611d08` |

## 1.2 修改文件

| 文件 | 作用 |
|---|---|
| `kernel/e1000.c` | 实现 TX/RX 描述符环和并发控制 |
| `Makefile` | 适配 RISC-V 工具链与新版 QEMU E1000 ROM 行为 |
| `gradelib.py` | 适配 Python 3 的命令引用函数 |
| `time.txt` | 记录实验用时 |

# 2. E1000 与 DMA 描述符环

## 2.1 内存映射寄存器

PCI 初始化把 E1000 的寄存器区域映射到内核地址空间，`regs` 是该区域的基址。驱动按 32 位寄存器数组访问设备，其中本实验最关键的寄存器是：

| 寄存器 | 含义 | 驱动使用方式 |
|---|---|---|
| `E1000_TDT` | Transmit Descriptor Tail | 读取待填 TX 项；提交后推进一项 |
| `E1000_RDT` | Receive Descriptor Tail | 从下一项取包；换入新缓冲后推进 |
| `E1000_ICR` | Interrupt Cause Read | 中断入口写入以确认中断 |
| `E1000_IMS` | Interrupt Mask Set | 初始化时启用接收完成中断 |

初始化函数已经配置 TX/RX 环基址、长度、收发控制位和 QEMU 网卡 MAC 地址。TX 环初始每个 descriptor 均带 `E1000_TXD_STAT_DD`，表示可立即使用；RX 环的每个 descriptor 则预先绑定一个空 mbuf，供设备 DMA 写入。

## 2.2 descriptor 所有权

TX 环在软件与设备之间按以下顺序转移所有权：

1. 软件读取 `TDT` 并确认该项 `DD=1`。
2. 软件回收这个 descriptor 上一次保存的 mbuf。
3. 软件写入新数据地址、长度和命令，清除完成状态。
4. 软件推进 `TDT`，设备获得该项并执行 DMA 读取。
5. 发送完成后设备写回 `DD=1`，软件以后才能再次使用。

RX 环的方向相反：设备在可用 descriptor 指向的 mbuf 中 DMA 写入包，记录长度并设置 `DD`；软件看到 `DD` 后取得旧 mbuf，给 descriptor 换入新的空 mbuf并清零状态，再推进 `RDT` 把该项归还设备。

环索引均使用 `(index + 1) % RING_SIZE`，从第 15 项正确回绕到第 0 项。官方多进程测试会收发远多于 16 个包，因此一次性线性处理无法通过。

## 2.3 内存可见性

描述符与数据均由 CPU 和设备共享。软件必须先完成 descriptor 字段写入，再更新 tail 寄存器。实现使用 `__sync_synchronize()` 放在 tail 更新之前，防止编译器或处理器把 MMIO 通知重排到描述符初始化之前；否则设备可能观察到尚未完整填写的项。

# 3. 发送路径实现

`e1000_transmit()` 的输入 mbuf 已包含完整 Ethernet frame。实现首先在锁内读取硬件期望的 `TDT` 位置，并检查该 descriptor 是否完成：

```c
acquire(&e1000_lock);

index = regs[E1000_TDT];
desc = &tx_ring[index];
if((desc->status & E1000_TXD_STAT_DD) == 0){
  release(&e1000_lock);
  return -1;
}
```

若 `DD` 未设置，说明环已满或硬件还没有发送完该项。函数返回 `-1`，但不释放传入的 mbuf；调用者 `net_tx_eth()` 根据失败返回值执行 `mbuffree()`，从而保持清晰的所有权约定。

descriptor 可用时，先回收它上一次保存的 mbuf，再提交新包：

```c
if(tx_mbufs[index])
  mbuffree(tx_mbufs[index]);

tx_mbufs[index] = m;
desc->addr = (uint64)m->head;
desc->length = m->len;
desc->cmd = E1000_TXD_CMD_EOP | E1000_TXD_CMD_RS;
desc->status = 0;

__sync_synchronize();
regs[E1000_TDT] = (index + 1) % TX_RING_SIZE;
```

`EOP` 表示当前 descriptor 是该帧最后一段，本实验每个包只占一个 descriptor；`RS` 要求设备发送后写回状态，使软件能通过 `DD` 判断完成。mbuf 指针保存在 `tx_mbufs[index]`，不能在提交时立即释放，因为 DMA 可能仍在读取其数据。

锁保护 `TDT`、TX ring 和 `tx_mbufs` 的复合更新。如果两个进程同时发送而没有锁，它们可能读到同一个 `TDT`，后一次写入覆盖前一次 descriptor，造成丢包或 mbuf 生命周期错误。

# 4. 接收路径实现

## 4.1 扫描接收环

接收中断调用 `e1000_recv()`。驱动从 `RDT` 后一项开始扫描，只要 descriptor 的 `DD` 仍为 1 就继续处理：

```c
for(;;){
  acquire(&e1000_lock);
  index = (regs[E1000_RDT] + 1) % RX_RING_SIZE;
  desc = &rx_ring[index];
  if((desc->status & E1000_RXD_STAT_DD) == 0){
    release(&e1000_lock);
    break;
  }
  /* 取包、补充 descriptor、推进 RDT */
}
```

一次中断可能对应多个已完成 descriptor，因此不能只处理一包。循环直到下一项没有 `DD`，可以把一次突发接收中积累的所有包交给协议栈。

## 4.2 换入新缓冲区

已接收 mbuf 的 `len` 初始为 0，设备实际接收长度记录在 descriptor 中。驱动先分配 replacement，再交换所有权：

```c
replacement = mbufalloc(0);
if(replacement == 0){
  release(&e1000_lock);
  break;
}

m = rx_mbufs[index];
m->len = desc->length;
rx_mbufs[index] = replacement;
desc->addr = (uint64)replacement->head;
desc->status = 0;

__sync_synchronize();
regs[E1000_RDT] = index;
```

先成功分配 replacement，再修改 descriptor，可保证内存不足时旧包和硬件状态仍保持一致。清除状态后推进 `RDT`，设备以后回绕到该项时可把下一包写入新的 mbuf。

## 4.3 锁外调用协议栈

完成环状态更新后释放 `e1000_lock`，再执行 `net_rx(m)`。这是并发正确性的关键：接收到 ARP request 时，`net_rx()` 会构造 ARP reply 并调用 `e1000_transmit()`；如果仍持有同一把锁，当前 CPU 会再次申请 `e1000_lock`，形成不可恢复的自锁。

因此锁只覆盖设备共享状态，不覆盖协议处理。descriptor 已在调用 `net_rx()` 前换入新缓冲并归还设备，释放锁后其他 CPU 或下一次中断也不会重复处理同一包。

# 5. 实验结果与分析

## 5.1 nettests 运行结果

官方评分启动宿主 UDP server，并在 xv6 内运行 `nettests`。实际输出如下：

![Lab net 网络测试结果](../screenshots/lab-net-manual-run.png)

图 1　xv6 中 ping、并发 UDP 与 DNS 测试结果

测试覆盖单次 UDP 往返、同一进程连续收发、多个进程并发收发以及经 QEMU user-mode network 访问外部 DNS。DNS 成功解析 `pdos.csail.mit.edu`，说明发送、接收、ARP 响应、IP/UDP 处理和中断驱动流程均能持续工作。

生成的 `packets.pcap` 中可以检索到 `a message from xv6!` 与 `this is the host!`，证明 guest 到 host 和 host 到 guest 两个方向的 UDP 数据均被捕获；多次重复消息也验证 descriptor 环在超过 16 包后可以正确回绕。

## 5.2 官方评分

![Lab net 官方评分结果](../screenshots/lab-net-make-grade.png)

图 2　官方 `grade-lab-net` 评分结果

各项得分为：Ping 40/40、Single process 20/20、Multi-process 20/20、DNS 19/19、Time 1/1，合计 **100/100**。

# 6. 兼容性、复现与总结

## 6.1 本地环境兼容处理

本机 QEMU 10.2.1 会为 E1000 尝试加载默认的 `efi-e1000.rom`，而 WSL 安装中未提供该 ROM，导致最初评分在启动 QEMU 前失败：

```text
failed to find romfile "efi-e1000.rom"
```

xv6 使用 `-bios none`，实验也不通过网卡启动，因此该 option ROM 完全不参与驱动功能。Makefile 将设备参数改为：

```make
-device e1000,netdev=net0,bus=pcie.0,romfile=
```

显式禁用 ROM 后，QEMU 正常创建设备并通过全部测试。另补充 `rv64gc` 架构参数、当前 GCC 的警告兼容选项，并把 Python 3 已移除的 `pipes.quote` 替换成 `shlex.quote`；这些修改不改变实验算法或评分逻辑。

## 6.2 官方评分复现

```sh
wsl -d Ubuntu
cd /mnt/c/Users/Aemeath/Helper/2026/OS_Design/xv6-labs-2021
git switch net
make clean
make grade
echo $?
```

通过标准是最后输出 `Score: 100/100`，且退出码为 0。若要手工观察运行过程，可分别启动宿主 server 和 xv6：

```sh
# 终端一
make server

# 终端二
make qemu
# 在 xv6 shell 中执行：nettests
```

## 6.3 实验总结

本实验完成了 E1000 的 TX/RX DMA 描述符环驱动。实现通过 `DD` 位明确 CPU 与设备的所有权，用 tail 寄存器提交或归还 descriptor，用 mbuf 数组保证 DMA 期间的数据生命周期，并以自旋锁保护多进程与中断并发。接收路径在锁外调用协议栈，避免 ARP 回复重入发送路径造成死锁。最终 ping、单进程、多进程、DNS 和时间测试全部通过，官方得分 100/100。
