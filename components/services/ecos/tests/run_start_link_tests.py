"""Extract and test the production ECOS start_link function with MSVC stubs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--vcvars', type=Path, default=Path(
    'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
args = parser.parse_args()
test_dir = Path(__file__).resolve().parent
source = test_dir.parent / 'ecos_link.c'


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


body = extract_function(source.read_text(encoding='utf-8'), 'static void start_link(void)')
test_file = (test_dir / 'test_start_link.c').as_posix()
with tempfile.TemporaryDirectory(prefix='ecos-start-link-') as temp:
    temp = Path(temp)
    exe = temp / 'ecos_start_link_tests.exe'
    obj_dir = temp / 'obj'
    obj_dir.mkdir()
    harness = temp / 'extracted_ecos_link.c'
    harness.write_text(
        '#include "test_start_link.c"\n'
        + body + '\n',
        encoding='utf-8',
    )
    batch = temp / 'run.cmd'
    command = (
        f'call "{args.vcvars}" >nul && '
        f'cl /nologo /std:c11 /W3 /I"{test_dir}" '
        f'/Fo:"{obj_dir.as_posix()}/" /Fe:"{exe}" '
        f'"{harness}" /link /OUT:"{exe}" && "{exe}"'
    )
    batch.write_text('@echo off\n' + command + '\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=test_dir, check=True)
