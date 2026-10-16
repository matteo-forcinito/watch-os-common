"""Exercise the ORF2 mirror session of a device: frames, input, session end.

    python mirror_test.py <ip> <token-file> [pair-code]
"""
import http.client
import json
import struct
import sys
import time
from pathlib import Path

ip, token_file = sys.argv[1], Path(sys.argv[2])
conn = http.client.HTTPConnection(ip, 80, timeout=10)


def call(method, path, body=None, token=None):
    headers = {'X-Key': token or ''}
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers['Content-Type'] = 'application/json'
    for attempt in range(2):
        try:
            conn.request(method, path, body=data, headers=headers)
            r = conn.getresponse()
            return r.status, r.read()
        except (http.client.HTTPException, OSError):
            conn.close()
            if attempt:
                raise


if len(sys.argv) > 3:
    status, raw = call('POST', '/api/pair', {'code': sys.argv[3]})
    print('pair', status, raw[:80])
    token_file.write_text(json.loads(raw)['token'])
token = token_file.read_text().strip()

print('remote', call('GET', '/api/remote', token=token))
print('enable', call('POST', '/api/remote', {'enabled': True}, token))

seq = None
sizes, times, areas = [], [], []
t_start = time.time()
for i in range(40):
    t0 = time.time()
    q = '' if seq is None else f'&seq={seq}'
    status, raw = call('GET', '/api/remote/frame?format=orf2&wait=150' + q, token=token)
    dt = time.time() - t0
    if status != 200:
        print('frame', i, status, raw[:100])
        time.sleep(1)
        continue
    magic, sw, sh, x, y, w, h, seq, flags, length = struct.unpack('<4s6HIII', raw[:28])
    assert magic == b'ORF2' and length == len(raw) - 28, (magic, length, len(raw))
    sizes.append(len(raw)); times.append(dt); areas.append((x, y, w, h, flags))
    if i == 5:
        print('tap', call('POST', '/api/remote/input', {'action': 'swipe', 'x': 230, 'y': 100, 'end_x': 230, 'end_y': 380}, token))
    if i == 20:
        print('home', call('POST', '/api/remote/input', {'action': 'home'}, token))
print(f'{len(times)} frames in {time.time() - t_start:.1f}s')
for (dt, size, area) in zip(times, sizes, areas):
    print(f'  {dt*1000:6.0f} ms {size:7d} B area={area}')
print('config', call('GET', '/api/remote', token=token))
print('end', call('POST', '/api/remote/session', {'active': False}, token))
print('config', call('GET', '/api/remote', token=token))
