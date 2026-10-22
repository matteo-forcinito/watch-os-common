"""Read-only Windows ESP-IDF build monitor. No third-party dependencies."""
import argparse
import datetime
import json
from pathlib import Path
import re
import subprocess
import time


def processes():
    command = ("Get-CimInstance Win32_Process | Where-Object { "
               "$_.Name -match 'python|ninja|cmake|xtensa|ccache' } | "
               "Select-Object ProcessId,ParentProcessId,Name,CommandLine | ConvertTo-Json -Compress")
    result = subprocess.run(['powershell.exe', '-NoProfile', '-Command', command],
                            capture_output=True, text=True, timeout=30)
    if result.returncode:
        raise RuntimeError(result.stderr.strip())
    data = json.loads(result.stdout or '[]')
    return data if isinstance(data, list) else [data]


def tail(path):
    # Read only the end, even for multi-megabyte build logs.
    with path.open('rb') as stream:
        stream.seek(max(0, path.stat().st_size - 131072))
        return stream.read().decode('utf-8', errors='replace')


def inspect(project, build_name, procs):
    build = project / build_name
    logs = list((build / 'log').glob('idf_py_*_output_*'))
    if not logs:
        return 'NON RILEVATA', '-', '-', 'Nessun log ESP-IDF'
    # A fresh empty log supersedes any previous successful build.
    latest = max(logs, key=lambda p: p.stat().st_mtime)
    pid = latest.name.rsplit('_', 1)[-1]
    current = [p for p in logs if p.name.endswith('_' + pid)]
    stdout = '\n'.join(tail(p) for p in current if 'stdout' in p.name)
    stderr = '\n'.join(tail(p) for p in current if 'stderr' in p.name)
    text = stdout + '\n' + stderr
    age = int(time.time() - max(p.stat().st_mtime for p in current))
    key = str(project.resolve()).replace('\\', '/').lower()
    active = [p for p in procs if str(p['ProcessId']) == pid or
              key in (p.get('CommandLine') or '').replace('\\', '/').lower()]
    steps = re.findall(r'\[(\d+)/(\d+)\]', stdout)
    progress = '-'
    if steps:
        done, total = map(int, steps[-1])
        progress = f'{done}/{total} ({100 * done / total:.1f}%)' if total else '-'
    errors = re.findall(r'^.*(?:CMake Error|fatal error:|error:|FAILED:|ninja: build stopped).*$',
                        text, flags=re.MULTILINE)
    # End-of-build partition check follows image creation and copying.
    complete = bool(re.search(r'binary size .*Smallest app partition', stdout))
    if active:
        status = 'COMPILAZIONE' if steps else 'CONFIGURAZIONE'
        if errors:
            status = 'ATTIVA CON ERRORI'
    elif errors:
        status = 'ERRORE'
    elif complete:
        status = 'COMPLETATA'
    else:
        status = 'ESITO NON CERTO'
    detail = errors[-1].strip() if errors else next(
        (line.strip() for line in reversed(stdout.splitlines()) if line.strip()),
        'Log ancora vuoto')
    return status, progress, f'{age}s', detail


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path,
                        default=Path(__file__).resolve().parents[2] / '.release-work')
    parser.add_argument('--build-dir', default='build-release')
    parser.add_argument('--watch', action='store_true', help='Refresh until Ctrl+C')
    parser.add_argument('--interval', type=int, default=10)
    args = parser.parse_args()
    if args.interval < 1:
        parser.error('--interval must be positive')
    if not args.root.is_dir():
        parser.error('Root directory does not exist: ' + str(args.root))
    try:
        while True:
            procs = processes()
            print('\n' + datetime.datetime.now().astimezone().isoformat(timespec='seconds'))
            print(f'{"PROGETTO":35} {"STATO":20} {"PASSI NINJA":23} ETA LOG')
            projects = sorted(p for p in args.root.iterdir()
                              if p.is_dir() and (p / args.build_dir).is_dir())
            for project in projects:
                status, progress, age, detail = inspect(project, args.build_dir, procs)
                print(f'{project.name:35} {status:20} {progress:23} {age}')
                print('  ' + detail[:240])
            if not projects:
                print('Nessuna cartella build trovata.')
            print('Percentuale = passi Ninja, non tempo residuo. Log vecchio non significa blocco.')
            unattributed = [p for p in procs if 'idf.py' in (p.get('CommandLine') or '')]
            if unattributed:
                print('Processi idf.py presenti (anche wrapper): ' +
                      ', '.join(str(p['ProcessId']) for p in unattributed))
            if not args.watch:
                break
            time.sleep(args.interval)
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
