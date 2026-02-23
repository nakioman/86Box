#ifdef SUBSYS_PROFILE
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#endif
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>
#include <86box/86box.h>
#include "cpu.h"
#include <86box/timer.h>
#include <86box/nv/vid_nv_rivatimer.h>

#ifdef SUBSYS_PROFILE
#include <time.h>
#ifndef _WIN32
#include <dlfcn.h>
#endif

static inline uint64_t
prof_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

#define PROF_MAX_CBS 64

typedef struct {
    void    (*callback)(void *);
    uint64_t total_ns;
    uint32_t call_count;
} prof_cb_entry_t;

static prof_cb_entry_t prof_cb_entries[PROF_MAX_CBS];
static int             prof_cb_count = 0;
uint64_t               prof_timer_total_ns = 0;

static prof_cb_entry_t *
prof_cb_find(void (*cb)(void *))
{
    for (int i = 0; i < prof_cb_count; i++) {
        if (prof_cb_entries[i].callback == cb)
            return &prof_cb_entries[i];
    }
    if (prof_cb_count < PROF_MAX_CBS) {
        prof_cb_entry_t *e = &prof_cb_entries[prof_cb_count++];
        e->callback  = cb;
        e->total_ns  = 0;
        e->call_count = 0;
        return e;
    }
    return NULL;
}

void
prof_timer_print_reset(void)
{
    if (!prof_cb_count)
        return;

    /* Sort descending by total_ns. */
    for (int i = 0; i < prof_cb_count - 1; i++) {
        for (int j = i + 1; j < prof_cb_count; j++) {
            if (prof_cb_entries[j].total_ns > prof_cb_entries[i].total_ns) {
                prof_cb_entry_t tmp = prof_cb_entries[i];
                prof_cb_entries[i]  = prof_cb_entries[j];
                prof_cb_entries[j]  = tmp;
            }
        }
    }

    fprintf(stderr, "--- Device Callback Breakdown (top 15 by time) ---\n");

    for (int i = 0; i < prof_cb_count && i < 15; i++) {
        if (prof_cb_entries[i].call_count == 0)
            continue;

        const char *name = NULL;
#ifndef _WIN32
        Dl_info info;
        if (dladdr((void *) (uintptr_t) prof_cb_entries[i].callback, &info) && info.dli_sname)
            name = info.dli_sname;
#endif
        char addr_buf[32];
        if (!name) {
            snprintf(addr_buf, sizeof(addr_buf), "%p",
                     (void *) (uintptr_t) prof_cb_entries[i].callback);
            name = addr_buf;
        }

        fprintf(stderr, "  %-35s %8.2f ms  (%6u calls, %6.1f us/call)\n",
                name,
                prof_cb_entries[i].total_ns / 1e6,
                prof_cb_entries[i].call_count,
                prof_cb_entries[i].call_count > 0
                    ? prof_cb_entries[i].total_ns / 1e3 / prof_cb_entries[i].call_count
                    : 0.0);
    }

    /* Reset. */
    for (int i = 0; i < prof_cb_count; i++) {
        prof_cb_entries[i].total_ns  = 0;
        prof_cb_entries[i].call_count = 0;
    }
    prof_timer_total_ns = 0;
}
#endif /* SUBSYS_PROFILE */

uint64_t TIMER_USEC;
uint64_t timer_target;

/*Enabled timers are stored in a linked list, with the first timer to expire at
  the head.*/
pc_timer_t *timer_head = NULL;

/* Are we initialized? */
int timer_inited = 0;

static void timer_advance_ex(pc_timer_t *timer, int start);

void
timer_enable(pc_timer_t *timer)
{
    pc_timer_t *timer_node = timer_head;

    if (timer->flags & TIMER_ENABLED)
        timer_disable(timer);

    if (timer->next || timer->prev)
        fatal("timer_enable - timer->next\n");

    timer->flags |= TIMER_ENABLED;

    /*List currently empty - add to head*/
    if (!timer_head) {
        timer_head = timer;
        timer->next = timer->prev = NULL;
        timer_target = timer_head->ts_integer;
        return;
    }

    timer_node = timer_head;

    while (1) {
        /*
           Timer expires before timer_node.
           Add to list in front of timer_node
         */
        if (TIMER_LESS_THAN(timer, timer_node)) {
            timer->next = timer_node;
            timer->prev = timer_node->prev;
            timer_node->prev = timer;
            if (timer->prev)
                timer->prev->next = timer;
            else {
                timer_head = timer;
                timer_target = timer_head->ts_integer;
            }
            return;
        }

        /*
           timer_node is last in the list.
           Add timer to end of list
         */
        if (!timer_node->next) {
            timer_node->next = timer;
            timer->prev = timer_node;
            return;
        }

        timer_node = timer_node->next;
    }
}

void
timer_disable(pc_timer_t *timer)
{
    if (!timer_inited || (timer == NULL) || !(timer->flags & TIMER_ENABLED))
        return;

    if (!timer->next && !timer->prev && timer != timer_head) {
        uint32_t *p = NULL;
        *p = 5;    /* Crash deliberately. */
        fatal("timer_disable(): Attempting to disable an isolated "
              "non-head timer incorrectly marked as enabled\n");
    }

    timer->flags &= ~TIMER_ENABLED;
    timer->in_callback = 0;

    if (timer->prev)
        timer->prev->next = timer->next;
    else
        timer_head = timer->next;
    if (timer->next)
        timer->next->prev = timer->prev;
    timer->prev = timer->next = NULL;
}

static void
timer_remove_head(void)
{
    if (timer_head) {
        pc_timer_t *timer = timer_head;
        timer_head = timer->next;
        timer_head->prev = NULL;
        timer->next = timer->prev = NULL;
        timer->flags &= ~TIMER_ENABLED;
    }
}

void
timer_process(void)
{
    int num = 0;

    if (!timer_head)
        return;

    while (1) {
        pc_timer_t *timer = timer_head;

        if (!TIMER_LESS_THAN_VAL(timer, (uint64_t) tsc))
            break;

        timer_remove_head();

        if (timer->flags & TIMER_SPLIT)
            timer_advance_ex(timer, 0);   /* We're splitting a > 1 s period into
                                             multiple <= 1 s periods. */
        else if (timer->callback != NULL) {
            /*
               Make sure it's not NULL, so that we can
               have a NULL callback when no operation
               is needed.
             */
#ifdef SUBSYS_PROFILE
            uint64_t prof_t0 = prof_now_ns();
#endif
            timer->in_callback = 1;
            timer->callback(timer->priv);
            timer->in_callback = 0;
#ifdef SUBSYS_PROFILE
            {
                uint64_t         prof_dt = prof_now_ns() - prof_t0;
                prof_cb_entry_t *e       = prof_cb_find(timer->callback);
                if (e) {
                    e->total_ns += prof_dt;
                    e->call_count++;
                }
                prof_timer_total_ns += prof_dt;
            }
#endif
        }

        num++;
    }

    timer_target = timer_head->ts_integer;
}

void
timer_close(void)
{
    pc_timer_t *t = timer_head;
    pc_timer_t *r;

    /* Set all timers' prev and next to NULL so it is assured that
       timers that are not in malloc'd structs don't keep pointing
       to timers that may be in malloc'd structs. */
    while (t != NULL) {
        r       = t;
        r->prev = r->next = NULL;
        t                 = r->next;
    }

    timer_head = NULL;

    timer_inited = 0;
}

void
timer_init(void)
{
    timer_target = 0ULL;
    tsc          = 0;

    /* Initialise the CPU-independent timer */
    rivatimer_init();

    timer_inited = 1;
}

void
timer_add(pc_timer_t *timer, void (*callback)(void *priv), void *priv, int start_timer)
{
    memset(timer, 0, sizeof(pc_timer_t));

    timer->callback    = callback;
    timer->in_callback = 0;
    timer->priv        = priv;
    timer->flags       = 0;
    timer->prev        = timer->next = NULL;
    if (start_timer)
        timer_set_delay_u64(timer, 0);
}

/* The API for big timer periods starts here. */
void
timer_stop(pc_timer_t *timer)
{
    if (!timer_inited || (timer == NULL))
        return;

    timer->period = 0.0;
    if (timer->flags & TIMER_ENABLED)
        timer_disable(timer);
    timer->flags &= ~TIMER_SPLIT;
    timer->in_callback = 0;
}

static void
timer_do_period(pc_timer_t *timer, uint64_t period, int start)
{
    if (!timer_inited || (timer == NULL))
        return;

    if (start)
        timer_set_delay_u64(timer, period);
    else
        timer_advance_u64(timer, period);
}

static void
timer_advance_ex(pc_timer_t *timer, int start)
{
    if (!timer_inited || (timer == NULL))
        return;

    if (timer->period > MAX_USEC) {
        timer_do_period(timer, MAX_USEC64 * TIMER_USEC, start);
        timer->period -= MAX_USEC;
        timer->flags |= TIMER_SPLIT;
    } else {
        if (timer->period > 0.0)
            timer_do_period(timer, (uint64_t) (timer->period * ((double) TIMER_USEC)), start);
        else
            timer_disable(timer);
        timer->period = 0.0;
        timer->flags &= ~TIMER_SPLIT;
    }
}

static void
timer_on(pc_timer_t *timer, double period, int start)
{
    if (!timer_inited || (timer == NULL))
        return;

    timer->period = period;
    timer_advance_ex(timer, start);
}

void
timer_on_auto(pc_timer_t *timer, double period)
{
    if (!timer_inited || (timer == NULL))
        return;

    if (period > 0.0)
        /* If the timer is in the callback, signal that, so that timer_advance_u64()
           is used instead of timer_set_delay_u64(). */
        timer_on(timer, period, (timer->period <= 0.0) && !timer->in_callback);
    else
        timer_stop(timer);
}

void
timer_set_new_tsc(uint64_t new_tsc)
{
    pc_timer_t *timer = NULL;
    /* Run timers already expired. */
#ifdef USE_DYNAREC
    if (cpu_use_dynarec)
        update_tsc();
#endif

    if (!timer_head) {
        tsc = new_tsc;
        return;
    }

    timer = timer_head;
    timer_target = new_tsc + (int64_t)(timer_get_ts_int(timer_head) - (uint64_t)tsc);

    while (timer) {
        int64_t offset_from_current_tsc = (int64_t)(timer_get_ts_int(timer) - (uint64_t)tsc);
        timer->ts_integer = new_tsc + offset_from_current_tsc;

        timer = timer->next;
    }

    tsc = new_tsc;
}
