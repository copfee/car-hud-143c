# IOS-Vlink 通用读取脚本

维护入口elm327_ble.py，数据定义位于项目config/obd_profiles.json。需要Python3.10+，依赖见requirements.txt。

```bash
python3 elm327_ble.py --read --extended --protocol auto
python3 elm327_ble.py --live --profile generic
python3 elm327_ble.py --live --profile mercedes-c260-2021
```

默认通用／自动协议；`--profile`支持generic和mercedes-c260-2021，均默认自动协议，手动--protocol优先；--protocol支持auto和1～9。单轮与实时均支持--log，VIN只在会话中缓存，日志保留实际响应。终端原地更新，Ctrl+C释放连接。原工作区outputs/elm327_ble.py仍可使用。完整说明见项目使用说明.md。

当前只提供主动OBD读取工具。CAN被动监听暂不提供，作为未来可选性能优化方向记录在项目README。
