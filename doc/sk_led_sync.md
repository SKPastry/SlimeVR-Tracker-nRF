# SK 私有呼吸灯同步

`CONFIG_SK_LED_SYNC` 默认仅在 CheeseCake P00/P10 开启。它复用接收器的
PONG 时钟，让同一接收器下的相同呼吸灯效保持相位一致，不增加无线报文。

| 灯效 | 开启同步 | 关闭同步 |
| --- | --- | --- |
| `PULSE_PERSIST`、`BREATH_SLOW` | 4 秒 | 原有本地 5 秒 |
| `BREATH_FAST` | 2 秒 | 原有本地 2 秒 |

颜色、亮度、优先级、其他闪灯、线程休眠以及灯带的 20 ms 更新限制均沿用
现有实现。每次刷新按公共时间重新计算亮度，提示灯打断后也会回到当前相位。

## 接入点与接口

- `src/system/sk_led_sync.c/.h`：私有模块，周期和相位计算集中在这里。
  `sk_led_sync_phase()` 只由 LED 线程调用，仅保存一个 32 位时钟偏移。
- `src/system/led.c`：三种呼吸灯各调用一次相位 helper。关闭配置时，header
  中的 inline helper 执行原来的 `(state + 1) % 1000`，不读取 ESB 时钟。
- `src/connection/esb.c`：一个受配置开关保护的 `sk_led_sync_read_clock()`
  函数块。它在短临界区内快照时间和同步参数，解锁后计算，不改写 ESB 状态。
- `Kconfig`：一个独立的 `SK_LED_SYNC` 开关。源文件沿用现有自动收集机制。

时钟接口始终提供同一次采样的本机 ticks；同步有效时还提供网络 ticks。
二者单位均为 1/32768 秒，按模 `2^32` 计算；网络时间零值有效。
未初始化、正在配对、没有有效同步记录、记录来自未来或超过现有 15 秒
有效期时，标记未同步。接口针对单核 LED 线程调用，不提供 ISR/SMP 调用保证。

尚未同步时按本机时钟呼吸；同步后保存 `network_ticks - local_ticks`。
失联时保留这个偏移，以绝对本机 ticks 续播；重连后下一次刷新立即对齐，
允许一次亮度跳变。4 秒和 2 秒周期整除 `2^32`，不受无线时钟回绕影响。

不要改用现有毫秒转换或 64 位网络时间接口：本功能刻意使用低 32 位模运算，
不猜测接收器的计时高位。已有 TDMA 64 位计时问题应独立修复，本功能不修复它。

## 验证

在本 tracker 仓库根目录运行主机测试（不需要 NCS SDK）：

```sh
make -C tests/host/sk_led_sync check
```

测试编译实际私有模块并提供可控时钟，覆盖晚加入、周期、零值、回绕、
不规则刷新、断线续播、重连以及关闭开关后的原有行为。

固件构建前激活与 `west.yml` 一致的 NCS SDK 和工具链。从本仓库根目录运行：

```sh
west build --sysbuild -p always -b sk_cheesecake_nrf_p00/nrf52840/uf2 -d build/led-sync-p00 -- -DBOARD_ROOT="$PWD"
west build --sysbuild -p always -b sk_cheesecake_nrf_p00/nrf52840/uf2 -d build/led-local-p00 -- -DBOARD_ROOT="$PWD" -DCONFIG_SK_LED_SYNC=n
west build --sysbuild -p always -b sk_cheesecake_nrf_p10/nrf52840/uf2 -d build/led-sync-p10 -- -DBOARD_ROOT="$PWD"
west build --sysbuild -p always -b sk_cheesecake_nrf_p10/nrf52840/uf2 -d build/led-local-p10 -- -DBOARD_ROOT="$PWD" -DCONFIG_SK_LED_SYNC=n
west build --sysbuild -p always -b nrf52840dk/nrf52840 -d build/led-default-dk -- -DBOARD_ROOT="$PWD"
```

同时检查 P00/P10 不带覆盖参数时默认开启，其他板型默认关闭。旧 SDK 若出现
`SPI_DT_SPEC_GET` 参数数量错误，属于环境不匹配，不应为构建验证修改 SPI 宏。

本次验证记录：主机测试的 74 个同步相位检查及 2001 步关闭配置检查通过；
上述五组固件均完成构建，生成的 `.config` 符合预期。使用隔离的 NCS 3.3.4
源码（nrf `437914c07dc1`、Zephyr `37e6c28576ee`）和 Zephyr SDK 0.17.0
GCC 12.2.0，未改动原有旧 SDK 或 SPI 宏。尚未完成实机验收。

实机使用至少两台 tracker 错开启动，比较呼吸峰谷，并覆盖充电颜色变化、
校准快慢切换、按键提示打断、断线超过 15 秒和接收器重启。记录可见时间差；
时钟 tick 分辨率不是 LED 同步精度保证，刷新和调度延迟也影响结果。

## 后续合并上游

保留为独立的下游提交，暂不向上游提交功能。合并时核对三处 LED 接入点、
同步参数的含义及写入上下文、网络 tick 单位和有效期，再运行上述测试。
关闭 `CONFIG_SK_LED_SYNC` 可恢复原有行为，不需要迁移配对或存储数据。
