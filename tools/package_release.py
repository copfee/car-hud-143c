#!/usr/bin/env python3
"""Package the maintained project, including diagnostics, shared definitions and tests."""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys
import zipfile
ROOT=Path(__file__).resolve().parents[1]
EXCLUDED={'.git','.pio','.tools','__pycache__','.DS_Store'}
def package(output=None):
    subprocess.run([sys.executable,str(ROOT/'tools/manifest.py'),'--verify'],check=True)
    manifest=json.loads((ROOT/'firmware/manifest.json').read_text())
    output=(output or ROOT.parent/f'{ROOT.name}-{manifest["version"]}-release.zip').resolve()
    if output.is_relative_to(ROOT):raise SystemExit('发布包必须位于项目目录之外')
    for name,expected in manifest['source_sha256'].items():
        if hashlib.sha256((ROOT/name).read_bytes()).hexdigest()!=expected:raise SystemExit(f'源码已变化，请重新构建：{name}')
    with zipfile.ZipFile(output,'w',zipfile.ZIP_DEFLATED,compresslevel=9) as z:
        for item in sorted(ROOT.rglob('*')):
            if item.is_file() and not(set(item.relative_to(ROOT).parts)&EXCLUDED):z.write(item,arcname=str(Path(ROOT.name)/item.relative_to(ROOT)))
    with zipfile.ZipFile(output) as z:
        assert z.testzip() is None
        for name,entry in manifest['images'].items():
            content=z.read(f'{ROOT.name}/firmware/{name}')
            assert len(content)==entry['bytes'] and hashlib.sha256(content).hexdigest()==entry['sha256']
    checksum=hashlib.sha256(output.read_bytes()).hexdigest()
    output.with_suffix('.zip.sha256').write_text(f'{checksum}  {output.name}\n')
    print(json.dumps({'version':manifest['version'],'archive':str(output),'bytes':output.stat().st_size,'sha256':checksum},ensure_ascii=False,indent=2))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path);package(p.parse_args().output)
