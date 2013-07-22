#include "internal.h"
#include "delete_int.h"
#include <monitor_invocations.h>
#include <caplock.h>
#include <barrelfish/event_queue.h>

static struct event_queue trigger_queue;
static bool triggered;
static bool enqueued;
static int suspended;
static struct event_queue_node trigger_qn;
static struct event_closure step_closure;
static struct event_queue_node caplock_qn;
static struct delete_st delete_step_st;
static struct capref delcap;
static struct event_queue delete_queue;
static struct delete_queue_node *pending_head, *pending_tail;

static void delete_steps_cont(void *st);
static void delete_steps_clear(void *st);
static void delete_queue_notify(void);

void
delete_steps_init(struct waitset *ws)
{
    errval_t err;

    event_queue_init(&trigger_queue, ws, EVENT_QUEUE_CONTINUOUS);
    triggered = false;
    enqueued = false;
    suspended = 0;
    step_closure = MKCLOSURE(delete_steps_cont, NULL);

    event_queue_init(&delete_queue, ws, EVENT_QUEUE_CONTINUOUS);
    pending_head = pending_tail = NULL;

    delete_step_st.wait = false;
    delete_step_st.result_handler = NULL;
    err = slot_alloc(&delcap);
    PANIC_IF_ERR(err, "allocating delete_steps slot");
    delete_step_st.capref = get_cap_domref(delcap);
    err = slot_alloc(&delete_step_st.newcap);
    PANIC_IF_ERR(err, "allocating delete_steps new cap slot");
}

void
delete_steps_trigger(void)
{
    if (!triggered) {
        triggered = true;
        if (!suspended && !enqueued) {
            event_queue_add(&trigger_queue, &trigger_qn, step_closure);
            enqueued = true;
        }
    }
}

void
delete_steps_pause(void)
{
    suspended++;
}

void
delete_steps_resume(void)
{
    assert(suspended > 0);
    suspended--;
    if (!suspended) {
        event_queue_add(&trigger_queue, &trigger_qn, step_closure);
        enqueued = true;
    }
}

static void
delete_steps_delete_result(errval_t status, void *st)
{
    assert(err_is_ok(status));
    delete_steps_resume();
}

static void
delete_steps_cont(void *st)
{
    errval_t err;
    assert(triggered);
    assert(enqueued);
    enqueued = false;
    if (suspended) {
        return;
    }

    err = monitor_delete_step(delcap);
    if (err_no(err) == SYS_ERR_CAP_LOCKED) {
        // XXX
        caplock_wait(get_cap_domref(NULL_CAP), &caplock_qn, step_closure);
    }
    if (err_no(err) == SYS_ERR_DELETE_LAST_OWNED) {
        assert(!delete_step_st.result_handler);
        delete_step_st.result_handler = delete_steps_delete_result;
        delete_step_st.st = NULL;
        capops_delete_int(&delete_step_st);
    }
    else if (err_no(err) == SYS_ERR_CAP_NOT_FOUND) {
        delete_steps_clear(st);
    }
    else if (err_is_fail(err)) {
        USER_PANIC_ERR(err, "while performing delete steps");
    }
    else {
        if (err_no(err) == SYS_ERR_RAM_CAP_CREATED) {
            send_new_ram_cap(delcap);
        }
        if (!enqueued) {
            event_queue_add(&trigger_queue, &trigger_qn, step_closure);
            enqueued = true;
        }
    }
}

static void
delete_steps_clear(void *st)
{
    errval_t err;
    while (true) {
        err = monitor_clear_step(delcap);
        if (err_no(err) == SYS_ERR_CAP_NOT_FOUND) {
            break;
        }
        else if (err_is_fail(err)) {
            USER_PANIC_ERR(err, "while performing clear steps");
        }
        else if (err_no(err) == SYS_ERR_RAM_CAP_CREATED) {
            send_new_ram_cap(delcap);
        }
    }
    triggered = false;
    delete_queue_notify();
}

void
delete_queue_wait(struct delete_queue_node *qn, struct event_closure cont)
{
    // enqueue the node in the list of pending events
    if (!pending_head) {
        assert(!pending_tail);
        pending_head = pending_tail = qn;
        qn->next = NULL;
    }
    else {
        assert(pending_tail);
        assert(!pending_tail->next);
        pending_tail->next = qn;
        pending_tail = qn;
        qn->next = NULL;
    }
    qn->cont = cont;

    // trigger "stepping" mode of the delete/revoke state machine
    delete_steps_trigger();
}

static void
delete_queue_notify(void)
{
    // this should only be triggered when the "stepping" mode of the
    // delete/revoke state machine completes, so a notify without any
    // operations pending would be very strange and is probably a bug
    assert(pending_head);
    assert(pending_tail);

    // extract the contents of the queue of currently pending delete operations
    struct delete_queue_node *curr = pending_head;
    pending_head = pending_tail = NULL;

    // put them all in the event queue so they are executed
    for ( ; curr; curr = curr->next) {
        event_queue_add(&delete_queue, &curr->qn, curr->cont);
    }
}

