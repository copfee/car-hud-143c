#!/usr/bin/env python3
"""Create/verify firmware images and hashes; never opens hardware."""
from pathlib import Path
import hashlib
import json
import re
import subprocess
import sys
ROOT=Path(__file__).resolve().parents[1]
FOLDER=ROOT/'firmware'
MANIFEST=FOLDER/'manifest.json'
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
if '--verify' in sys.argv:
    data=json.loads(MANIFEST.read_text())
    for name,entry in data['images'].items():
        path=FOLDER/name
        if path.stat().st_size!=entry['bytes'] or digest(path)!=entry['sha256']:raise SystemExit(f'校验失败：{name}')
    print('固件 SHA-256 校验通过。')
else:
    definition=json.loads((ROOT/'config/obd_profiles.json').read_text())
    timing=definition['timing']
    full=(FOLDER/'car-hud-143c-full.bin').read_bytes()
    offsets={'bootloader.bin':0,'partitions.bin':0x8000,'boot_app0.bin':0xe000,'application.bin':0x10000}
    for name,offset in offsets.items():
        image=(FOLDER/name).read_bytes()
        if full[offset:offset+len(image)]!=image:raise SystemExit(f'镜像地址/内容错误：{name}')
    if len(full)>8388608:raise SystemExit('超出8MB Flash')
    data={
        'version':re.search(r'FirmwareVersion\[\]="([^"]+)"',(ROOT/'src/hud/version.hpp').read_text()).group(1),
        'board':'ESP32-S3-Touch-AMOLED-1.43C','adapter':'User-selected BLE ELM327 / 18F0/2AF0/2AF1',
        'upstream':'https://github.com/fbiego/car-hud','upstream_commit':'fc1d22e22a8d45689488066cbeb774d109af5ae1',
        'environment':'waveshare_amoled_143c','platform':'espressif32@6.9.0',
        'arduino_framework':'3.20017.241212+sha.dcc1105b','lvgl':'9.4.0','nimble':'2.2.3',
        'flash_bytes':8388608,'psram_bytes':8388608,'full_image_address':'0x0','application_address':'0x10000',
        'boot_header_flash_mode':'DIO (PlatformIO default for QIO firmware)',
        'default_profile':'generic','default_standard_protocol':'auto','software_protocols':list(range(1,10)),
        'profile_definitions':definition,'protocol_cache':'adapter address + manual profile, always revalidated',
        'connection':{**{key:timing[key] for key in ('notice_ms','round_ms','heartbeat_ms','silence_ms')},'background_recovery':True},
        'splash':{'duration_ms':3000,'fade_in_ms':300,'hold_ms':2400,'fade_out_ms':300,'brightness_percent':50,
                  'restore_saved_brightness_after_black_flush':True,'width':466,'height':466,'background':'#000000',
                  'format':'RGB565','ble_starts_after_splash':True,'repeat_on_reconnect':False,
                  'asset':json.loads((ROOT/'assets/startup/manifest.json').read_text())},
        'identity':{'source':'vehicle only','vin_command':'0902','ecu_name_command':'090A','model_inference':False,
                    'per_source_reassembly':True,'persistent_vin_cache':False,'online_decode':False},
        'pages':['overview','pedals','settings'],
        'settings':{'vertical_scroll':True,'info_refresh_ms':1000,'battery_sample_ms':2000,'battery_adc_gpio':4,
                    'battery_voltage_multiplier':2,'charger_status_gpio':7,'battery_presence_detection':False,
                    'content_layout':'vertical flex / automatic text height'},
        'bluetooth_selection':{'first_boot_scan_after_splash':True,'scan_ms':4000,'max_displayed_devices':24,
                               'persist_after':'GATT service/characteristic validation and subscription',
                               'persistent_record':'c260-hud / ble_peer / version 1; address + address type + name',
                               'automatic_reconnect':'saved address and address type',
                               'classic_bluetooth_spp':False,'bonded_private_address_resolution':False,
                               'hardware_acceptance_pending':True},
        'verification':{'protocol_1_to_9':'synthetic parser and fake ECU tests',
                        'mercedes_standard_7_pressure_6':'historical captures, current firmware requires vehicle regression',
                        'honda_cl300_2023':'user screenshot confirms protocol 5 in Car Scanner; current firmware/PID test pending','current_release_vehicle_tested':False},
        'hardware_tested':False,'images':{},'source_sha256':{}
    }
    for path in sorted(FOLDER.glob('*.bin')):data['images'][path.name]={'bytes':path.stat().st_size,'sha256':digest(path)}
    for path in [ROOT/'config/obd_profiles.json',*sorted((ROOT/'diagnostics').glob('*.py')),*sorted((ROOT/'src/hud').glob('*.cpp')),*sorted((ROOT/'src/hud').glob('*.hpp'))]:
        data['source_sha256'][str(path.relative_to(ROOT))]=digest(path)
    MANIFEST.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n')
    (FOLDER/'SHA256SUMS').write_text(''.join(f"{entry['sha256']}  {name}\n" for name,entry in data['images'].items()))
    print('已生成版本清单及 SHA256SUMS。')
