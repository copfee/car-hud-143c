#!/usr/bin/env python3
"""Render PNG/GIF deliverables from the actual LVGL ui_test frame captures (Pillow required)."""
from pathlib import Path
import argparse
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]

def load(path):
    with Image.open(path) as image:
        return image.convert('RGB')

def render(native):
    out = ROOT / 'previews'
    out.mkdir(exist_ok=True)
    for path in native.glob('*.ppm'):
        if not path.name.startswith('splash-'):
            load(path).save(out / (path.stem + '.png'))
    load(native/'splash-0300.ppm').save(out/'startup-logo.png')
    frames = [load(native/f'splash-{ms:04d}.ppm') for ms in range(0, 3000, 50)]
    connection = load(native/'splash-3000.ppm')
    palette_source = Image.new('RGB', (932, 466))
    palette_source.paste(frames[6], (0, 0));palette_source.paste(connection, (466, 0))
    palette = palette_source.quantize(colors=256)
    frames = [frame.quantize(palette=palette, dither=Image.Dither.NONE) for frame in frames]
    connection = connection.quantize(palette=palette, dither=Image.Dither.NONE)
    frames[0].save(out/'startup-animation.gif', save_all=True, append_images=frames[1:],
                   duration=[50]*len(frames), loop=0, optimize=True, disposal=1)
    frames[0].save(out/'startup-sequence.gif', save_all=True, append_images=frames[1:]+[connection],
                   duration=[50]*len(frames)+[500], loop=0, optimize=True, disposal=1)
    for filename, names in [('native-ui', ['overview', 'pedals', 'settings']),
                            ('settings-overview', ['settings', 'settings-connection', 'settings-system', 'settings-help'])]:
        sheet = Image.new('RGB', (466*len(names), 466), 'black')
        for i, name in enumerate(names):
            sheet.paste(load(out/(name+'.png')), (466*i, 0))
        sheet.save(out/(filename+'.png'))
    for filename, expected_ms in [('startup-animation.gif', 3000), ('startup-sequence.gif', 3500)]:
        with Image.open(out/filename) as gif:
            duration = 0
            for i in range(gif.n_frames):
                gif.seek(i);duration += gif.info['duration']
            assert duration == expected_ms, (filename, duration)
            print(f'{filename}: {duration} ms, {gif.n_frames} frames, {gif.size[0]}x{gif.size[1]}')

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--native-dir', type=Path, default=ROOT/'.tools/native')
    render(parser.parse_args().native_dir)
