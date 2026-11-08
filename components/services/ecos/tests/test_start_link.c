#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef int portMUX_TYPE;
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define SYS_CORE_ANY 0

enum { CMD_NONE, CMD_ORBIT };
static portMUX_TYPE s_lock;
static int s_cmd;
static bool s_orbit_waiting, s_running, s_suspend;
static bool disabled, task_create_result = true;
static int create_calls, message_calls;
static const char *last_message;

static bool family_disabled(void) { return disabled; }
static void link_task(void *arg) { (void)arg; }
static bool sys_task_create(void (*fn)(void *), const char *name, int stack,
                            void *arg, int priority, int core)
{
    (void)fn; (void)name; (void)stack; (void)arg; (void)priority; (void)core;
    create_calls++;
    return task_create_result;
}
static void set_message(const char *message, const char *arg)
{ (void)arg; message_calls++; last_message = message; }

static void start_link(void);

static void reset(void)
{
    s_cmd = CMD_NONE;
    s_orbit_waiting = s_running = s_suspend = disabled = false;
    task_create_result = true;
    create_calls = message_calls = 0;
    last_message = NULL;
}

static void test_disabled_policy_cancels_pending_orbit(void)
{
    reset();
    disabled = true;
    s_cmd = CMD_ORBIT;
    s_orbit_waiting = true;
    start_link();
    assert(s_cmd == CMD_NONE && !s_orbit_waiting && !s_running);
    assert(create_calls == 0);
}

static void test_suspension_without_worker_cancels_pending_orbit(void)
{
    reset();
    s_suspend = true;
    s_cmd = CMD_ORBIT;
    s_orbit_waiting = true;
    start_link();
    assert(s_cmd == CMD_NONE && !s_orbit_waiting && !s_running);
    assert(create_calls == 0);
}

static void test_task_creation_failure_cancels_and_clears_running(void)
{
    reset();
    s_cmd = CMD_ORBIT;
    s_orbit_waiting = true;
    task_create_result = false;
    start_link();
    assert(create_calls == 1 && !s_running);
    assert(s_cmd == CMD_NONE && !s_orbit_waiting);
    assert(message_calls == 1 && last_message != NULL);
}

static void test_successful_start_marks_running_once(void)
{
    reset();
    start_link();
    assert(create_calls == 1 && s_running);
    start_link();
    assert(create_calls == 1 && s_running);
}

static void test_already_running_preserves_inflight_orbit(void)
{
    reset();
    s_running = true;
    s_cmd = CMD_ORBIT;
    s_orbit_waiting = true;
    start_link();
    assert(create_calls == 0 && s_running);
    assert(s_cmd == CMD_ORBIT && s_orbit_waiting);
}

int main(void)
{
    test_disabled_policy_cancels_pending_orbit();
    test_suspension_without_worker_cancels_pending_orbit();
    test_task_creation_failure_cancels_and_clears_running();
    test_successful_start_marks_running_once();
    test_already_running_preserves_inflight_orbit();
    puts("ECOS start_link tests passed");
    return 0;
}
