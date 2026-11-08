"""Exercise production alarm storage operations with failing NVS and sys queue."""
from pathlib import Path
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
source = (tests.parent / "alarm.c").read_text()
def extract(signature):
    start = source.index(signature)
    body = source.index("{", start)
    depth = 0
    for index in range(body, len(source)):
        if source[index] == "{": depth += 1
        elif source[index] == "}":
            depth -= 1
            if not depth: return source[start:index + 1]
    raise AssertionError(signature)

signatures = ["static esp_err_t save(void)", "static uint32_t new_id(void)",
              "static void load(void)", "esp_err_t alarm_save(",
              "esp_err_t alarm_delete(", "static void timer_cb(void *arg)\n{"]
fixture = '#define STATE_ALARM_VERSION 1\n#include "alarm_stubs.h"\nstatic void check_job(void *arg) {(void)arg;}\n'
fixture += "\n".join(extract(x) for x in signatures)
fixture += "\n" + (tests / "test_alarm_persistence.c").read_text()
with tempfile.TemporaryDirectory(prefix="alarm-tests-") as directory:
    temp = Path(directory)
    unit = temp / "alarm.c"
    unit.write_text(fixture)
    exe = temp / "alarm.exe"
    batch = temp / "run.cmd"
    vcvars = "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat"
    batch.write_text(f'@echo off\ncall "{vcvars}" >nul && cl /nologo /std:c11 /W3 /I"{tests}" /I"{tests / "stubs"}" /I"{tests.parent.parent / "include"}" "{unit}" /Fo:"{temp / "alarm.obj"}" /Fe:"{exe}" && "{exe}"\n')
    subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=tests, check=True)
