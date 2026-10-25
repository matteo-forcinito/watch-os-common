"""Original vector-like battery mark rendered to an offline PNG, no external assets."""
from pathlib import Path
import struct, zlib

def rounded(x,y,left,top,right,bottom,radius):
    if not(left<=x<=right and top<=y<=bottom): return False
    cx=min(max(x,left+radius),right-radius)
    cy=min(max(y,top+radius),bottom-radius)
    return (x-cx)**2+(y-cy)**2<=radius**2

def polygon(x,y,points):
    inside=False
    j=len(points)-1
    for i,(px,py) in enumerate(points):
        qx,qy=points[j]
        if (py>y)!=(qy>y) and x<(qx-px)*(y-py)/(qy-py)+px: inside=not inside
        j=i
    return inside

def sample(x,y):
    if not rounded(x,y,2,2,62,62,19): return (0,0,0,0)
    color=(21,37,39,255)
    if rounded(x,y,26,13,38,20,2) or rounded(x,y,17,18,47,51,5): color=(157,232,187,255)
    if rounded(x,y,21,22,43,47,2): color=(21,37,39,255)
    if polygon(x,y,[(34,24),(24,37),(31,37),(28,46),(41,31),(34,31)]): color=(157,232,187,255)
    return color

rows=[]
for y in range(64):
    row=bytearray()
    for x in range(64):
        colors=[sample(x+(dx+.5)/4,y+(dy+.5)/4) for dy in range(4) for dx in range(4)]
        opaque=[c for c in colors if c[3]]
        row.extend([round(sum(c[i] for c in opaque)/len(opaque)) if opaque else 0 for i in range(3)]+[round(len(opaque)*255/16)])
    rows.append(b'\x00'+row)
def chunk(kind,data):
    raw=kind+data
    return struct.pack('>I',len(data))+raw+struct.pack('>I',zlib.crc32(raw)&0xffffffff)
png=b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>IIBBBBB',64,64,8,6,0,0,0))+chunk(b'IDAT',zlib.compress(b''.join(rows),9))+chunk(b'IEND',b'')
target=Path(__file__).resolve().parents[1]/'lite/icon.png'
target.write_bytes(png)
print('Watch Lite original icon:',len(png),'bytes')
