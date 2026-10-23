/* Actual asynchronous checkpoint backend with a fault-injecting NVS. */
#define _CRT_SECURE_NO_WARNINGS
#include "../components/apps/minigames/minigame_progress.c"
#include <assert.h>
static checkpoint_t durable[PROGRESS_SLOTS], pending;
static size_t packed_bytes;
static char keys[PROGRESS_SLOTS][16];
static int key_count, pending_key, fail_commit, queue_full, count;
static struct { void (*fn)(void *); void *arg; } jobs[32];
static const char *test_directory;
static int files_busy;
const char *storage_dir(void) { return test_directory; }
bool storage_files_acquire(void) { return !files_busy; }
void storage_files_release(void) {}
int nvs_erase_key(int handle, const char *key) { (void)handle; (void)key; return 1; }
int nvs_open(const char *name, int mode, int *handle) { (void)name; (void)mode; *handle = 1; return 0; }
int nvs_get_blob(int handle, const char *key, void *data, size_t *size)
{
    (void)handle;
    for (int i = 0; i < key_count; i++) if (!strcmp(keys[i], key))
    { assert(*size >= sizeof(checkpoint_t)); *size = pack(&durable[i], data); return 0; }
    return 1;
}
int nvs_set_blob(int handle, const char *key, const void *data, size_t size)
{
    (void)handle;
    assert(size <= sizeof(checkpoint_t) + 32);
    int i;
    for (i = 0; i < key_count && strcmp(keys[i], key); i++) {}
    if (i == key_count) { assert(key_count < PROGRESS_SLOTS); snprintf(keys[key_count++], 16, "%s", key); }
    pending_key = i; assert(unpack(&pending, data, size)); packed_bytes = size; return 0;
}
int nvs_commit(int handle) { (void)handle; if (fail_commit) return 9; durable[pending_key] = pending; return 0; }
void nvs_close(int handle) { (void)handle; }
bool sys_post(void (*fn)(void *), void *arg)
{
    if (queue_full) return false;
    assert(count < 32); jobs[count].fn = fn; jobs[count++].arg = arg; return true;
}
static void drain(void)
{
    for (int i = 0; i < count; i++) jobs[i].fn(jobs[i].arg);
    count = 0;
}
static void boot(void)
{
    assert(!count); free(s_slots); s_slots = NULL; mg_progress_init(); assert(s_slots);
}
int main(void)
{
    const mg_game_t *g = pet_game_find("archer"); assert(g);
    mg_session_t *a = mg_create(g, 7, 0), *b = mg_create(g, 7, 0); assert(a && b);
    boot(); assert(!mg_progress_load(g, mg_state(b)));
    mg_advance(a, 160, true);
    assert(mg_progress_save(g, mg_state(a)) && count == 1 && !key_count);
    assert(mg_progress_save(g, mg_state(a)) && count == 1);
    assert(mg_progress_load(g, mg_state(b)));
    drain(); assert(key_count == 1 && !mg_progress_failed(g));
    assert(packed_bytes < sizeof(checkpoint_t) / 2);
    /* Full random payload, sparse payload, malformed streams and legacy records. */
    checkpoint_t raw = {0}, decoded;
    uint8_t encoded[sizeof(checkpoint_t) + 32];
    raw.magic = PROGRESS_MAGIC; raw.size = MG_CHECKPOINT_BYTES; raw.version = 1;
    for (unsigned i = 0; i < raw.size; i++) raw.data[i] = (uint8_t)(i * 37);
    raw.checksum = checksum(&raw);
    size_t bytes = pack(&raw, encoded);
    assert(unpack(&decoded, encoded, bytes) && !memcmp(&raw, &decoded, sizeof(raw)));
    assert(!unpack(&decoded, encoded, bytes - 1));
    assert(unpack(&decoded, (const uint8_t *)&raw, sizeof(raw)));
    memset(raw.data, 0, sizeof(raw.data)); raw.checksum = checksum(&raw);
    bytes = pack(&raw, encoded);
    assert(bytes < 32 && unpack(&decoded, encoded, bytes) && !memcmp(&raw, &decoded, sizeof(raw)));
    checkpoint_t previous = durable[0];
    boot(); assert(mg_progress_load(g, mg_state(b)));
    mg_advance(a, 320, true); fail_commit = 1;
    assert(mg_progress_save(g, mg_state(a))); drain();
    assert(mg_progress_failed(g) && !memcmp(&durable[0], &previous, sizeof(previous)));
    boot(); assert(mg_progress_load(g, mg_state(b))); fail_commit = 0;
    queue_full = 1; assert(!mg_progress_save(g, mg_state(a))); queue_full = 0;
    assert(mg_progress_save(g, mg_state(a))); drain(); assert(!mg_progress_failed(g));
    boot(); assert(mg_progress_load(g, mg_state(b)));
    durable[0].data[8] ^= 1;
    boot(); assert(!mg_progress_load(g, mg_state(b)));
    assert(mg_progress_save(g, mg_state(a))); drain();
    durable[0].version++;
    boot(); assert(!mg_progress_load(g, mg_state(b)));
    durable[0].version--; /* Restore a valid legacy save before file migration. */
    test_directory = "progress-files";
    _mkdir(test_directory);
    boot(); assert(count == 1); /* Migration is queued before a game is opened. */
    assert(mg_progress_save(g, mg_state(a))); drain();
    assert(!mg_progress_failed(g));
    boot(); assert(mg_progress_load(g, mg_state(b)));
    mg_advance(a, 480, true);
    assert(mg_progress_save(g, mg_state(a))); drain();
    assert(s_slots[find(g)].disk_sequence == 2);
    char path[128]; snprintf(path, sizeof(path), "%s/game-saves/%s.0", test_directory, g->id);
    FILE *broken = fopen(path, "wb"); assert(broken); fputc(0, broken); fclose(broken);
    boot(); assert(mg_progress_load(g, mg_state(b)) && s_slots[find(g)].disk_sequence == 1);
    files_busy = 1; assert(mg_progress_save(g, mg_state(a))); drain();
    assert(mg_progress_failed(g)); files_busy = 0;
    assert(mg_progress_save(g, mg_state(a))); drain(); assert(!mg_progress_failed(g));
    snprintf(path, sizeof(path), "%s/game-saves/%s.0", test_directory, g->id); remove(path);
    snprintf(path, sizeof(path), "%s/game-saves/%s.1", test_directory, g->id); remove(path);
    mg_destroy(a); mg_destroy(b); free(s_slots);
    puts("PASS: compressed checkpoints, legacy import, truncated data, deferred/coalesced writes, reboot and storage failures");
    return 0;
}
