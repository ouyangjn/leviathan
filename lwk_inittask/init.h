/* Pisces LWK inittask 
 * (c) 2015, Jack Lange, <jacklange@cs.pitt.edu>
 */

#ifndef __INIT_H__
#define __INIT_H__


#include <hobbes_cmd_queue.h>

extern cpu_set_t  enclave_cpus;
extern uint64_t   enclave_id;
extern char     * enclave_name;

extern bool v3vee_enabled;
extern bool hobbes_enabled;


#endif
