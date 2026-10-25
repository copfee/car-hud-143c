# IOS-Vlink 通用读取脚本

维护入口elm327_ble.py，数据定义位于项目config/obd_profiles.json。需要Python3.10+，依赖见requirements.txt。

```bash
python3 elm327_ble.py --read --extended --protocol auto
python3 elm327_ble.py --live --profile generic
python3 elm327_ble.py --live --profile mercedes-w205-2020
```

默认通用／自动协议；新增`--profile honda-cl300-2023`默认使用5，手动--protocol优先；--protocol支持auto和1～9。单轮与实时均支持--log，VIN只在会话中缓存，日志保留实际响应。终端原地更新，Ctrl+C释放连接。原工作区outputs/elm327_ble.py仍可使用。完整说明见项目使用说明.md。

独立CL300测试：`python3 cl300_pid_test.py`，或`python3 cl300_pid_test.py --all-supported --samples 1`。默认协议5，连接上限60秒，详见CL300-PID测试说明.md。

当前只提供主动OBD读取工具。CAN被动监听暂不提供，作为未来可选性能优化方向记录在项目README。
