/* 
 * HIO Engine
 * (c) 2016, Jiannan Ouyang <ouyang@cs.pitt.edu>
 *
 * The HIO engine is a shared ringbuffer between the I/O domain and the host.
 * A kthread is created on each side to poll the pending requests.
 * 
 * On the I/O domain side, syscall requests are dispatched to hio_stubs based on 
 * the stub_id associated with each request. Currently, each hio_stub can have 
 * one pending syscall at a time.
 */
#include <linux/sched.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/slab.h>

#include "hio.h"

static int hio_handler_worker_polling(void *arg) {
    struct hio_engine *engine = (struct hio_engine *)arg;
    do {
        if (engine->rb_syscall_prod_idx != engine->rb_syscall_cons_idx) {
            // there are pending syscalls
            struct stub_syscall_t *syscall = kmalloc(sizeof(struct stub_syscall_t), GFP_KERNEL);
            if (syscall == NULL) {
                printk(KERN_ERR "Failed allocate syscall memeory\n");
                return -1;
            }

            while (engine->rb_syscall_prod_idx != engine->rb_syscall_cons_idx) {
                // there are pending syscalls
                struct hio_cmd_t *cmd = &engine->rb[engine->rb_syscall_cons_idx];
                struct hio_stub *stub = lookup_stub(engine, cmd->stub_id);
                if (stub == NULL) {
                    printk(KERN_ERR "stub_id %d does not exist\n", cmd->stub_id);
                    return -1;
                } else if (stub->is_pending) {
                    printk(KERN_ERR "Failed to process syscall while previous one is pending\n");
                    printk(KERN_ERR "HIO currently supports ONE outstanding syscall per proc\n");
                    return -1;
                } else {
                    syscall->stub_id = cmd->stub_id;
                    syscall->syscall_nr = cmd->syscall_nr;
                    syscall->arg0 = cmd->arg0;
                    syscall->arg1 = cmd->arg1;
                    syscall->arg2 = cmd->arg2;
                    syscall->arg3 = cmd->arg3;
                    syscall->arg4 = cmd->arg4;

                    spin_lock(&stub->lock);
                    stub->pending_syscall = syscall;
                    stub->is_pending = true;
                    spin_unlock(&stub->lock);
                }
                spin_lock(&engine->lock);
                engine->rb_syscall_cons_idx = (engine->rb_syscall_cons_idx + 1) % HIO_RB_SIZE;
                spin_unlock(&engine->lock);
                wake_up_interruptible(&stub->syscall_wq);
            } 
        }
    } while (!kthread_should_stop());

    return 0;
}

void hio_engine_insert_syscall(struct hio_engine *engine, struct stub_syscall_t *syscall) {
    // Insert syscall into hio_engine
    spin_lock(&engine->lock);
    if ((engine->rb_syscall_prod_idx + 1) % HIO_RB_SIZE != engine->rb_syscall_cons_idx) {
        // ringbuffer is not full
        struct hio_cmd_t *cmd = &engine->rb[engine->rb_syscall_prod_idx];
        cmd->stub_id     =   syscall->stub_id;
        cmd->syscall_nr =   syscall->syscall_nr;
        cmd->arg0       =   syscall->arg0; 
        cmd->arg1       =   syscall->arg1;
        cmd->arg2       =   syscall->arg2;
        cmd->arg3       =   syscall->arg3;
        cmd->arg4       =   syscall->arg4;

        engine->rb_syscall_prod_idx = (engine->rb_syscall_prod_idx + 1) % HIO_RB_SIZE;
    }
    spin_unlock(&engine->lock);
}


// this is for test purpose
int hio_engine_syscall(struct hio_engine *engine, struct stub_syscall_t *syscall) {
    int ret = 0;

    hio_engine_insert_syscall(engine, syscall);

    // busy waiting returns
    while (engine->rb_ret_cons_idx == engine->rb_ret_prod_idx) {
        /* spinning */
    }

    spin_lock(&engine->lock);
    if (engine->rb_ret_cons_idx != engine->rb_ret_prod_idx) {
        struct hio_cmd_t *cmd = &engine->rb[engine->rb_ret_cons_idx];
        ret = cmd->ret_val;
        engine->rb_ret_cons_idx = (engine->rb_ret_cons_idx + 1) % HIO_RB_SIZE;
    }
    spin_unlock(&engine->lock);

    return ret;
} 

int hio_engine_init(struct hio_engine *hio_engine) {

    spin_lock_init(&hio_engine->lock);

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
