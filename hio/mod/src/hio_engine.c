/* 
 * HIO Ringbufffer
 * (c) 2016, Jiannan Ouyang <ouyang@cs.pitt.edu>
 */
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>

#include "hio.h"

static int hio_handler_worker_polling(void *arg) {
    struct hio_engine *hio_engine = (struct hio_engine *)arg;
    do {
        if (!rb_syscall_is_empty()) {
            spin_lock(&hio_engine->rb_lock);
            if (!rb_syscall_is_empty()) {
            } 
            spin_unlock(&hio_engine->rb_lock);
        }
    } while (!kthread_should_stop());

    return 0;
}

int hio_engine_init(struct hio_engine *hio_engine) {

    spin_lock_init(&hio_engine->rb_lock);

    {
        // create polling kthread
        struct task_struct *handler_thread = kthread_run(hio_handler_worker_polling, (void *)hio_engine, "hio_polling");
        if (IS_ERR(handler_thread)) {
            printk(KERN_ERR "Failed to start hio hanlder thread\n");
            return -1;
        }
        hio_engine->handler_thread = handler_thread;
    }

    return 0;
}

int hio_engine_deinit(struct hio_engine *hio_engine) {
    kthread_stop(hio_engine->handler_thread);
    return 0;
}


int 
insert_stub(struct hio_engine *hio_engine, 
        int key, struct hio_stub *stub) {
    if (hio_engine->stub_lookup_table[key] != NULL) {
        printk(KERN_ERR "Failed to insert duplicated stub key %d\n", key);
        return -1;
    }
    hio_engine->stub_lookup_table[key] = stub;
    return 0;
}


int
remove_stub(struct hio_engine *hio_engine, int key) {
    int ret = 0;
    if (hio_engine->stub_lookup_table[key] != NULL) {
        printk(KERN_WARNING "Trying to remove a non-existing stub, key=%d\n", key);
        ret = -1;
    }
    hio_engine->stub_lookup_table[key] = NULL;
    return ret;
}


struct hio_stub * 
lookup_stub(struct hio_engine *hio_engine, int key) {
    return hio_engine->stub_lookup_table[key];
}
