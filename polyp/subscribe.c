#include <stdio.h>
#include <assert.h>

#include "queue.h"
#include "subscribe.h"
#include "xmalloc.h"

struct pa_subscription {
    struct pa_core *core;
    int dead;
    void (*callback)(struct pa_core *c, enum pa_subscription_event_type t, uint32_t index, void *userdata);
    void *userdata;
    enum pa_subscription_mask mask;

    struct pa_subscription *prev, *next;
};

struct pa_subscription_event {
    enum pa_subscription_event_type type;
    uint32_t index;
};

static void sched_event(struct pa_core *c);

struct pa_subscription* pa_subscription_new(struct pa_core *c, enum pa_subscription_mask m, void (*callback)(struct pa_core *c, enum pa_subscription_event_type t, uint32_t index, void *userdata), void *userdata) {
    struct pa_subscription *s;
    assert(c);

    s = pa_xmalloc(sizeof(struct pa_subscription));
    s->core = c;
    s->dead = 0;
    s->callback = callback;
    s->userdata = userdata;
    s->mask = m;

    if ((s->next = c->subscriptions))
        s->next->prev = s;
    s->prev = NULL;
    c->subscriptions = s;
    return NULL;
}

void pa_subscription_free(struct pa_subscription*s) {
    assert(s && !s->dead);
    s->dead = 1;
    sched_event(s->core);
}

static void free_item(struct pa_subscription *s) {
    assert(s && s->core);

    if (s->prev)
        s->prev->next = s->next;
    else
        s->core->subscriptions = s->next;
            
    if (s->next)
        s->next->prev = s->prev;
    
    pa_xfree(s);
}

void pa_subscription_free_all(struct pa_core *c) {
    struct pa_subscription_event *e;
    assert(c);
    
    while (c->subscriptions)
        free_item(c->subscriptions);

    if (c->subscription_event_queue) {
        while ((e = pa_queue_pop(c->subscription_event_queue)))
            pa_xfree(e);
        
        pa_queue_free(c->subscription_event_queue, NULL, NULL);
        c->subscription_event_queue = NULL;
    }

    if (c->subscription_defer_event) {
        c->mainloop->defer_free(c->subscription_defer_event);
        c->subscription_defer_event = NULL;
    }
}

static void defer_cb(struct pa_mainloop_api *m, struct pa_defer_event *e, void *userdata) {
    struct pa_core *c = userdata;
    struct pa_subscription *s;
    assert(c && c->subscription_defer_event == e && c->mainloop == m);

    c->mainloop->defer_enable(c->subscription_defer_event, 0);


    /* Dispatch queued events */
    
    if (c->subscription_event_queue) {
        struct pa_subscription_event *e;
        
        while ((e = pa_queue_pop(c->subscription_event_queue))) {
            struct pa_subscription *s;

            for (s = c->subscriptions; s; s = s->next) {
                if (!s->dead && pa_subscription_match_flags(s->mask, e->type))
                    s->callback(c, e->type, e->index, s->userdata);
            }
            
            pa_xfree(e);
        }
    }

    /* Remove dead subscriptions */
    
    s = c->subscriptions;
    while (s) {
        struct pa_subscription *n = s->next;
        if (s->dead)
            free_item(s);
        s = n;
    }
}

static void sched_event(struct pa_core *c) {
    assert(c);
    
    if (!c->subscription_defer_event) {
        c->subscription_defer_event = c->mainloop->defer_new(c->mainloop, defer_cb, c);
        assert(c->subscription_defer_event);
    }
        
    c->mainloop->defer_enable(c->subscription_defer_event, 1);
}


void pa_subscription_post(struct pa_core *c, enum pa_subscription_event_type t, uint32_t index) {
    struct pa_subscription_event *e;
    assert(c);

    e = pa_xmalloc(sizeof(struct pa_subscription_event));
    e->type = t;
    e->index = index;

    if (!c->subscription_event_queue) {
        c->subscription_event_queue = pa_queue_new();
        assert(c->subscription_event_queue);
    }
    
    pa_queue_push(c->subscription_event_queue, e);
    sched_event(c);
}


int pa_subscription_match_flags(enum pa_subscription_mask m, enum pa_subscription_event_type t) {
    return !!(m & (1 >> (t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK)));
}
