/* 
 * Copyright (c) 2015, Jack Lange <jacklange@cs.pitt.edu>
 * All rights reserved.
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "PETLAB_LICENSE".
 */


#include <pet_log.h>
#include <pet_xml.h>

#include "hobbes.h"
#include "hobbes_util.h"
#include "hobbes_memory.h"
#include "hobbes_cmd_queue.h"
#include "hobbes_sys_db.h"
#include "hobbes_db.h"


extern hdb_db_t hobbes_master_db;



uint32_t 
hobbes_get_numa_cnt(void)
{
    return hdb_get_sys_numa_cnt(hobbes_master_db);
}

uint64_t
hobbes_get_block_size(void)
{
    return hdb_get_sys_blk_size(hobbes_master_db);
}

uint64_t 
hobbes_get_mem_size(void)
{
    uint64_t blk_cnt  = 0;
    uint64_t blk_size = 0;

    blk_cnt  = hdb_get_sys_blk_cnt  ( hobbes_master_db );
    blk_size = hdb_get_sys_blk_size ( hobbes_master_db );

    if ((blk_cnt ==  0) || (blk_size ==  0) ||
	(blk_cnt == -1) || (blk_size == -1)) {
	return 0;
    }

    return blk_cnt * blk_size;
}

uint64_t 
hobbes_get_free_mem(void)
{
    uint64_t blk_cnt  = 0;
    uint64_t blk_size = 0;

    blk_cnt  = hdb_get_sys_free_blk_cnt  ( hobbes_master_db );
    blk_size = hdb_get_sys_blk_size      ( hobbes_master_db );

    if ((blk_cnt ==  0) || (blk_size ==  0) ||
	(blk_cnt == -1) || (blk_size == -1)) {
	return 0;
    }

    return blk_cnt * blk_size;
}



uintptr_t 
hobbes_alloc_memory(uint32_t  numa_node, 
		    uintptr_t size_in_MB)
{
    uint32_t block_span = ((size_in_MB / (hobbes_get_block_size() / (1024 * 1024))) +
			   (size_in_MB % (hobbes_get_block_size() / (1024 * 1024)) != 0));

    return hobbes_alloc_memblock(numa_node, block_span);
}


uintptr_t 
hobbes_alloc_memblock(uint32_t numa_node,
		      uint32_t block_span)
{
    uintptr_t block_paddr = 0;

    printf("Allocating %d blocks of memory\n", block_span);

    block_paddr = hdb_allocate_memory(hobbes_master_db, numa_node, block_span);

    return block_paddr;
}




struct hobbes_memory_info * 
hobbes_get_memory_list(uint64_t * num_mem_blks)
{
    struct hobbes_memory_info * blk_arr  = NULL;
    uint64_t                  * addr_arr = NULL;
    
    uint64_t blk_cnt = 0;
    uint64_t i       = 0;

    addr_arr = hdb_get_mem_blocks(hobbes_master_db, &blk_cnt);

    if (addr_arr == NULL) {
	ERROR("Could not retrieve memory block list\n");
	return NULL;
    }
    
    blk_arr = calloc(sizeof(struct hobbes_memory_info), blk_cnt);
    
    for (i = 0; i < blk_cnt; i++) {
	blk_arr[i].base_addr = addr_arr[i];
	
	blk_arr[i].size_in_bytes = hdb_get_sys_blk_size(hobbes_master_db);

	blk_arr[i].numa_node     = hdb_get_mem_numa_node  ( hobbes_master_db, addr_arr[i] );
	blk_arr[i].state         = hdb_get_mem_state      ( hobbes_master_db, addr_arr[i] );
	blk_arr[i].enclave_id    = hdb_get_mem_enclave_id ( hobbes_master_db, addr_arr[i] );
	blk_arr[i].app_id        = hdb_get_mem_app_id     ( hobbes_master_db, addr_arr[i] );
    }

    free(addr_arr);

    *num_mem_blks = blk_cnt;

    return blk_arr;
}



struct hobbes_cpu_info *
hobbes_get_cpu_list(uint32_t * num_cpus)
{
    struct hobbes_cpu_info * cpu_arr = NULL;
    uint32_t               * id_arr  = NULL;

    uint32_t cpu_cnt = 0;
    uint32_t i       = 0;

    id_arr = hdb_get_cpus(hobbes_master_db, &cpu_cnt);
    *num_cpus = cpu_cnt;

    if (id_arr == NULL) {
	ERROR("Could not retrieve CPU list\n");
	return NULL;
    }

    cpu_arr = calloc(sizeof(struct hobbes_cpu_info), cpu_cnt);

    for (i = 0; i < cpu_cnt; i++) {
	cpu_arr[i].cpu_id    = id_arr[i];

	cpu_arr[i].numa_node  = hdb_get_cpu_numa_node  ( hobbes_master_db, id_arr[i] );
	cpu_arr[i].state      = hdb_get_cpu_state      ( hobbes_master_db, id_arr[i] );
	cpu_arr[i].enclave_id = hdb_get_cpu_enclave_id ( hobbes_master_db, id_arr[i] );
    }

    free(id_arr);

    return cpu_arr;
}

const char * 
mem_state_to_str(mem_state_t state) 
{
    
    switch (state) {
	case MEMORY_INVALID:   return "INVALID";
	case MEMORY_RSVD:      return "RSVD";
	case MEMORY_FREE:      return "FREE";
	case MEMORY_ALLOCATED: return "ALLOCATED";

	default: return NULL;
    }

    return NULL;
}


const char * 
cpu_state_to_str(cpu_state_t state) 
{
    switch (state) {
	case CPU_INVALID:   return "INVALID";
	case CPU_RSVD:      return "RSVD";
	case CPU_FREE:      return "FREE";
	case CPU_ALLOCATED: return "ALLOCATED";

	default: return NULL;
    }

    return NULL;
}





int 
hobbes_assign_memory(hcq_handle_t hcq,
		     uintptr_t    base_addr, 
		     uint64_t     size,
		     bool         allocated,
		     bool         zeroed)
{

    hcq_cmd_t cmd      = HCQ_INVALID_CMD;
    pet_xml_t cmd_xml  = PET_INVALID_XML;

    uint32_t  ret_size =  0;
    uint8_t * ret_data =  NULL;

    char    * tmp_str  =  NULL;
    int       ret      = -1;

    

    cmd_xml = pet_xml_new_tree("memory");

    if (cmd_xml == PET_INVALID_XML) {
        ERROR("Could not create xml command\n");
        goto err;
    }

    /* Base Address */
    if (asprintf(&tmp_str, "%p", (void *)base_addr) == -1) {
	tmp_str = NULL;
	goto err;
    }

    pet_xml_add_val(cmd_xml, "base_addr",  tmp_str);
    smart_free(tmp_str);

    /* Size */
    if (asprintf(&tmp_str, "%lu", size) == -1) {
	tmp_str = NULL;
	goto err;
    }

    pet_xml_add_val(cmd_xml, "size",  tmp_str);
    smart_free(tmp_str);
    
    pet_xml_add_val(cmd_xml, "allocated", (allocated) ? "1" : "0");
    pet_xml_add_val(cmd_xml, "zeroed",    (zeroed)    ? "1" : "0");

    tmp_str = pet_xml_get_str(cmd_xml);
    cmd     = hcq_cmd_issue(hcq, HOBBES_CMD_ADD_MEM, strlen(tmp_str) + 1, tmp_str);
    
    if (cmd == HCQ_INVALID_CMD) {
	ERROR("Error issuing add memory command (%s)\n", tmp_str);
	goto err;
    } 


    ret = hcq_get_ret_code(hcq, cmd);
    
    if (ret != 0) {
	ret_data = hcq_get_ret_data(hcq, cmd, &ret_size);
	ERROR("Error adding memory (%s) [ret=%d]\n", ret_data, ret);
	goto err;
    }

    hcq_cmd_complete(hcq, cmd);

    smart_free(tmp_str);
    pet_xml_free(cmd_xml);

    return ret;

 err:
    if (tmp_str != NULL)            smart_free(tmp_str);                 
    if (cmd_xml != PET_INVALID_XML) pet_xml_free(cmd_xml);
    if (cmd     != HCQ_INVALID_CMD) hcq_cmd_complete(hcq, cmd);
    return -1;
}
