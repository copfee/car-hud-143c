# 本田 CL300 2023 PID 实测

工具：`diagnostics/cl300_pid_test.py`；共同解析器在同目录。这是独立的有限测试流程，不会启动奔驰配置、查询奔驰压力、扫描未知私有地址或修改车辆设置。

## 开始测试

确认诊断接口及转接线接线正确，将 IOS-Vlink 接到 CL300，打开点火，关闭手机或其他正在占用模块的客户端。在仓库根目录执行（先按README安装`.tools/obd-env`环境）：

```bash
.tools/obd-env/bin/python diagnostics/cl300_pid_test.py
```

默认使用用户截图确认的协议5（ISO14230-4 KWP快速初始化，10.4kbaud），连接与协议验证上限60秒；读取每个ECU的标准支持位图，采样五轮常用数据，终端原地更新，完成后释放蓝牙并退出。车速为0是有效结果；车型名称不根据VIN猜测。

默认数据：转速、车速、水温、绝对／相对节气门、油量、里程、ECU供电电压、进气温度、发动机运行时间、负荷，以及车辆声明支持时的标准踏板PID。踏板PID不等同于摩托车手柄开度；未声明的PID不发送。

完整读取所有**已声明支持**的标准PID，建议先跑一轮：

```bash
.tools/obd-env/bin/python diagnostics/cl300_pid_test.py --all-supported --samples 1
```

只查看指定项目：

```bash
.tools/obd-env/bin/python diagnostics/cl300_pid_test.py --pids 0C,0D,05,11 --samples 10
```

仍只请求已声明的PID，不会强制探测未支持项目。未知解码格式保留原始字节。最多每项每轮一个请求，请求之间至少留100ms；全量读取可能明显更久，60秒仅限制连接与协议发现，不限制后续完整采样总时长。

需要重新自动识别时加`--protocol auto`；只有在核实后才用其他协议强制对照。默认5来自本车Car Scanner连接截图，不是按年份猜测。截图确认KWP5的应用连接，但新脚本和固件仍待实测。

## 输出

- 项目上一级的 `cl300_pid_test.log`：JSON行格式原始命令／响应、耗时、采样轮次和阶段；重复运行追加日志。
- 项目上一级的 `cl300_pid_test_summary.json`：本次测试完整汇总，包含协议证据、各ECU位图、每项支持状态、请求／成功次数、原始数据和解码结果、VIN／ECU名称。

不同ECU分别保留，不取平均。`last_responses`只表示最近一次查询；`sources`保留各来源最后一次有效响应，并带有轮次与时间。终端空间不足时省略部分行，JSON仍保存全部项目。

Ctrl+C、断连或超时会保存已经完成的结果并释放蓝牙。退出码0表示至少一项PID读取成功，2表示未完成或没有有效PID数据，130表示用户取消。`completed=true`仅表示测试流程跑完，不能单独证明所有数据可用。

如果`0100`没有有效ECU响应，脚本不会把ATDPN的适配器配置当成车辆协议确认，也不会继续盲扫PID。先核实点火、电源、接口与转接线；仍无响应时保留日志，可能需要本田专用诊断定义，不能仅据此认定车辆没有诊断数据。

可用`--log`和`--report`另存结果；`--connection-timeout`可设5～180秒的连接总上限，默认60秒；`--no-identity`跳过VIN／ECU名称，`--no-tui`关闭原地刷新。默认按IOS-Vlink名称发现；可用`--device`指定本机扫描出的地址/UUID。

新电脑或解压发布包：安装项目`diagnostics/requirements.txt`依赖后直接运行`diagnostics/cl300_pid_test.py`。不需要编译或烧录固件。

通用脚本也可使用`diagnostics/elm327_ble.py --live --profile honda-cl300-2023`，省略--protocol时默认5，显式--protocol auto则自动识别。截图中的ECU名称和校准ID不硬编码为当前车辆读回值。
