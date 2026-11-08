#pragma once
#include <stdbool.h>
#include <stdint.h>
bool sys_post(void (*fn)(void *), void *arg);
typedef void (*TaskFunction_t)(void *);
typedef int BaseType_t; typedef unsigned UBaseType_t;
#define SYS_CORE_ANY 0
bool sys_task_create(TaskFunction_t,const char *,uint32_t,void *,UBaseType_t,BaseType_t);
