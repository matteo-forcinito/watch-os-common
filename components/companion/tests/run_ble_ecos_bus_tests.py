"""Compile and run the production ECOS BLE bus bridge with real cJSON."""
from pathlib import Path
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
component = tests.parent
workspace = tests.parents[3]
cjson_sources = list(workspace.glob('*/managed_components/espressif__cjson/cJSON/cJSON.c'))
if not cjson_sources:
    raise FileNotFoundError('Install the ESP-IDF espressif__cjson managed component to run this test')
cjson_source = cjson_sources[0]
cjson_include = cjson_source.parent
with tempfile.TemporaryDirectory(prefix='ble-ecos-bus-host-') as temp_name:
    temp = Path(temp_name)
    exe = temp / 'ble_ecos_bus_tests.exe'
    obj_dir = temp / 'obj'
    obj_dir.mkdir()
    batch = temp / 'run.cmd'
    vcvars = Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat')
    command = (
        f'call "{vcvars}" >nul && cl /nologo /std:c11 /W3 /D CJSON_NESTING_LIMIT=1000 '
        f'/I"{tests / "ble_ecos_bus_stubs"}" /I"{component}" /I"{cjson_include}" '
        f'"{tests / "test_ble_ecos_bus.c"}" "{component / "ble_ecos_bus.c"}" "{cjson_source}" '
        f'/Fo:"{obj_dir.as_posix()}/" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"'
    )
    batch.write_text('@echo off\n' + command + '\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=tests, check=True)
