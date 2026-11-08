"""Compile production queued peer-input handling against deterministic UI stubs."""
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

tests=Path(__file__).resolve().parent
source=(tests.parent/'link_scene.c').read_text(encoding='utf-8')
functions='\n'.join(extract_function(source,s) for s in ('static void input_apply(', 'bool link_scene_input('))
with tempfile.TemporaryDirectory(prefix='orbit-peer-input-') as temp:
    temp=Path(temp); src=temp/'input.c'; exe=temp/'input.exe'; batch=temp/'run.cmd'
    src.write_text((tests/'test_peer_input.c').read_text(encoding='utf-8').replace('/* PRODUCTION */',functions),encoding='utf-8')
    vc='C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'
    batch.write_text(f'@echo off\ncall "{vc}" >nul && cl /nologo /std:c11 /W3 "{src}" /Fo:"{temp/"input.obj"}" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"\n',encoding='utf-8')
    subprocess.run(['cmd.exe','/d','/c',str(batch)],cwd=tests,check=True)
