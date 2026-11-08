"""Extract and test the production Wi-Fi lifecycle functions with MSVC stubs."""
import argparse
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--vcvars', type=Path, default=Path(
    'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat'))
args = parser.parse_args()
test_dir = Path(__file__).resolve().parent
source = test_dir.parent / 'wifi.c'


def extract_function(text: str, signature: str) -> str:
    """Return one complete C function, counting braces outside comments/quotes."""
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
            if c == '*' and n == '/':
                state = 'code'
                i += 1
        elif state in ('string', 'char'):
            if escaped:
                escaped = False
            elif c == '\\':
                escaped = True
            elif (state == 'string' and c == '"') or (state == 'char' and c == "'"):
                state = 'code'
        else:
            if c == '/' and n == '/':
                state = 'line'; i += 1
            elif c == '/' and n == '*':
                state = 'block'; i += 1
            elif c == '"': state = 'string'
            elif c == "'": state = 'char'
            elif c == '{': depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    return text[start:i + 1]
        i += 1
    raise ValueError(f'unclosed C body for {signature}')


contents = source.read_text(encoding='utf-8')
signatures = (
    'static bool driver_up(void)',
    'static void driver_down(void)',
    'static bool radio_on(wifi_mode_t mode)',
    'static void radio_off(void)',
    'static void off_job(void *arg)',
    'static void off_timer_cb(void *arg)',
    'static void known_load(void)',
    'static bool intercom_channel_reserved(void)',
    'static esp_err_t join(const char *ssid, const char *password, uint32_t timeout_ms)',
    'static wifi_ap_record_t *scan_run(bool hidden, uint16_t *found)',
    'esp_err_t wifi_acquire(uint32_t timeout_ms)',
    'void wifi_radio_release(void)',
    'void wifi_release(void)',
    'void wifi_release_now(void)',
    'bool wifi_radio_set_channel(uint8_t channel)',
    'bool wifi_radio_channel_reserve(const char owner[4], uint8_t channel)',
    'void wifi_radio_channel_release(const char owner[4])',
    'uint8_t wifi_radio_pinned_channel(void)',
    'uint8_t wifi_radio_common_channel(void)',
    'bool wifi_radio_common_channel_set(uint8_t channel)',
)
functions = '\n\n'.join(extract_function(contents, signature) for signature in signatures)
test_file = (test_dir / 'test_driver_lifecycle.c').as_posix()
with tempfile.TemporaryDirectory(prefix='wifi-driver-') as temp:
    temp = Path(temp)
    exe = temp / 'wifi_driver_tests.exe'
    obj_dir = temp / 'obj'
    obj_dir.mkdir()
    harness = temp / 'extracted_wifi.c'
    harness.write_text(
        '#include "wifi_stubs.h"\n'
        '#define TAG "wifi-test"\n'
        'static void driver_down(void);\n'
        + functions + '\n'
        + f'#include "{test_file}"\n',
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
