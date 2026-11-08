"""Extract and test production timer_remote/watch-toggle code with MSVC stubs."""
from pathlib import Path
import subprocess
import tempfile

test_dir = Path(__file__).resolve().parent
source = test_dir.parent / 'timer_app.c'


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
                state = 'code'; i += 1
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


contents = source.read_text(encoding='utf-8')
functions = '\n\n'.join(extract_function(contents, signature) for signature in (
    'static void on_watch_toggle(lv_event_t *e)',
    'bool timer_remote(const char *action, int seconds)',
))

with tempfile.TemporaryDirectory(prefix='timer-remote-') as temp:
    temp = Path(temp)
    exe = temp / 'timer_remote_tests.exe'
    obj_dir = temp / 'obj'
    obj_dir.mkdir()
    harness = temp / 'extracted_timer.c'
    harness.write_text(
        '#include "timer_stubs.h"\n'
        '#include <assert.h>\n#include <string.h>\n'
        'struct stub_timer timer_instance;\n'
        'int64_t fake_now_us;\n'
        'esp_err_t create_result, stop_result, start_result;\n'
        'int create_calls, stop_calls, start_calls, alarm_stop_calls, watch_label_calls;\n'
        'uint64_t last_start_us;\n'
        'stopwatch_t s_watch;\n'
        'esp_timer_handle_t s_countdown;\n'
        'int64_t s_countdown_end_us;\n'
        'int s_countdown_minutes = 5;\n'
        'int64_t s_countdown_duration_us;\n'
        'lv_obj_t *s_watch_button;\n'
        'int64_t esp_timer_get_time(void) { return fake_now_us; }\n'
        'esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out) { assert(args && args->callback && strcmp(args->name, "countdown") == 0); ++create_calls; if (create_result == ESP_OK) *out = &timer_instance; return create_result; }\n'
        'esp_err_t esp_timer_stop(esp_timer_handle_t timer) { assert(timer); ++stop_calls; return stop_result; }\n'
        'esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t us) { assert(timer); ++start_calls; last_start_us = us; return start_result; }\n'
        'void sound_alarm_stop(void) { ++alarm_stop_calls; }\n'
        'lv_obj_t *lv_obj_get_child(lv_obj_t *obj, int index) { (void)obj; (void)index; return NULL; }\n'
        'void lv_label_set_text(lv_obj_t *obj, const char *text) { assert(obj == NULL && text); ++watch_label_calls; }\n'
        'static void countdown_done(void *arg) { (void)arg; }\n'
        + functions + '\n'
        + f'#include "{(test_dir / "test_timer_remote.c").as_posix()}"\n',
        encoding='utf-8')
    batch = temp / 'run.cmd'
    command = (
        'call "C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat" >nul && '
        f'cl /nologo /std:c11 /W3 /I"{test_dir}" /Fo:"{obj_dir.as_posix()}/" '
        f'/Fe:"{exe}" "{harness}" /link /OUT:"{exe}" && "{exe}"'
    )
    batch.write_text('@echo off\n' + command + '\n', encoding='utf-8')
    subprocess.run(['cmd.exe', '/d', '/c', str(batch)], cwd=test_dir, check=True)
