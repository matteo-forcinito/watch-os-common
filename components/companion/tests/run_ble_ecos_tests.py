"""Run the production EC01 BLE adapter with real cJSON and SDK fault stubs."""
from pathlib import Path
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
workspace = tests.parents[3]
cjson_sources = list(workspace.glob('*/managed_components/espressif__cjson/cJSON/cJSON.c'))
if not cjson_sources:
    raise FileNotFoundError('Install the ESP-IDF espressif__cjson managed component to run this test')
cjson_source = cjson_sources[0]
cjson_include = cjson_source.parent
with tempfile.TemporaryDirectory(prefix='ble-ecos-host-') as temp_name:
    temp = Path(temp_name)
    exe = temp / 'ble_ecos_tests.exe'
    obj = temp / 'ble_ecos_tests.obj'
    cjson_obj = temp / 'cjson.obj'
    batch = temp / 'run.cmd'
    vcvars = Path('C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat')
    command = (
        f'call "{vcvars}" >nul && cl /nologo /std:c11 /W3 /I"{tests / "ble_ecos_stubs"}" '
        f'/I"{tests.parent}" /I"{cjson_include}" /c "{tests / "test_ble_ecos.c"}" /Fo:"{obj}" && '
        f'cl /nologo /std:c11 /W3 /I"{cjson_include}" /c "{cjson_source}" /Fo:"{cjson_obj}" && '
        f'link /nologo /OUT:"{exe}" "{obj}" "{cjson_obj}" && "{exe}"'
    )
    batch.write_text('@echo off\n' + command + '\n')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=tests, check=True)
