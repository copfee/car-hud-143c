# 奔驰 C260 2021：CAN 被动监听测试

本工具用于实测 IOS-Vlink 在奔驰 OBD 接口能看到哪些 CAN 报文。它与原有 PID 读取脚本独立，**不发送 `0100`、`010C`、`222002` 等车辆查询**，不解码未经确认的车速、转速或压力。

原有实际日志证明车辆标准 OBD 查询走协议 7；这不能直接证明网关放行了仪表所需的广播。先测试可见性，再核对信号定义，是本工具的用途。

## 1. 开始前

- 车辆停稳，驻车；先做“点火 ON、发动机未启动”和“发动机怠速”两个独立记录。
- 关闭 Car Scanner、原来的 Python 读取进程等其他 BLE 客户端。开发板若也在自动连接 IOS-Vlink，请先进入演示模式或断电，避免争抢连接。
- 已使用现有 BLE 虚拟环境，无需新增依赖；单独解压项目后需安装 `diagnostics/requirements.txt`，Python 3.10+。
- 默认设备为IOS-Vlink，按设备名称 `IOS-Vlink` 查找。换电脑后 macOS UUID 可能不同，可用 `--device` 指定该电脑识别的地址。
- 键盘标记功能在 macOS/Linux 交互终端启用；重定向输入时自动关闭，Ctrl+C 仍可结束。

## 2. 直接执行

在仓库根目录执行（先按README创建`.tools/obd-env`环境）：

```bash
cd car-hud-143c
.tools/obd-env/bin/python diagnostics/mercedes_can_monitor.py --label "点火ON-发动机未启动"
```

默认先监听 **11 位 / 500 kbit/s（ELM 协议 6）30 秒**，再监听 **29 位 / 500 kbit/s（ELM 协议 7）30 秒**。初始化另计，两个阶段各自保存结果。11 位与 29 位是报文 ID 格式，不代表一定连接了两条不同物理总线。

更长的怠速记录：

```bash
.tools/obd-env/bin/python diagnostics/mercedes_can_monitor.py --seconds 60 --label "发动机怠速"
```

只测某一种格式：

```bash
.tools/obd-env/bin/python diagnostics/mercedes_can_monitor.py --protocol 6 --seconds 60
.tools/obd-env/bin/python diagnostics/mercedes_can_monitor.py --protocol 7 --seconds 60
```

从发布包运行：在解压后的 `car-hud-1.43c` 目录安装依赖，然后用对应 Python 执行 `diagnostics/mercedes_can_monitor.py`；它与工作区 兼容入口是同一维护实现。

## 3. 实时终端与时间标记

界面在当前终端原地更新，每秒最多四次，显示当前配置/监听阶段、帧数、CAN ID、数据变化次数、最近原始数据和错误数。不会逐条打印全部帧。

监听时直接按键，无需回车：

|按键|记录内容|
|---|---|
|`1`|静止/怠速时间点|
|`2`|轻给油时间点|
|`3`|制动踏板操作时间点|
|`4`|松开踏板时间点|
|`m`|自定义事件时间点（标签固定为“手动标记”）|
|`q`|停止监听、保存并退出|
|Ctrl+C|停止监听、保存并退出|

标记只记录主机操作时间，不会发给车辆。建议在车辆驻车、发动机怠速时，依次记录：静止数秒 → 按 `2` 并轻给油 → 按 `4` 并松开 → 按 `3` 并操作制动 → 按 `4` 并松开。操作间隔数秒、每次只改变一个因素，便于对照。此过程不需要挂挡或行驶。

不要将“某字节同时变化”直接当作已找到信号：计数器、校验和、多个状态联动都可能产生类似现象。应重复多轮并用已知量核对。

## 4. 输出文件

每次创建新目录，默认位于项目上一级：`mercedes_can_monitor_年月日_时分秒_微秒/`。不会覆盖此前记录。结束时打印完整报告路径。

|文件|用途|
|---|---|
|`report.md`|先看这里：各格式结果、最多 50 个高频 ID、错误和结论边界|
|`summary.json`|完整结构化汇总：参数、主机版本、GATT、配置命令响应、ELM 版本、电压、CAN 错误计数、逐 ID 统计|
|`raw.jsonl`|完整 BLE 接收分片（ASCII 与十六进制）、AT 命令及响应、监听启停、错误、时间标记|
|`frames.csv`|每帧的时间、阶段、协议、ID、DLC、原始数据和最近标记|
|`can_ids.csv`|各 ID 的帧数、观测频率、变化次数、变化字节和递增计数器候选|
|`markers.csv`|人工操作时间点，便于与帧 CSV 对齐|

逐 ID 的 JSON 统计还包括：不同 DLC 数量、前 8 种数据样本、每字节最小/最大值、不同取值数量、变化位掩码、间隔均值和最近 256 个正间隔的中位数/p95。

字节位置从 **0** 开始；原始数据保留 ISO-TP PCI 和填充，未按 PID 重新解释。CSV 的 `diagnostic_shape` 仅表示 ID 外观接近常见诊断地址，不代表已经确认报文用途。

**时间来自电脑收到 BLE 通知的时刻**。同一通知中的多帧可能出现零间隔；一帧跨通知时使用收齐该行的时间。因此这些统计可以研究接收表现，不能作为车辆真实总线时间戳或完整总线负载。

## 5. 如何理解结果

|结果|含义与下一步|
|---|---|
|有大量 ID，数据稳定或随操作变化|已看到被动报文；下一步对照原始帧和标记做信号验证，暂不接入 HUD 解码|
|仅看到 `7E0…7EF`、`18DA…`、`18DB…` 等形态|可能只有诊断流量；检查是否有其他诊断端主动查询，不能据此断言已能被动获取全部仪表数据|
|两个格式均完整监听、零帧|当前接口、时刻和模块设置下未观察到 CAN 帧；可能是网关隔离、车辆休眠或适配器能力，不能据此断言整车没有广播|
|`ATCSM1` 等必需配置返回 `?`/非 OK|配置验证失败，脚本不会进入 ATMA；保存响应后核对模块实现|
|`BUFFER FULL` / 接收队列溢出|采集不完整；不能使用频率当作真实总线频率。找到目标 ID 后用硬件过滤重测|
|有输出但无法解析|查看 `raw.jsonl` 的实际格式；脚本会记录前 20 个样本，不会伪装成静默无帧|
|停止后没收到 `>`|ELM 会话失步，该阶段不计完整；不会继续发普通查询，必要时重连再初始化|
|蓝牙断开/用户退出|保存已有数据和退出原因，不能算完整时间窗|

`ATDPN` / `ATDP` 返回 6 或 7 只表明适配器的本地配置，**不代表已经与车辆 ECU 建立有效诊断连接**。本测试没有主动唤醒 ECU，也不以这些文本宣称车辆兼容。

即使没有报告溢出，ELM/BLE 内部仍可能丢帧；本工具不能证明抓到了全量报文。若后续需要准确总线时间戳或高负载完整采集，应使用具备硬件时间戳与 listen-only 模式的专用 CAN 采集器。

## 6. 更细的测试

重复两轮，比较相同操作下的 ID：

```bash
.tools/obd-env/bin/python diagnostics/mercedes_can_monitor.py --seconds 60 --passes 2 --label "驻车-分阶段操作"
```

已找到某个 ID 后，仅用适配器接收过滤。例如 `123` **只是参数格式示例，并非已验证奔驰信号 ID**：

```bash
.tools/obd-env/bin/python diagnostics/mercedes_can_monitor.py --protocol 6 --filter-id 123 --seconds 120
```

29 位示例同样不是车辆定义：

```bash
.tools/obd-env/bin/python diagnostics/mercedes_can_monitor.py --protocol 7 --filter-id 18FF1234 --seconds 120
```

过滤发生在 ELM 接收端，通过 `ATCRA` 配置；不发送该 CAN ID，不使用 `ATSH`。若 29 位过滤 ID 超过 `7FF`，`both` 自动缩小为只测 29 位；显式 11 位加超范围 ID 会报参数错误。

不支持显示 DLC 的模块可尝试：

```bash
.tools/obd-env/bin/python diagnostics/mercedes_can_monitor.py --no-dlc
```

这会改用 `ATD0`。默认 `ATD1` 下若模块仍省略 DLC，工具会保留格式提醒，而不是悄悄视为完全一致。

查看参数或关闭终端刷新：

```bash
.tools/obd-env/bin/python diagnostics/mercedes_can_monitor.py --help
.tools/obd-env/bin/python diagnostics/mercedes_can_monitor.py --no-tui --no-keys
```

参数范围：`--seconds` 1～600 秒/阶段；`--passes` 1～20；`--connection-timeout` 5～180 秒，默认 60（BLE 连接、每阶段配置各自的上限）；`--stop-timeout` 1～15 秒，默认 5；`--queue-size` 128～65536 个通知，默认 4096；`--max-ids` 16～16384，默认 4096；`--max-frames` 0～10000000，默认 0 只按时间停止。

达到逐 ID 统计容量时不再建立新 ID 的统计，原始帧仍写盘并在报告提醒；达到帧数上限时正常停止该阶段。可用 `--output-dir` 指定一个尚不存在的新目录。退出码：0=全部阶段完整且至少有一帧；2=无帧、部分完成或错误；130=用户退出。完整无帧也返回 2，表示尚无可用监听证据。

## 7. 实现边界与命令依据

复用现有 BLE/GATT 连接层；独立封装持续接收会话、分片解析、逐 ID 统计、文件写入和终端视图，避免把 ATMA 当作一问一答的 PID 命令。

初始化以本地 `ATI` 获取提示符，再 `ATZ`。确认 `ATE0 / ATL0 / ATS1 / ATM0 / ATH1 / ATCSM1 / ATCAF0 / ATCFC0 / ATD1 / ATTP6或7`；未指定单 ID 时使用零过滤器/零掩码接收当前格式全部 ID。`ATCSM1` 要求静默监听，`ATCFC0` 关闭自动流控；任何必需配置未返回 OK 都终止测试。随后读 `ATDPN / ATDP / ATCS / ATRV` 记录模块配置和状态。`ATCS` 在部分克隆模块上可能不支持，只作可选信息。

启动 `ATMA` 后持续接收；到时发送单字节 `x` 中断，并等到 `>` 才继续本地命令。**不发送空回车**，避免 ELM 重复上一条诊断请求。写入路径使用命令白名单，禁止原始 CAN 数据、诊断 PID、非静默配置和主动协议搜索。

软件不能仅凭克隆模块返回 OK 独立验证其收发器完全遵守静默配置。测试也不绕过网关、不进行车型专用地址扫描，不适用于 CL300 的 KWP/K-Line 被动广播验证。

命令依据：[ELM327 官方数据手册](https://www.elmelectronics.com/wp-content/uploads/2017/01/ELM327DS.pdf)，主要参考 CAN Silent Monitoring、CAN Automatic Formatting、CAN Flow Control、Monitor All、CAN Filter/Mask 与 Receive Address 章节。

本次只增加电脑测试工具；固件与现有 PID 读取逻辑未修改，无需烧录。工具尚未连接用户车辆进行监听验证，测试报告中的模拟用例不能替代实车结果。
