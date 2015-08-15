/* 
 * Copyright (c) 2015, Jack Lange <jacklange@cs.pitt.edu>
 * All rights reserved.
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "PETLAB_LICENSE".
 */

#ifndef __HOBBES_MEMORY_H__
#define __HOBBES_MEMORY_H__

#include <stdint.h>


#include "hobbes.h"
#include "hobbes_cmd_queue.h"


int hobbes_assign_memory(hcq_handle_t hcq,
			 uintptr_t    base_addr, 
			 uint64_t     size,
			 bool         allocated,
			 bool         zeroed);
			 



#endif
