"""Compile the production ecos_family_orbit() body against deterministic host stubs."""
from pathlib import Path
import subprocess
import tempfile

TESTS = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ECOS_ORBIT_MAX 144
typedef enum { CMD_NONE, CMD_ORBIT, CMD_OTHER } cmd_kind_t;
typedef int portMUX_TYPE;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define pdMS_TO_TICKS(ms) (ms)

static portMUX_TYPE s_lock;
static bool s_orbit_calling, s_orbit_waiting, s_orbit_processing;
static bool s_suspend, s_busy, s_running, s_disabled;
static cmd_kind_t s_cmd;
static uint8_t s_orbit_request[ECOS_ORBIT_MAX], s_orbit_reply[ECOS_ORBIT_MAX];
static size_t s_orbit_length;
static uint64_t s_orbit_request_id;
static uint8_t s_orbit_mac[6], s_cmd_mac[6];
static int s_orbit_result = -1;
static int64_t now_us;
static int start_mode, delay_count, start_count, random_count;
enum { START_FAIL, START_ASYNC_REPLY, START_NO_CLAIM, START_BUSY_WORKER, START_OVERSIZE_REPLY };

static bool parse_id(const char *id, uint8_t mac[6])
{
    if (id == NULL || strlen(id) != 12) return false;
    for (int i = 0; i < 12; ++i)
    {
        char c = id[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    for (int i = 0; i < 6; ++i) mac[i] = (uint8_t)(i + 1);
    return true;
}
static bool family_disabled(void) { return s_disabled; }
static void esp_fill_random(void *out, size_t size)
{
    ++random_count;
    memset(out, 0x5a, size);
}
static int64_t esp_timer_get_time(void) { return now_us; }
static void start_link(void)
{
    ++start_count;
    if (start_mode == START_FAIL)
    {
        s_running = false;
        s_cmd = CMD_NONE;
        s_orbit_waiting = false;
    }
    else if (start_mode == START_ASYNC_REPLY)
    {
        memcpy(s_orbit_reply, "ACK", 3);
        s_orbit_result = 3;
    }
    else if (start_mode == START_BUSY_WORKER)
    {
        s_cmd = CMD_NONE;
        s_busy = true;
        s_orbit_processing = true;
    }
    else if (start_mode == START_OVERSIZE_REPLY)
    {
        memcpy(s_orbit_reply, "TOOLARGE", 8);
        s_orbit_result = 8;
        s_cmd = CMD_NONE;
        s_orbit_waiting = false;
    }
}
static void vTaskDelay(int ticks)
{
    ++delay_count;
    now_us += (int64_t)ticks * 1000;
    if (start_mode == START_ASYNC_REPLY)
    {
        if (delay_count == 1) { s_orbit_waiting = false; s_orbit_processing = true; }
        else if (delay_count == 2) s_orbit_processing = false;
    }
}
'''


def extract_function(source: str) -> str:
    signature = "int ecos_family_orbit("
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[start:index + 1]
    raise RuntimeError("unterminated ecos_family_orbit() body")


MAIN = r'''
int main(void)
{
    const char *peer = "010203040506";
    const uint8_t request[] = {1, 2, 3};
    uint8_t reply[16];

    /* Public argument and bounds validation is side-effect free. */
    assert(ecos_family_orbit(NULL, request, sizeof(request), reply, sizeof(reply), 100) == -1);
    assert(ecos_family_orbit("bad", request, sizeof(request), reply, sizeof(reply), 100) == -1);
    assert(ecos_family_orbit(peer, NULL, sizeof(request), reply, sizeof(reply), 100) == -1);
    assert(ecos_family_orbit(peer, request, 0, reply, sizeof(reply), 100) == -1);
    assert(ecos_family_orbit(peer, request, ECOS_ORBIT_MAX + 1, reply, sizeof(reply), 100) == -1);
    assert(ecos_family_orbit(peer, request, sizeof(request), reply, 0, 100) == -1);
    assert(ecos_family_orbit(peer, request, sizeof(request), reply, sizeof(reply), 0) == -1);
    assert(ecos_family_orbit(peer, request, sizeof(request), reply, sizeof(reply), 30001) == -1);
    assert(start_count == 0 && random_count == 0);

    /* A reply arriving before processing finishes must not release the slot. */
    memset(reply, 0xcc, sizeof(reply));
    start_mode = START_ASYNC_REPLY;
    assert(ecos_family_orbit(peer, request, sizeof(request), reply, sizeof(reply), 100) == 3);
    assert(delay_count == 2 && memcmp(reply, "ACK", 3) == 0);
    assert(!s_orbit_calling && !s_orbit_waiting && !s_orbit_processing && s_cmd == CMD_NONE);

    /* A queued command that the worker never claims is cancelled at timeout. */
    delay_count = 0; now_us = 0; start_mode = START_NO_CLAIM;
    assert(ecos_family_orbit(peer, request, sizeof(request), reply, sizeof(reply), 40) == -1);
    assert(delay_count == 2 && s_cmd == CMD_NONE && !s_orbit_waiting && !s_orbit_calling);

    /* A claimed worker remains busy after timeout; the next call cannot overwrite its slot. */
    delay_count = 0; now_us = 0; start_mode = START_BUSY_WORKER;
    assert(ecos_family_orbit(peer, request, sizeof(request), reply, sizeof(reply), 40) == -1);
    assert(s_busy && s_orbit_processing && !s_orbit_waiting && !s_orbit_calling);
    uint8_t saved_request[sizeof(s_orbit_request)], saved_mac[sizeof(s_orbit_mac)];
    memcpy(saved_request, s_orbit_request, sizeof(saved_request));
    memcpy(saved_mac, s_orbit_mac, sizeof(saved_mac));
    int starts = start_count;
    assert(ecos_family_orbit("0a0b0c0d0e0f", request, sizeof(request), reply, sizeof(reply), 40) == -1);
    assert(start_count == starts && memcmp(saved_request, s_orbit_request, sizeof(saved_request)) == 0);
    assert(memcmp(saved_mac, s_orbit_mac, sizeof(saved_mac)) == 0);

    /* Oversized acknowledgement is rejected without copying a partial reply. */
    s_busy = false; s_orbit_processing = false; delay_count = 0; now_us = 0;
    start_mode = START_OVERSIZE_REPLY;
    memset(reply, 0xcc, sizeof(reply));
    assert(ecos_family_orbit(peer, request, sizeof(request), reply, 4, 100) == -1);
    for (size_t i = 0; i < sizeof(reply); ++i) assert(reply[i] == 0xcc);

    /* Failure to start the transport resolves immediately and releases the slot. */
    s_orbit_result = -1; s_orbit_waiting = false; s_orbit_processing = false;
    s_cmd = CMD_NONE; s_busy = false; delay_count = 0; now_us = 0;
    start_mode = START_FAIL;
    assert(ecos_family_orbit(peer, request, sizeof(request), reply, sizeof(reply), 100) == -1);
    assert(start_count == starts + 2 && !s_running && s_cmd == CMD_NONE);
    assert(!s_orbit_calling && !s_orbit_waiting && !s_orbit_processing);

    puts("PASS: Orbit call validation, async completion, cancellation, busy slot, oversized reply and start failure");
    return 0;
}
'''

test_dir = Path(__file__).resolve().parent
source_file = test_dir.parent / "ecos_link.c"
source = source_file.read_text(encoding="utf-8")
function = extract_function(source)
with tempfile.TemporaryDirectory(prefix="ecos-orbit-call-") as temp:
    temp = Path(temp)
    c_file = temp / "orbit_call_tests.c"
    exe = temp / "orbit_call_tests.exe"
    batch = temp / "run.cmd"
    c_file.write_text(TESTS + "\n" + function + "\n" + MAIN, encoding="utf-8")
    vcvars = Path("C:/Program Files/Microsoft Visual Studio/18/Community/VC/Auxiliary/Build/vcvars64.bat")
    command = f'call "{vcvars}" >nul && cl /nologo /std:c11 /W4 "{c_file}" /Fo:"{temp / "call.obj"}" /Fe:"{exe}" /link /OUT:"{exe}" && "{exe}"'
    batch.write_text("@echo off\n" + command + "\n", encoding="utf-8")
    subprocess.run(["cmd.exe", "/d", "/c", str(batch)], cwd=test_dir, check=True)
