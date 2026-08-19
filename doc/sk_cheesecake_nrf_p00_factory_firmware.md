# SK CheeseCake NRF P00 工厂合并固件

本文记录如何把 `sk_cheesecake_nrf_p00` 的 Bootloader 和 App 合并为一个可通过
SWD/J-Link 一次烧录的 Intel HEX 文件。本文只描述产物合并，不改变 Bootloader、
App、分区或现有更新行为。

## 适用配置

| 部分 | 配置/板级目录 | 本次使用的构建产物 |
| --- | --- | --- |
| Bootloader | `boot/Adafruit_nRF52_Bootloader/src/boards/sk_cheesecake_nrf_p00` | `boot/Adafruit_nRF52_Bootloader/cmake-build-sk_cheesecake_nrf_p00/bootloader_mbr.hex` |
| App | `tracker/SlimeVR-Tracker-nRF/boards/crazt/sk_cheesecake_nrf_p00` | `tracker/SlimeVR-Tracker-nRF/build_sk_ck_p00/SlimeVR-Tracker-nRF/zephyr/zephyr.hex` |

下文命令均从同时包含 `boot/` 和 `tracker/` 的工作区根目录执行。合并脚本使用
Zephyr 自带的 `tracker/zephyr/scripts/build/mergehex.py`，Python 环境必须安装
`intelhex`；当前工作区可直接使用 `.venv-bootloader/bin/python`。

## 为什么输出是 HEX

工厂产物必须是 Intel HEX，而不是把两个 UF2 文件直接拼接：

- 空白 nRF52840 尚无 UF2 Bootloader，不能自行解析通过 USB 写入的 UF2。
- App UF2 和 Bootloader UF2 有各自的块序号、块总数和 family ID，直接连接两个
  文件不会生成一个合法的单次烧录镜像。
- App UF2 用于更新 App，Bootloader UF2 用于 Bootloader 自升级；二者仍作为独立
  产物使用。
- factory HEX 用于 SWD/J-Link 编程器对空片或整片擦除后的芯片进行首次烧录。

不要使用 `bootloader.bin` 作为输入。Bootloader HEX 同时包含内部 Flash 和高地址
UICR 数据，将其转换成从地址 0 开始的平坦 BIN 会因为中间地址空洞而生成约 256 MiB
的无效大文件。正确输入是 `bootloader_mbr.hex`。

## 地址布局与兼容性

| 地址范围 | 用途 | factory HEX 中的数据 |
| --- | --- | --- |
| `0x00000000–0x00000AFF` | MBR | 来自 `bootloader_mbr.hex` |
| `0x00000B00–0x00000FFF` | MBR 保留区空洞 | 无 |
| `0x00001000–0x00058FA7` | 当前 App 实际数据 | 来自 `zephyr.hex` |
| `0x000E0000–0x000E9FFF` | BL UF2 自升级暂存区 | 必须为空 |
| `0x000EE000–0x000F3FFF` | NVS | 必须为空 |
| `0x000F4000–0x000FD857` | 当前 Bootloader 实际数据 | 来自 `bootloader_mbr.hex` |
| `0x000FE000–0x000FEFFF` | MBR 参数页 | 本产物不写入 |
| `0x000FF000–0x000FFFFF` | Bootloader settings 页 | 本产物不写入 |
| `0x10001014–0x1000101B` | UICR Bootloader/MBR 参数地址 | 来自 `bootloader_mbr.hex` |

App 的静态分区虽然延伸到 `0xEE000`，但当前 Bootloader 的自升级暂存区从
`0xE0000` 开始。为了确保以后仍能安全执行 BL UF2 自升级，本流程要求 App 的实际
数据严格结束在 `0xE0000` 之前。当前 App 的最后一个数据字节是 `0x58FA7`，余量充足。
如果将来 App 增长到该边界，必须先重新评估分区和 BL 自升级方案，不能继续无条件
合并。

当前 App 向量表位于 `0x1000`：

- 初始栈指针：`0x20013180`
- Reset Handler：`0x0003D421`

UICR 中的两个小端 32 位值为：

- `0x10001014`：Bootloader 地址 `0x000F4000`
- `0x10001018`：MBR 参数页地址 `0x000FE000`

factory HEX 逐字节保留现有 Bootloader，因此不会改变 BL UF2 自升级的 family
校验、暂存、MBR Copy-BL 或 UICR 行为。App USB UF2 和 ESB App OTA 流程也保持
不变。本次合并未给镜像增加 SoftDevice。

当前 BL UF2 的板型校验沿用 USB VID/PID，而 P00 暂用的 `0x239A:0x0029` 并非
该板独占。因此应只向设备提供明确由 P00 配置生成的 BL UF2，不能只凭 VID/PID
选择更新文件。这是现有 BL 更新流程的限制，factory HEX 合并既不引入也不修复该
问题；正式量产前应单独决定是否分配唯一标识。

## CUSTOMER 启动诊断

P00/P10 Tracker 从 `0.1.1.2` 开始会在正常 App 启动时只读快照 nRF52840 UICR
CUSTOMER 槽 A/B，并解析当前工程样例 `SKT0/schema 1`。功能不会启用 NVMC，也不会
写入、纠正或擦除 UICR。RTT 启动日志、USB 控制台连接横幅和交互式 `info` 命令会
输出同一份缓存结果。

- 普通 `factory-test.hex` 不含 CUSTOMER，设备应报告 A/B 均为擦除态；
- `factory-SAMPLE.hex` 只写槽 A，记录和板型匹配时会输出产品、硬件版本、区域、
  批次、生产日期、出厂 App 版本、provenance 前缀和 CRC；
- 旧 schema、未知格式、字段或 CRC 损坏、板型身份不匹配及槽 B 意外非空只会产生
  明确告警，不得阻止后续启动；
- 槽 B 当前必须全为 `0xFF`，固件不会把它作为回退记录解析。

无效记录中的字符串不会作为身份输出。CRC-32/ISO-HDLC 仅用于检测意外损坏，不是
签名，不能证明记录来源、阻止克隆或提供区域/功能授权。`CUSTOMER:` 日志是人工诊断
文本，不是冻结的生产测试机器接口。

## 生成 factory HEX

先确认输入确实是预期的测试构建：

```bash
sha256sum \
  boot/Adafruit_nRF52_Bootloader/cmake-build-sk_cheesecake_nrf_p00/bootloader_mbr.hex \
  tracker/SlimeVR-Tracker-nRF/build_sk_ck_p00/SlimeVR-Tracker-nRF/zephyr/zephyr.hex
```

本次参考输入应得到：

```text
e04b9d8a0f4c36957bcdd4ccbc05b2a1f94604bb8cf9bfa0e253ad711d958d9b  boot/Adafruit_nRF52_Bootloader/cmake-build-sk_cheesecake_nrf_p00/bootloader_mbr.hex
74cc30620970d470c3956870520ffc45b4c405b40a373bdc02560afd62435854  tracker/SlimeVR-Tracker-nRF/build_sk_ck_p00/SlimeVR-Tracker-nRF/zephyr/zephyr.hex
```

创建输出目录并合并：

```bash
mkdir -p artifacts

.venv-bootloader/bin/python \
  tracker/zephyr/scripts/build/mergehex.py \
  --overlap=error \
  -o artifacts/sk_cheesecake_nrf_p00_factory_test.hex \
  boot/Adafruit_nRF52_Bootloader/cmake-build-sk_cheesecake_nrf_p00/bootloader_mbr.hex \
  tracker/SlimeVR-Tracker-nRF/build_sk_ck_p00/SlimeVR-Tracker-nRF/zephyr/zephyr.hex

sha256sum artifacts/sk_cheesecake_nrf_p00_factory_test.hex \
  > artifacts/sk_cheesecake_nrf_p00_factory_test.sha256
```

必须保留 `--overlap=error`，地址冲突应当导致合并失败，不能静默覆盖。Bootloader
仓库现有的 `tools/hexmerge.py` 会比较两个 HEX 的启动地址元数据，并对当前这组
互不重叠的输入报 `Starting addresses are different`；因此本流程使用会清理该
元数据并按数据地址检查冲突的 Zephyr `mergehex.py`。

## 静态校验

以下检查会验证地址集合、逐字节一致性、App 向量表、UICR、BL 暂存区、NVS 和
settings 页。任何断言失败都不能继续烧录：

```bash
.venv-bootloader/bin/python <<'PY'
from intelhex import IntelHex

boot = IntelHex(
    "boot/Adafruit_nRF52_Bootloader/"
    "cmake-build-sk_cheesecake_nrf_p00/bootloader_mbr.hex"
)
app = IntelHex(
    "tracker/SlimeVR-Tracker-nRF/build_sk_ck_p00/"
    "SlimeVR-Tracker-nRF/zephyr/zephyr.hex"
)
factory = IntelHex("artifacts/sk_cheesecake_nrf_p00_factory_test.hex")

boot_addrs = set(boot.addresses())
app_addrs = set(app.addresses())
factory_addrs = set(factory.addresses())

assert boot_addrs.isdisjoint(app_addrs), "Bootloader 和 App 地址重叠"
assert factory_addrs == boot_addrs | app_addrs, "输出地址集合不是输入并集"
assert all(factory[a] == boot[a] for a in boot_addrs), "Bootloader 数据被改变"
assert all(factory[a] == app[a] for a in app_addrs), "App 数据被改变"

app_first = min(app_addrs)
app_last = max(app_addrs)
assert app_first == 0x1000, hex(app_first)
assert app_last < 0xE0000, hex(app_last)

sp = int.from_bytes(bytes(app[a] for a in range(0x1000, 0x1004)), "little")
reset = int.from_bytes(bytes(app[a] for a in range(0x1004, 0x1008)), "little")
assert 0x20000000 <= sp < 0x20040000, hex(sp)
assert reset & 1 and app_first <= (reset & ~1) <= app_last, hex(reset)

assert 0xF4000 in boot_addrs, "Bootloader 未从 0xF4000 开始"
boot_addr = int.from_bytes(
    bytes(factory[a] for a in range(0x10001014, 0x10001018)), "little"
)
mbr_param = int.from_bytes(
    bytes(factory[a] for a in range(0x10001018, 0x1000101C)), "little"
)
assert boot_addr == 0xF4000, hex(boot_addr)
assert mbr_param == 0xFE000, hex(mbr_param)

assert not any(0xE0000 <= a < 0xEA000 for a in factory_addrs), "BL 暂存区非空"
assert not any(0xEE000 <= a < 0xF4000 for a in factory_addrs), "NVS 非空"
assert not any(0xFF000 <= a < 0x100000 for a in factory_addrs), "settings 页非空"

print("Boot segments:", [(hex(s), hex(e)) for s, e in boot.segments()])
print("App segments:", [(hex(s), hex(e)) for s, e in app.segments()])
print("Factory segments:", [(hex(s), hex(e)) for s, e in factory.segments()])
print(f"App vector: SP=0x{sp:08X}, reset=0x{reset:08X}")
print(f"UICR: boot=0x{boot_addr:X}, mbr_params=0x{mbr_param:X}")
print("ALL STATIC CHECKS PASSED")
PY
```

## 烧录

factory HEX 必须通过 SWD/J-Link 全片擦除后烧录。只擦除 HEX 涉及的页可能保留旧
的 NVS 或 Bootloader settings，造成首次启动采用旧状态。

使用当前 Nordic `nrfutil device` 时：

```bash
nrfutil device program \
  --serial-number <probe-serial-number> \
  --firmware artifacts/sk_cheesecake_nrf_p00_factory_test.hex \
  --options chip_erase_mode=ERASE_ALL,verify=VERIFY_READ,reset=RESET_SYSTEM
```

使用旧版 `nrfjprog` 时：

```bash
nrfjprog \
  --family NRF52 \
  --program artifacts/sk_cheesecake_nrf_p00_factory_test.hex \
  --chiperase \
  --verify \
  --reset
```

首次启动时 Bootloader 可能需要写入 `UICR.REGOUT0=3.3V` 并额外复位一次，这是预期
行为。全片擦除后的 settings 页为空，现有 Bootloader 会通过 `0x1000` 处的 App
向量表验证并启动这个直接烧录的 App。不要把 factory HEX 或 Bootloader UF2 交给
ESB App OTA 工具。

## 产物用途

| 产物 | 使用方式 |
| --- | --- |
| `sk_cheesecake_nrf_p00_factory_test.hex` | SWD/J-Link 全片擦除后的首次/工厂烧录 |
| App `zephyr.uf2` | 已安装 Bootloader 的设备通过 USB 更新 App |
| Bootloader UF2 | 已安装兼容 Bootloader 的设备执行 BL 自升级 |
| ESB OTA App 包 | 通过接收器更新 App |

这些产物不能互相替代。尤其不能通过 USB 拖入 factory HEX，也不能把 factory HEX
送入 ESB OTA 流程。

## 硬件验收清单

软件静态检查通过不等于已经完成硬件验收。每次更换输入构建后至少执行：

1. 使用全片擦除烧录 factory HEX，确认校验成功。
2. 断电重启，允许首次 UICR 配置导致的一次额外复位，确认 App 正常启动。
3. 进入 USB Bootloader，刷入当前 App UF2，确认更新及启动正常。
4. 刷入与该板匹配的现有 Bootloader UF2，确认 BL 自升级后 App 仍能启动。
5. 执行一次 ESB App OTA，确认 OTA、重启和 App 运行正常。
6. 再次进入 USB Bootloader，确认 ESB OTA 没有破坏 USB App/BL 更新能力。

如果以上任一步失败，应保留烧录日志、使用的三个产物及其 SHA-256，不要仅依据
文件名判断版本。

## 本次测试产物记录

生成日期：2026-07-27（Asia/Shanghai）。

| 项目 | Git 提交/SHA-256 |
| --- | --- |
| Bootloader 仓库 | `84656248267248625f78298a6f85c19f65d13524` |
| App 仓库 | `48c8b05920f86e37b39e9a907fce221e9091fe6d` |
| `bootloader_mbr.hex` | `e04b9d8a0f4c36957bcdd4ccbc05b2a1f94604bb8cf9bfa0e253ad711d958d9b` |
| `zephyr.hex` | `74cc30620970d470c3956870520ffc45b4c405b40a373bdc02560afd62435854` |
| `sk_cheesecake_nrf_p00_factory_test.hex` | `7e7c3bb991af231691d518ca67fbc8ad99f1f1d1150819f85697e88dc3b8647b` |

本记录只绑定上述输入文件，不能仅凭仓库提交推断另一次本地构建会得到相同产物。
正式 CI、发布命名、产物归档和量产流程不在本次范围内。
