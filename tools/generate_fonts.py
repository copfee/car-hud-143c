#!/usr/bin/env python3
"""Regenerate the Chinese UI subset. Building firmware does not require this tool."""
import argparse
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--font-source', required=True, type=Path, help='NotoSansSC-Regular.otf')
parser.add_argument('--converter', required=True, type=Path, help='lv_font_conv.js (npm lv_font_conv)')
args = parser.parse_args()
sources=[p for directory in (root/'src/hud',root/'tests') for p in directory.iterdir() if p.suffix in ('.cpp','.hpp')]
symbols = sorted({c for path in sources for c in path.read_text() if ord(c) > 127})
subprocess.run(['node', str(args.converter.resolve()), '--font', str(args.font_source.resolve()),
                '--size', '18', '--bpp', '4', '--format', 'lvgl', '--lv-font-name', 'hud_cn_18',
                '--range', '0x20-0x7e', '--symbols', ''.join(symbols), '--no-compress',
                '--output', str(root/'src/hud/hud_cn_18.c')], check=True)
output=root/'src/hud/hud_cn_18.c'
portable=output.read_text().replace(str(args.font_source.resolve()),args.font_source.name).replace(str(root)+'/', '')
output.write_text(portable.rstrip()+'\n')
print(f'Chinese/punctuation glyphs: {len(symbols)}')
