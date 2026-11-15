/* Minimal XNU surface used by bsd/dev/sweet_touch.c, for host tests only. */
#ifndef KSTUB_H
#define KSTUB_H
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

#ifndef ENOTSUP
#define ENOTSUP 45
#endif

typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
typedef int kern_return_t;
typedef int wait_result_t;
typedef struct fake_thread *thread_t;
typedef void (*thread_continue_t)(void *, wait_result_t);

#define KERN_SUCCESS          0
#define NSEC_PER_MSEC         1000000ull
#define NSEC_PER_SEC          1000000000ull
#define THREAD_UNINT          0
#define THREAD_CONTINUE_NULL  ((thread_continue_t)0)

/* Provided by the harness. */
void clock_interval_to_deadline(uint32_t interval, uint64_t scale, uint64_t *deadline);
wait_result_t assert_wait_deadline(void *event, int interruptible, uint64_t deadline);
wait_result_t thread_block(thread_continue_t cont);
void delay(int usec);
int PE_parse_boot_argn(const char *arg, void *ptr, int max_len);
kern_return_t kernel_thread_start(thread_continue_t fn, void *arg, thread_t *out);
void thread_deallocate(thread_t t);
/* Time (kern/clock.h): the harness returns a fake monotonic counter. */
uint64_t mach_absolute_time(void);
void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
void nanoseconds_to_absolutetime(uint64_t nanoseconds, uint64_t *result);
thread_t current_thread(void);
#endif
