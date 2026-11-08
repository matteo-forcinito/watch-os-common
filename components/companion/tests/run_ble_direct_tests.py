"""Test production direct BLE dispatch and idle-worker handoff with MSVC."""
from pathlib import Path
import subprocess
import tempfile

def extract_function(text: str, signature: str) -> str:
    """Return a complete C function, counting braces outside comments/quotes."""
    search = 0
    while True:
        start = text.index(signature, search)
        body = text.index('{', start)
        semicolon = text.find(';', start, body)
        if semicolon < 0:
            break
        search = start + len(signature)
    depth = 0
    state = 'code'
    escaped = False
    i = body
    while i < len(text):
        c = text[i]
        n = text[i + 1] if i + 1 < len(text) else ''
        if state == 'line':
            if c == '\n': state = 'code'
        elif state == 'block':
            if c == '*' and n == '/': state = 'code'; i += 1
        elif state in ('string', 'char'):
            if escaped: escaped = False
            elif c == '\\': escaped = True
            elif (state == 'string' and c == '"') or (state == 'char' and c == "'"):
                state = 'code'
        else:
            if c == '/' and n == '/': state = 'line'; i += 1
            elif c == '/' and n == '*': state = 'block'; i += 1
            elif c == '"': state = 'string'
            elif c == "'": state = 'char'
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0: return text[start:i + 1]
        i += 1
    raise ValueError(f'unclosed C body for {signature}')

tests = Path(__file__).resolve().parent
source = (tests.parent / 'ble_bridge.c').read_text(encoding='utf-8')
functions = '\n'.join(extract_function(source, signature) for signature in (
    'void ble_companion_set_api_handler(', 'static bool run_direct(', 'static void worker('))
with tempfile.TemporaryDirectory(prefix='orbit-ble-direct-') as temp:
    temp = Path(temp)
    c_file = temp / 'ble_direct.c'
    exe = temp / 'ble_direct.exe'
    c_file.write_text((tests / 'test_ble_direct.c').read_text(encoding='utf-8').replace('/* PRODUCTION */', functions), encoding='utf-8')
    batch = temp / 'run.cmd'
    vcvars = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
    batch.write_text(f'@echo off\ncall "{vcvars}" >nul && cl /nologo /std:c11 /experimental:c11atomics /W3 "{c_file}" /Fo:"{temp / "direct.obj"}" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=tests, check=True)
