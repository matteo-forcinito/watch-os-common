#ifndef INTERCOM_TEST_PORTMACRO_H
#define INTERCOM_TEST_PORTMACRO_H
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
void test_lock_enter(void);
void test_lock_exit(void);
#define portENTER_CRITICAL(mux) ((void)(mux), test_lock_enter())
#define portEXIT_CRITICAL(mux) ((void)(mux), test_lock_exit())
#endif
