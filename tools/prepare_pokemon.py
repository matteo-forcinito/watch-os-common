"""Build a compact personal-use SpriteCollab pack; keep generated art outside Git.

Requires Pillow. Downloads are pinned, credits accompany the generated source.
This is a build-time asset converter, not a firmware installer.
"""
from pathlib import Path
import argparse, concurrent.futures, json, urllib.request
import xml.etree.ElementTree as ET
from PIL import Image

REVISION = 'cb8cfe557d1528af1df0efdcf8bd518c6c0e5ba0'
FAMILIES = [[4,5,6],[37,38,38],[58,59,59],[7,8,9],[60,61,62],[54,55,55],
            [1,2,3],[43,44,45],[69,70,71],[74,75,76],[25,26,26],[111,112,112]]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
out = args.output.resolve(); out.mkdir(parents=True, exist_ok=True)
cache = out/'source'; cache.mkdir(exist_ok=True)
base = f'https://raw.githubusercontent.com/PMDCollab/SpriteCollab/{REVISION}/sprite'

def fetch(number, shiny, name):
    folder = f'{number:04d}' + ('/0000/0001' if shiny else '')
    path = cache/f'{number:03d}-{int(shiny)}-{name}'
    if not path.exists():
        request = urllib.request.Request(f'{base}/{folder}/{name}', headers={'User-Agent':'Petland-asset-builder'})
        with urllib.request.urlopen(request, timeout=35) as response: path.write_bytes(response.read())
    return path

def build(pair):
    number, shiny = pair
    xml = ET.parse(fetch(number, shiny, 'AnimData.xml')).getroot()
    animations = {a.findtext('Name'): a for a in xml.find('Anims')}
    name = 'Idle' if 'Idle' in animations else 'Walk'
    animation = animations[name]
    while animation.find('CopyOf') is not None:
        name = animation.findtext('CopyOf'); animation = animations[name]
    width, height = int(animation.findtext('FrameWidth')), int(animation.findtext('FrameHeight'))
    image = Image.open(fetch(number, shiny, name+'-Anim.png')).convert('RGBA')
    count = min(4, len(animation.find('Durations')), image.width // width)
    if not count: raise ValueError(f'Empty animation {number}')
    frames = [image.crop((i*width,0,(i+1)*width,height)) for i in range(count)]
    boxes = [frame.getchannel('A').getbbox() for frame in frames]
    boxes = [box for box in boxes if box]
    if not boxes: raise ValueError(f'Invisible sprite {number}')
    box = (min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes), max(b[3] for b in boxes))
    factor = min(1, 40/(box[2]-box[0]), 40/(box[3]-box[1]))
    size = (max(1,int((box[2]-box[0])*factor)), max(1,int((box[3]-box[1])*factor)))
    palette, indices, data = [], {}, bytearray()
    preview = []
    for frame in frames:
        cell = Image.new('RGBA',(40,40))
        cropped = frame.crop(box).resize(size, Image.Resampling.NEAREST)
        cell.paste(cropped,((40-size[0])//2,40-size[1]))
        preview.append(cell)
        for r,g,b,a in cell.getdata():
            if a < 128: data.append(255); continue
            color = (r>>3)<<11 | (g>>2)<<5 | b>>3
            if color not in indices:
                if len(palette) >= 255: raise ValueError('Palette budget exceeded')
                indices[color] = len(palette); palette.append(color)
            data.append(indices[color])
    credit = fetch(number, shiny, 'credits.txt').read_text(encoding='utf-8')
    return number, shiny, count, palette, data, credit, preview

pairs = [(n, shiny) for n in sorted({n for family in FAMILIES for n in family}) for shiny in [False,True]]
with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool: sprites = list(pool.map(build,pairs))
lines = ['/* Generated personal-use PMD sprite pack. See CREDITS.json. */','#include "pokemon.h"']
for n, shiny, count, palette, data, credit, preview in sprites:
    key = f'p{n}_{int(shiny)}'
    lines += [f'static const uint16_t {key}_pal[] = {{'+','.join(map(str,palette))+'};',
              f'static const uint8_t {key}_px[] = {{'+','.join(map(str,data))+'};']
counts = {(n,int(shiny)):count for n,shiny,count,*_ in sprites}
lines.append('const pokemon_sprite_t pokemon_sprites[12][3][2] = {')
for family in FAMILIES:
    lines.append('{'+','.join('{'+','.join(f'{{p{n}_{s}_px,p{n}_{s}_pal,{counts[n,s]}}}' for s in [0,1])+'}' for n in family)+'},')
lines.append('};')
(out/'pokemon_assets.c').write_text('\n'.join(lines),encoding='utf-8')
(out/'pokemon_assets.h').write_text('#include "pokemon.h"\nextern const pokemon_sprite_t pokemon_sprites[12][3][2];\n')
(out/'CREDITS.json').write_text(json.dumps({'source':'https://github.com/PMDCollab/SpriteCollab',
    'revision':REVISION,'terms':'https://github.com/PMDCollab/SpriteCollab#submission-and-use-policy',
    'license':'CC BY-NC 4.0; Pokemon copyright Nintendo/Game Freak/The Pokemon Company; original sprites credited individually',
    'changes':'Idle frames extracted, transparent, nearest-neighbour fit to 40x40, RGB565 indexed encoding',
    'sprites':[{'dex':n,'shiny':shiny,'credits':credit} for n,shiny,_,_,_,credit,_ in sprites]},indent=2),encoding='utf-8')
sheet = Image.new('RGBA',(40*10,60*((len(sprites)+9)//10)),(18,32,45,255))
from PIL import ImageDraw
draw = ImageDraw.Draw(sheet)
for i,(n,shiny,_,_,_,_,preview) in enumerate(sprites):
    x,y = i%10*40,i//10*60
    sheet.alpha_composite(preview[0],(x,y)); draw.text((x,y+42),f'{n}{"s" if shiny else ""}',fill='white')
sheet.save(out/'preview.png')
print(f'POKEMON_PACK_READY {len(sprites)//2} Pokemon, normal + shiny; {sum(len(s[4]) for s in sprites)} pixel bytes')
