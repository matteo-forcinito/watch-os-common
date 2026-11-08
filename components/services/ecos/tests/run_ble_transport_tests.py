"""Compile the production EL02 BLE bridge functions against deterministic SDK stubs."""
from pathlib import Path
import subprocess
import tempfile

tests = Path(__file__).resolve().parent
source = (tests.parent / "ecos_link.c").read_text(encoding="utf-8")

def function(signature):
    begin = source.index(signature)
    brace = source.index("{", begin)
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[begin:i + 1]
    raise AssertionError(signature)

production = "\n\n".join(function(signature) for signature in (
    "static void id_text(", "static bool queue_frame(", "static QueueHandle_t unpublish_rx(",
    "static int ble_send_bound(", "static int send_frame(",
    "static bool ble_termination_required(",
    "bool ecos_family_ble_open(", "bool ecos_family_ble_receive(",
    "bool ecos_family_companion(", "void ecos_family_ble_close("))
test_source = (tests / "test_ble_transport.c").read_text(encoding="utf-8")
test_source = test_source.replace(
    '#include "../ecos_link_wire.h"',
    '#include "' + (tests.parent / "ecos_link_wire.h").as_posix() + '"',
)
test_source = test_source.replace("/* PRODUCTION */", production)

with tempfile.TemporaryDirectory(prefix="ecos-ble-transport-") as directory:
    directory = Path(directory)
    cfile = directory / "test.c"
    cfile.write_text(test_source, encoding="utf-8")
    exe = directory / "test.exe"
    batch = directory / "run.cmd"
    vcvars = "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat"
    batch.write_text(
        f'@echo off\ncall "{vcvars}" >nul && cl /nologo /std:c11 /W3 "{cfile}" '
        f'/Fo:"{directory / "test.obj"}" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"\n',
        encoding="utf-8",
    )
    subprocess.run(["cmd.exe", "/d", "/c", str(batch)], check=True)
