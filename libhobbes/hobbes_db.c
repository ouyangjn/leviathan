/* 
 * Copyright (c) 2015, Jack Lange <jacklange@cs.pitt.edu>
 * All rights reserved.
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "PETLAB_LICENSE".
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include <dbapi.h>
#include <dballoc.h>

#include <pet_log.h>

#include "hobbes.h"
#include "hobbes_db.h"
#include "hobbes_db_schema.h"

#define PAGE_SIZE sysconf(_SC_PAGESIZE)

hdb_db_t 
hdb_create(uint64_t size) 
{
    void * db      = NULL;

    if (size % PAGE_SIZE) {
	ERROR("Database must be integral number of pages\n");
	return NULL;
    }

    db = wg_attach_local_database(size);

    if (db == NULL) {
	ERROR("Could not create database\n");
	return NULL;
    }

    return db;
}

hdb_db_t 
hdb_attach(void * db_addr) 
{ 
    hdb_db_t db = NULL;

    db = wg_attach_existing_local_database(db_addr);

    return db;
}    

void 
hdb_detach(hdb_db_t db)
{
    wg_detach_local_database(db);
}


int
hdb_init_master_db(hdb_db_t db)
{
    void * rec = NULL;
    
    /* Create Enclave Header */
    rec = wg_create_record(db, 3);
    wg_set_field(db, rec, HDB_TYPE_FIELD,       wg_encode_int(db, HDB_REC_ENCLAVE_HDR));
    wg_set_field(db, rec, HDB_ENCLAVE_HDR_NEXT, wg_encode_int(db, 0));
    wg_set_field(db, rec, HDB_ENCLAVE_HDR_CNT,  wg_encode_int(db, 0));
    
    /* Create Application Header */
    rec = wg_create_record(db, 3);
    wg_set_field(db, rec, HDB_TYPE_FIELD,       wg_encode_int(db, HDB_REC_APP_HDR));
    wg_set_field(db, rec, HDB_APP_HDR_NEXT,     wg_encode_int(db, 1));
    wg_set_field(db, rec, HDB_APP_HDR_CNT,      wg_encode_int(db, 0));
    
    /* Create XEMEM header */
    rec = wg_create_record(db, 2);
    wg_set_field(db, rec, HDB_TYPE_FIELD,       wg_encode_int(db, HDB_REC_XEMEM_HDR));
    wg_set_field(db, rec, HDB_SEGMENT_HDR_CNT,  wg_encode_int(db, 0));

    return 0;
}


void * 
hdb_get_db_addr(hdb_db_t db) 
{
#ifdef USE_DATABASE_HANDLE
   return ((db_handle *)db)->db;
#else
    return db;
#endif
}



/* 
 * Enclave Accessors 
 */


/**
 * Get an enclave handle from an enclave id
 *  - Returns NULL if no enclave is found 
 **/
static hdb_enclave_t
__get_enclave_by_id(hdb_db_t    db, 
		    hobbes_id_t enclave_id) 
{
    hdb_enclave_t enclave  = NULL;
    wg_query    * query    = NULL;
    wg_query_arg  arglist[2];

    arglist[0].column = HDB_TYPE_FIELD;
    arglist[0].cond   = WG_COND_EQUAL;
    arglist[0].value  = wg_encode_query_param_int(db, HDB_REC_ENCLAVE);    

    arglist[1].column = HDB_ENCLAVE_ID;
    arglist[1].cond   = WG_COND_EQUAL;
    arglist[1].value  = wg_encode_query_param_int(db, enclave_id);

    query   = wg_make_query(db, NULL, 0, arglist, 2);

    enclave = wg_fetch(db, query);

    wg_free_query(db, query);
    wg_free_query_param(db, arglist[0].value);
    wg_free_query_param(db, arglist[1].value);

    return enclave;
}





/**
 * Get an enclave handle from an enclave name
 *  - Returns NULL if no enclave is found 
 **/
static hdb_enclave_t
__get_enclave_by_name(hdb_db_t   db, 
		      char     * name)
{
    hdb_enclave_t   enclave  = NULL;
    wg_query      * query    = NULL;
    wg_query_arg    arglist[2];    
 
    arglist[0].column = HDB_TYPE_FIELD;
    arglist[0].cond   = WG_COND_EQUAL;
    arglist[0].value  = wg_encode_query_param_int(db, HDB_REC_ENCLAVE);    

    arglist[1].column = HDB_ENCLAVE_NAME;
    arglist[1].cond   = WG_COND_EQUAL;
    arglist[1].value  = wg_encode_query_param_str(db, name, NULL);

    query = wg_make_query(db, NULL, 0, arglist, 2);

    enclave = wg_fetch(db, query);

    wg_free_query(db, query);
    wg_free_query_param(db, arglist[0].value);
    wg_free_query_param(db, arglist[1].value);

    return enclave;
}





static hobbes_id_t
__create_enclave_record(hdb_db_t         db,
			char           * name, 
			int              mgmt_dev_id, 
			enclave_type_t   type, 
			hobbes_id_t      parent)
{
    void       * hdr_rec       = NULL;
    hobbes_id_t  enclave_id    = HOBBES_INVALID_ID;
    uint32_t     enclave_cnt   = 0;
    char         auto_name[32] = {[0 ... 31] = 0};

    hdb_enclave_t enclave   = NULL;

    
    hdr_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_ENCLAVE_HDR, NULL);
    
    if (!hdr_rec) {
	ERROR("malformed database. Missing enclave Header\n");
	return HOBBES_INVALID_ID;
    }
    
    if (parent == HOBBES_INVALID_ID) {
	/* The Master enclave doesn't have a parent  *
	 * and gets a well known ID                  */
	enclave_id = HOBBES_MASTER_ENCLAVE_ID;
    } else {
	/* Get Next Available enclave ID */
	enclave_id  = wg_decode_int(db, wg_get_field(db, hdr_rec, HDB_ENCLAVE_HDR_NEXT));
    }

    /* Verify that enclave_id is available */
    if (__get_enclave_by_id(db, enclave_id)) {
	ERROR("Enclave with ID (%d) already exists\n", enclave_id);
	return HOBBES_INVALID_ID;
    }

    enclave_cnt = wg_decode_int(db, wg_get_field(db, hdr_rec, HDB_ENCLAVE_HDR_CNT));
 
    if (name == NULL) {
	snprintf(auto_name, 31, "enclave-%d", enclave_id);
	name = auto_name;
    }
    

    /* Verify that name is available */
    if (__get_enclave_by_name(db, name)) {
	ERROR("Enclave with the name (%s) already exists\n", name);
	return HOBBES_INVALID_ID;
    }

    /* Insert enclave into the db */
    enclave = wg_create_record(db, 8);
    wg_set_field(db, enclave, HDB_TYPE_FIELD,       wg_encode_int(db, HDB_REC_ENCLAVE));
    wg_set_field(db, enclave, HDB_ENCLAVE_ID,       wg_encode_int(db, enclave_id));
    wg_set_field(db, enclave, HDB_ENCLAVE_TYPE,     wg_encode_int(db, type));
    wg_set_field(db, enclave, HDB_ENCLAVE_DEV_ID,   wg_encode_int(db, mgmt_dev_id));
    wg_set_field(db, enclave, HDB_ENCLAVE_STATE,    wg_encode_int(db, ENCLAVE_INITTED));
    wg_set_field(db, enclave, HDB_ENCLAVE_NAME,     wg_encode_str(db, name, NULL));
    wg_set_field(db, enclave, HDB_ENCLAVE_CMDQ_ID,  wg_encode_int(db, 0));
    wg_set_field(db, enclave, HDB_ENCLAVE_PARENT,   wg_encode_int(db, parent));

    
    /* Update the enclave Header information */
    wg_set_field(db, hdr_rec, HDB_ENCLAVE_HDR_NEXT, wg_encode_int(db, enclave_id  + 1));
    wg_set_field(db, hdr_rec, HDB_ENCLAVE_HDR_CNT,  wg_encode_int(db, enclave_cnt + 1));

    return enclave_id;
}

hobbes_id_t
hdb_create_enclave(hdb_db_t         db,
		   char           * name, 
		   int              mgmt_dev_id, 
		   enclave_type_t   type, 
		   hobbes_id_t      parent)
{
    wg_int      lock_id;
    hobbes_id_t enclave_id = HOBBES_INVALID_ID;

    lock_id = wg_start_write(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return HOBBES_INVALID_ID;
    }

    enclave_id = __create_enclave_record(db, name, mgmt_dev_id, type, parent);
    
    if (!wg_end_write(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return HOBBES_INVALID_ID;
    }
    

    return enclave_id;
}

static int
__delete_enclave(hdb_db_t    db,
		 hobbes_id_t enclave_id)
{
    uint32_t      enclave_cnt = 0;
    void        * hdr_rec     = NULL;
    hdb_enclave_t enclave     = NULL;


    hdr_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_ENCLAVE_HDR, NULL);
    
    if (!hdr_rec) {
	ERROR("Malformed database. Missing enclave Header\n");
	return -1;
    }
    
    enclave = __get_enclave_by_id(db, enclave_id);
  
    if (!enclave) {
	ERROR("Could not find enclave (id: %d)\n", enclave_id);
	return -1;
    }

    if (wg_delete_record(db, enclave) != 0) {
	ERROR("Could not delete enclave from database\n");
	return -1;
    }

    enclave_cnt = wg_decode_int(db, wg_get_field(db, hdr_rec, HDB_ENCLAVE_HDR_CNT));
    wg_set_field(db, hdr_rec, HDB_ENCLAVE_HDR_CNT, wg_encode_int(db, enclave_cnt - 1));

    return 0;
}



int 
hdb_delete_enclave(hdb_db_t    db,
		   hobbes_id_t enclave_id)
{
    wg_int lock_id;
    int    ret = 0;

    lock_id = wg_start_write(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    ret = __delete_enclave(db, enclave_id);
    
    if (!wg_end_write(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }
    

    return ret;
}



static int 
__get_enclave_dev_id(hdb_db_t    db, 
		     hobbes_id_t enclave_id)
{
    hdb_enclave_t enclave = NULL;

    int dev_id = 0;

    enclave = __get_enclave_by_id(db, enclave_id);
    
    if (enclave == NULL) {
	ERROR("Could not find enclave (id: %d)\n", enclave_id);
	return -1;
    }

    dev_id = wg_decode_int(db, wg_get_field(db, enclave, HDB_ENCLAVE_DEV_ID));

    return dev_id;
}

int 
hdb_get_enclave_dev_id(hdb_db_t    db, 
		       hobbes_id_t enclave_id)
{
    wg_int lock_id;
    int    ret = 0;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    ret = __get_enclave_dev_id(db, enclave_id);
    
    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }
    

    return ret;
}

static hobbes_id_t
__get_enclave_parent(hdb_db_t    db,
		     hobbes_id_t enclave_id)
{
    hdb_enclave_t enclave   = NULL;
    hobbes_id_t   parent_id = HOBBES_INVALID_ID;

    enclave = __get_enclave_by_id(db, enclave_id);

    if (enclave == NULL) {
	ERROR("could not find enclave (id: %d)\n", enclave_id);
	return HOBBES_INVALID_ID;
    }

    parent_id = wg_decode_int(db, wg_get_field(db, enclave, HDB_ENCLAVE_PARENT));

    return parent_id;
}


hobbes_id_t
hdb_get_enclave_parent(hdb_db_t    db,
		       hobbes_id_t enclave_id)
{
    wg_int      lock_id;
    hobbes_id_t parent_id = HOBBES_INVALID_ID;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return HOBBES_INVALID_ID;
    }

    parent_id = __get_enclave_parent(db, enclave_id);

    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return HOBBES_INVALID_ID;
    }

    return parent_id;
}


static int 
__set_enclave_dev_id(hdb_db_t    db, 
		     hobbes_id_t enclave_id, 
		     int         dev_id)
{
    hdb_enclave_t enclave = NULL;

    enclave = __get_enclave_by_id(db, enclave_id);
    
    if (enclave == NULL) {
	ERROR("Could not find enclave (id: %d)\n", enclave_id);
	return -1;
    }

    wg_set_field(db, enclave, HDB_ENCLAVE_DEV_ID, wg_encode_int(db, dev_id));

    return 0;
}

int 
hdb_set_enclave_dev_id(hdb_db_t    db, 
		       hobbes_id_t enclave_id, 
		       int         dev_id)
{
    wg_int lock_id;
    int    ret = 0;

    lock_id = wg_start_write(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    ret = __set_enclave_dev_id(db, enclave_id, dev_id);
    
    if (!wg_end_write(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }
    
    return ret;
}


static enclave_type_t
__get_enclave_type(hdb_db_t    db, 
		   hobbes_id_t enclave_id)
{
    hdb_enclave_t enclave = NULL;

    enclave_type_t type = INVALID_ENCLAVE;

    enclave = __get_enclave_by_id(db, enclave_id);
    
    if (enclave == NULL) {
	ERROR("Could not find enclave (id: %d)\n", enclave_id);
	return INVALID_ENCLAVE;
    }

    type = wg_decode_int(db, wg_get_field(db, enclave, HDB_ENCLAVE_TYPE));

    return type;
}

enclave_type_t
hdb_get_enclave_type(hdb_db_t    db, 
		     hobbes_id_t enclave_id)
{
    wg_int lock_id;
    enclave_type_t type = INVALID_ENCLAVE;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return INVALID_ENCLAVE;
    }

    type = __get_enclave_type(db, enclave_id);
    
    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return INVALID_ENCLAVE;
    }
    

    return type;
}




static enclave_state_t
__get_enclave_state(hdb_db_t    db, 
		    hobbes_id_t enclave_id)
{
    hdb_enclave_t enclave = NULL;

    enclave_state_t state = ENCLAVE_ERROR;

    enclave = __get_enclave_by_id(db, enclave_id);
    
    if (enclave == NULL) {
	ERROR("Could not find enclave (id: %d)\n", enclave_id);
	return ENCLAVE_ERROR;
    }

    state = wg_decode_int(db, wg_get_field(db, enclave, HDB_ENCLAVE_STATE));

    return state;
}

enclave_state_t
hdb_get_enclave_state(hdb_db_t    db, 
		      hobbes_id_t enclave_id)
{
    wg_int lock_id;
    enclave_state_t state = ENCLAVE_ERROR;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return ENCLAVE_ERROR;
    }

    state = __get_enclave_state(db, enclave_id);
    
    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return ENCLAVE_ERROR;
    }
    

    return state;
}


static int
__set_enclave_state(hdb_db_t        db, 
		    hobbes_id_t     enclave_id, 
		    enclave_state_t state)
{
    hdb_enclave_t enclave = NULL;

    enclave = __get_enclave_by_id(db, enclave_id);
    
    if (enclave == NULL) {
	ERROR("Could not find enclave (id: %d)\n", enclave_id);
	return -1;
    }

    wg_set_field(db, enclave, HDB_ENCLAVE_STATE, wg_encode_int(db, state));

    return 0;
}

int
hdb_set_enclave_state(hdb_db_t        db, 
		      hobbes_id_t     enclave_id, 
		      enclave_state_t state)
{
    wg_int lock_id;
    int ret = 0;

    lock_id = wg_start_write(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    ret = __set_enclave_state(db, enclave_id, state);
    
    if (!wg_end_write(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }
    

    return ret;
}



static char *
__get_enclave_name(hdb_db_t    db, 
		   hobbes_id_t enclave_id)
{
    hdb_enclave_t enclave = NULL;

    char * name = NULL;

    enclave = __get_enclave_by_id(db, enclave_id);
    
    if (enclave == NULL) {
	ERROR("Could not find enclave (id: %d)\n", enclave_id);
	return NULL;
    }

    name = wg_decode_str(db, wg_get_field(db, enclave, HDB_ENCLAVE_NAME));

    return name;
}

char * 
hdb_get_enclave_name(hdb_db_t    db, 
		     hobbes_id_t enclave_id)
{
    wg_int lock_id;
    char * name = NULL;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return NULL;
    }

   name = __get_enclave_name(db, enclave_id);
    
    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return NULL;
    }
    
    return name;
}


static xemem_segid_t
__get_enclave_cmdq(hdb_db_t    db, 
		   hobbes_id_t enclave_id)
{
    hdb_enclave_t enclave = NULL;
    xemem_segid_t segid   = 0;


    enclave = __get_enclave_by_id(db, enclave_id);
    
    if (enclave == NULL) {
	ERROR("Could not find enclave (id: %d)\n", enclave_id);
	return -1;
    }

    segid = wg_decode_int(db, wg_get_field(db, enclave, HDB_ENCLAVE_CMDQ_ID));

    return segid;
}

xemem_segid_t 
hdb_get_enclave_cmdq(hdb_db_t    db,
		     hobbes_id_t enclave_id)
{
    wg_int lock_id;
    xemem_segid_t segid = 0;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    segid = __get_enclave_cmdq(db, enclave_id);
    
    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }
    
    return segid;
}

static int
__set_enclave_cmdq(hdb_db_t      db,
		   hobbes_id_t   enclave_id, 
		   xemem_segid_t segid)
{
    hdb_enclave_t enclave = NULL;

    enclave = __get_enclave_by_id(db, enclave_id);
    
    if (enclave == NULL) {
	ERROR("Could not find enclave (id: %d)\n", enclave_id);
	return -1;
    }

    wg_set_field(db, enclave, HDB_ENCLAVE_CMDQ_ID, wg_encode_int(db, segid));

    return 0;
}

int
hdb_set_enclave_cmdq(hdb_db_t      db,
		     hobbes_id_t   enclave_id, 
		     xemem_segid_t segid)
{
    wg_int lock_id;
    int ret = 0;

    lock_id = wg_start_write(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    ret = __set_enclave_cmdq(db, enclave_id, segid);
    
    if (!wg_end_write(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }
    
    return ret;
}



static hobbes_id_t
__get_enclave_id(hdb_db_t   db, 
		 char     * enclave_name)
{
    hdb_enclave_t enclave    = NULL;
    hobbes_id_t   enclave_id = HOBBES_INVALID_ID;

    enclave = __get_enclave_by_name(db, enclave_name);
    
    if (enclave == NULL) {
	ERROR("Could not find enclave (name: %s)\n", enclave_name);
	return HOBBES_INVALID_ID;
    }

    enclave_id = wg_decode_int(db, wg_get_field(db, enclave, HDB_ENCLAVE_ID));

    return enclave_id;
}

hobbes_id_t
hdb_get_enclave_id(hdb_db_t   db, 
		   char     * enclave_name)
{
    wg_int      lock_id;
    hobbes_id_t enclave_id = HOBBES_INVALID_ID;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return HOBBES_INVALID_ID;
    }

   enclave_id = __get_enclave_id(db, enclave_name);
    
    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return HOBBES_INVALID_ID;
    }
    
    return enclave_id;
}

static hobbes_id_t *
__get_enclaves(hdb_db_t    db,
	       uint32_t  * num_enclaves)
{
    hobbes_id_t * id_arr  = NULL;
    void        * db_rec  = NULL;
    void        * hdr_rec = NULL;
    uint32_t      cnt     = 0;
    uint32_t      i       = 0;
    
    hdr_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_ENCLAVE_HDR, NULL);    

    if (!hdr_rec) {
	ERROR("Malformed database. Missing enclave Header\n");
	return NULL;
    }

    cnt = wg_decode_int(db, wg_get_field(db, hdr_rec, HDB_ENCLAVE_HDR_CNT));

    id_arr = calloc(sizeof(hobbes_id_t), cnt);

    for (i = 0; i < cnt; i++) {
	db_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_ENCLAVE, db_rec);
	

	if (!db_rec) {
	    ERROR("Enclave Header state mismatch\n");
	    cnt = i;
	    break;
	}

	id_arr[i] = wg_decode_int(db, wg_get_field(db, db_rec, HDB_ENCLAVE_ID));

    }

    *num_enclaves = cnt;
    return id_arr;
}


hobbes_id_t * 
hdb_get_enclaves(hdb_db_t   db,
		 uint32_t * num_enclaves)
{
    hobbes_id_t * id_arr = NULL;
    wg_int        lock_id;

    if (!num_enclaves) {
	return NULL;
    }

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return NULL;
    }

    id_arr = __get_enclaves(db, num_enclaves);

    if (!wg_end_read(db, lock_id)) {
	ERROR("Catastrophic database locking error\n");
	return NULL;
    }

    return id_arr;
}






/* *******
 * 
 *  XEMEM 
 * 
 * *******/

static hdb_segment_t
__get_segment_by_segid(hdb_db_t      db, 
		       xemem_segid_t segid) 
{
    hdb_segment_t segment = NULL;
    wg_query    * query   = NULL;
    wg_query_arg  arglist[2];

    /* Convert segid to string (TODO: can the db encode 64 bit values automatically?) */
    arglist[0].column = HDB_TYPE_FIELD;
    arglist[0].cond   = WG_COND_EQUAL;
    arglist[0].value  = wg_encode_query_param_int(db, HDB_REC_XEMEM_SEGMENT);    

    arglist[1].column = HDB_SEGMENT_SEGID;
    arglist[1].cond   = WG_COND_EQUAL;
    arglist[1].value  = wg_encode_query_param_int(db, segid);

    query = wg_make_query(db, NULL, 0, arglist, 2);

    segment = wg_fetch(db, query);

    wg_free_query(db, query);
    wg_free_query_param(db, arglist[0].value);
    wg_free_query_param(db, arglist[1].value);

    return segment;
}

static hdb_segment_t
__get_segment_by_name(hdb_db_t   db,
		      char     * name)
{
    hdb_segment_t segment = NULL;
    wg_query    * query   = NULL;
    wg_query_arg  arglist[2];

    arglist[0].column = HDB_TYPE_FIELD;
    arglist[0].cond   = WG_COND_EQUAL;
    arglist[0].value  = wg_encode_query_param_int(db, HDB_REC_XEMEM_SEGMENT);

    arglist[1].column = HDB_SEGMENT_NAME;
    arglist[1].cond   = WG_COND_EQUAL;
    arglist[1].value  = wg_encode_query_param_str(db, name, NULL);

    query = wg_make_query(db, NULL, 0, arglist, 2);

    segment = wg_fetch(db, query);

    wg_free_query(db, query);
    wg_free_query_param(db, arglist[0].value);
    wg_free_query_param(db, arglist[1].value);

    return segment;
}

static int 
__create_segment_record(hdb_db_t        db,
			xemem_segid_t   segid,
			char          * name,
			hobbes_id_t     enclave_id,
			hobbes_id_t     app_id)
{
    void * hdr_rec        = NULL;
    void * rec            = NULL;
    int    segment_cnt    = 0;

    hdr_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_XEMEM_HDR, NULL);
    
    if (!hdr_rec) {
        ERROR("malformed database. Missing xemem Header\n");
        return -1;
    }

    /* Ensure segid and name do not exist */
    rec = __get_segment_by_segid(db, segid);
    if (rec) {
        ERROR("xemem segment with segid %ld already present\n", segid);
        return -1;
    }

    if (name) {
	rec = __get_segment_by_name(db, name);
	if (rec) {
	    ERROR("xemem segment with name %s already present\n", name);
	    return -1;
	}
    }

    if (name == NULL) {
	name = "unnamed";
    }

    /* Insert segment into the db */
    rec = wg_create_record(db, 5);
    wg_set_field(db, rec, HDB_TYPE_FIELD,       wg_encode_int(db, HDB_REC_XEMEM_SEGMENT));
    wg_set_field(db, rec, HDB_SEGMENT_SEGID,    wg_encode_int(db, segid));
    wg_set_field(db, rec, HDB_SEGMENT_NAME,     wg_encode_str(db, name, NULL));
    wg_set_field(db, rec, HDB_SEGMENT_ENCLAVE,  wg_encode_int(db, enclave_id));
    wg_set_field(db, rec, HDB_SEGMENT_APP,      wg_encode_int(db, app_id));

    /* Update the xemem Header information */
    segment_cnt = wg_decode_int(db, wg_get_field(db, hdr_rec, HDB_SEGMENT_HDR_CNT));
    wg_set_field(db, hdr_rec, HDB_SEGMENT_HDR_CNT, wg_encode_int(db, segment_cnt + 1));

    return 0;
}


int
hdb_create_xemem_segment(hdb_db_t      db,
			 xemem_segid_t segid,
			 char        * name, 
			 hobbes_id_t   enclave_id,
			 hobbes_id_t   app_id)
{
    wg_int lock_id;
    int    ret;

    lock_id = wg_start_write(db);
    if (!lock_id) {
        ERROR("Could not lock database\n");
        return -1;
    }

    ret = __create_segment_record(db, segid, name, enclave_id, app_id);

    if (!wg_end_write(db, lock_id)) {
        ERROR("Apparently this is catastrophic...\n");
	return -1;
    }


    return ret;
}


static int
__delete_segment(hdb_db_t        db,
		 xemem_segid_t   segid)
{
    void * hdr_rec        = NULL;
    int    segment_cnt    = 0;
    int    ret            = 0;

    hdb_segment_t segment = NULL;


    hdr_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_XEMEM_HDR, NULL);
    
    if (!hdr_rec) {
        ERROR("malformed database. Missing xemem Header\n");
        return -1;
    }

    /* Find record */

    segment = __get_segment_by_segid(db, segid);

    if (!segment) {
	ERROR("Could not find xemem segment (segid: %ld)\n", segid);
	return -1;
    }
    
    if (wg_delete_record(db, segment) != 0) {
        ERROR("Could not delete xemem segment from database\n");
        return ret;
    }

    /* Update the xemem Header information */
    segment_cnt = wg_decode_int(db, wg_get_field(db, hdr_rec, HDB_SEGMENT_HDR_CNT));
    wg_set_field(db, hdr_rec, HDB_SEGMENT_HDR_CNT, wg_encode_int(db, segment_cnt - 1));

    return 0;
}



int
hdb_delete_xemem_segment(hdb_db_t      db,
			 xemem_segid_t segid)
{
    wg_int lock_id;
    int    ret;
    
    lock_id = wg_start_write(db);
    if (!lock_id) {
        ERROR("Could not lock database\n");
        return -1;
    }

    ret = __delete_segment(db, segid);

    if (!wg_end_write(db, lock_id)) {
        ERROR("Apparently this is catastrophic...\n");
	return -1;
    }

    return ret;

}

static xemem_segid_t
__get_xemem_segid(hdb_db_t   db,
		  char     * name)
{
    hdb_segment_t segment = NULL;
    xemem_segid_t segid   = XEMEM_INVALID_SEGID;

    segment = __get_segment_by_name(db, name);

    if (segment == NULL) {
	ERROR("Could not find XEMEM segment (name: %s)\n", name);
	return XEMEM_INVALID_SEGID;
    }

    segid = wg_decode_int(db, wg_get_field(db, segment, HDB_SEGMENT_SEGID));
    
    return segid;
}


xemem_segid_t
hdb_get_xemem_segid(hdb_db_t   db,
		    char     * name)
{
    wg_int lock_id;
    xemem_segid_t segid = XEMEM_INVALID_SEGID;

    lock_id = wg_start_read(db);

    if (!lock_id) {
        ERROR("Could not lock database\n");
        return XEMEM_INVALID_SEGID;
    }

    segid = __get_xemem_segid(db, name);

    if (!wg_end_read(db, lock_id)) {
        ERROR("Catastrophic database locking error\n");
	return XEMEM_INVALID_SEGID;
    }

    return segid;
}


static char *
__get_xemem_name(hdb_db_t       db,
		 xemem_segid_t segid)
{
    hdb_segment_t segment = NULL;
    char * name = NULL;

    segment = __get_segment_by_segid(db, segid);

    if (segment == NULL) {
	ERROR("Could not find XEMEM segment (id: %ld)\n", segid);
	return NULL;
    }

    name = wg_decode_str(db, wg_get_field(db, segment, HDB_SEGMENT_NAME));

    return name;
}


char * 
hdb_get_xemem_name(hdb_db_t      db,
		   xemem_segid_t segid)
{
    wg_int lock_id;
    char * name = NULL;
    
    lock_id = wg_start_read(db);

    if (!lock_id) {
        ERROR("Could not lock database\n");
        return NULL;
    }

    name = __get_xemem_name(db, segid);

    if (!wg_end_read(db, lock_id)) {
        ERROR("Catastrophic database locking error\n");
	return NULL;
    }

    return name;
}



hobbes_id_t
__get_xemem_enclave(hdb_db_t        db,
		    xemem_segid_t   segid)
{
    hdb_segment_t segment    = NULL;
    hobbes_id_t   enclave_id = HOBBES_INVALID_ID;

    segment = __get_segment_by_segid(db, segid);

    if (segment == NULL) {
	ERROR("Could not find XEMEM segment (id: %ld)\n", segid);
	return HOBBES_INVALID_ID;
    }

    enclave_id = wg_decode_int(db, wg_get_field(db, segment, HDB_SEGMENT_ENCLAVE));

    return enclave_id;
}


hobbes_id_t
hdb_get_xemem_enclave(hdb_db_t      db,
		      xemem_segid_t segid)
{
    wg_int lock_id;
    hobbes_id_t enclave_id = HOBBES_INVALID_ID;
    
    lock_id = wg_start_read(db);

    if (!lock_id) {
        ERROR("Could not lock database\n");
        return HOBBES_INVALID_ID;
    }

    enclave_id = __get_xemem_enclave(db, segid);

    if (!wg_end_read(db, lock_id)) {
        ERROR("Catastrophic database locking error\n");
	return HOBBES_INVALID_ID;
    }

    return enclave_id;
}

hobbes_id_t
__get_xemem_app(hdb_db_t        db,
		xemem_segid_t   segid)
{
    hdb_segment_t segment = NULL;
    hobbes_id_t   app_id  = HOBBES_INVALID_ID;

    segment = __get_segment_by_segid(db, segid);

    if (segment == NULL) {
	ERROR("Could not find XEMEM segment (id: %ld)\n", segid);
	return HOBBES_INVALID_ID;
    }

    app_id = wg_decode_int(db, wg_get_field(db, segment, HDB_SEGMENT_APP));

    return app_id;
}


hobbes_id_t
hdb_get_xemem_app(hdb_db_t      db,
		  xemem_segid_t segid)
{
    wg_int lock_id;
    hobbes_id_t app_id = HOBBES_INVALID_ID;
    
    lock_id = wg_start_read(db);

    if (!lock_id) {
        ERROR("Could not lock database\n");
        return HOBBES_INVALID_ID;
    }

    app_id = __get_xemem_app(db, segid);

    if (!wg_end_read(db, lock_id)) {
        ERROR("Catastrophic database locking error\n");
	return HOBBES_INVALID_ID;
    }

    return app_id;
}



static xemem_segid_t *
__get_segments(hdb_db_t   db,
	       int      * num_segments)
{
    void * hdr_rec = NULL;
    void * db_rec  = NULL;
    int    cnt     = 0;
    int    i       = 0;

    xemem_segid_t * segid_arr = NULL;

    hdr_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_XEMEM_HDR, NULL);    

    if (!hdr_rec) {
        ERROR("Malformed database. Missing xemem Header\n");
        return NULL;
    }

    cnt = wg_decode_int(db, wg_get_field(db, hdr_rec, HDB_SEGMENT_HDR_CNT));

    segid_arr = calloc(sizeof(xemem_segid_t), cnt);

    for (i = 0; i < cnt; i++) {
        db_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_XEMEM_SEGMENT, db_rec);
        
        if (!db_rec) {
            ERROR("xemem Header state mismatch\n");
	    cnt = i;
            break;
        }

	segid_arr[i] = wg_decode_int(db, wg_get_field(db, db_rec, HDB_SEGMENT_SEGID));
    }

    *num_segments = cnt;
    return segid_arr;
}



xemem_segid_t *
hdb_get_segments(hdb_db_t db,
		 int    * num_segments)
{
    xemem_segid_t * segid_arr = NULL;
    wg_int lock_id;

    if (!num_segments) {
        return NULL;
    }

    lock_id = wg_start_read(db);

    if (!lock_id) {
        ERROR("Could not lock database\n");
        return NULL;
    }

    segid_arr = __get_segments(db, num_segments);

    if (!wg_end_read(db, lock_id))
        ERROR("Catastrophic database locking error\n");

    return segid_arr;
}




/* *******
 * 
 *  Applications
 * 
 * *******/


/**
 * Get an app handle from an app id
 *  - Returns NULL if no app is found 
 **/
static hdb_app_t
__get_app_by_id(hdb_db_t    db, 
		hobbes_id_t app_id) 
{
    hdb_app_t     app  = NULL;
    wg_query    * query    = NULL;
    wg_query_arg  arglist[2];

    arglist[0].column = HDB_TYPE_FIELD;
    arglist[0].cond   = WG_COND_EQUAL;
    arglist[0].value  = wg_encode_query_param_int(db, HDB_REC_APP);    

    arglist[1].column = HDB_APP_ID;
    arglist[1].cond   = WG_COND_EQUAL;
    arglist[1].value  = wg_encode_query_param_int(db, app_id);

    query = wg_make_query(db, NULL, 0, arglist, 2);

    app = wg_fetch(db, query);

    wg_free_query(db, query);
    wg_free_query_param(db, arglist[0].value);
    wg_free_query_param(db, arglist[1].value);

    return app;
}





/**
 * Get an app handle from an app name
 *  - Returns NULL if no app is found 
 **/
static hdb_app_t
__get_app_by_name(hdb_db_t   db, 
		  char     * name)
{
    hdb_app_t       app      = NULL;
    wg_query      * query    = NULL;
    wg_query_arg    arglist[2];    
 
    arglist[0].column = HDB_TYPE_FIELD;
    arglist[0].cond   = WG_COND_EQUAL;
    arglist[0].value  = wg_encode_query_param_int(db, HDB_REC_APP);    

    arglist[1].column = HDB_APP_NAME;
    arglist[1].cond   = WG_COND_EQUAL;
    arglist[1].value  = wg_encode_query_param_str(db, name, NULL);

    query = wg_make_query(db, NULL, 0, arglist, 2);
    app   = wg_fetch(db, query);

    wg_free_query(db, query);
    wg_free_query_param(db, arglist[0].value);
    wg_free_query_param(db, arglist[1].value);

    return app;
}




static hobbes_id_t
__create_app_record(hdb_db_t    db,
		    char      * name, 
		    hobbes_id_t enclave_id,
		    hobbes_id_t hio_app_id)
{
    void       * hdr_rec       = NULL;
    hobbes_id_t  app_id        = HOBBES_INVALID_ID;
    uint32_t     app_cnt       = 0;
    char         auto_name[32] = {[0 ... 31] = 0};

    hdb_app_t app   = NULL;

    
    hdr_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_APP_HDR, NULL);
    
    if (!hdr_rec) {
	ERROR("malformed database. Missing app Header\n");
	return HOBBES_INVALID_ID;
    }

 
    /* Get Next Available app ID and app count */
    app_id  = wg_decode_int(db, wg_get_field(db, hdr_rec, HDB_APP_HDR_NEXT));

    /* Verify the app id is available */
    if (__get_app_by_id(db, app_id)) {
	ERROR("App with ID (%d) already exists\n", app_id);
	return HOBBES_INVALID_ID;
    }

    app_cnt = wg_decode_int(db, wg_get_field(db, hdr_rec, HDB_APP_HDR_CNT));
    
    if (name == NULL) {
	snprintf(auto_name, 31, "app-%d", app_id);
	name = auto_name;
    }

    /* Verify that name is available */
    if (__get_app_by_name(db, name)) {
	ERROR("App with the name (%s) already exists\n", name);
	return HOBBES_INVALID_ID;
    }
 
    /* Insert app into the db */
    app = wg_create_record(db, 6);
    wg_set_field(db, app, HDB_TYPE_FIELD,      wg_encode_int(db, HDB_REC_APP));
    wg_set_field(db, app, HDB_APP_ID,          wg_encode_int(db, app_id));
    wg_set_field(db, app, HDB_APP_STATE,       wg_encode_int(db, APP_INITTED));
    wg_set_field(db, app, HDB_APP_NAME,	       wg_encode_str(db, name, NULL));
    wg_set_field(db, app, HDB_APP_ENCLAVE,     wg_encode_int(db, enclave_id));
    wg_set_field(db, app, HDB_APP_HIO_APP_ID,  wg_encode_int(db, hio_app_id));
    
    /* Update the app Header information */
    wg_set_field(db, hdr_rec, HDB_APP_HDR_NEXT, wg_encode_int(db, app_id  + 1));
    wg_set_field(db, hdr_rec, HDB_APP_HDR_CNT,  wg_encode_int(db, app_cnt + 1));

    return app_id;
}

hobbes_id_t 
hdb_create_app(hdb_db_t    db,
	       char      * name, 
	       hobbes_id_t enclave_id,
	       hobbes_id_t hio_app_id)
{
    wg_int      lock_id;
    hobbes_id_t app_id = HOBBES_INVALID_ID;

    lock_id = wg_start_write(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return HOBBES_INVALID_ID;
    }

    app_id = __create_app_record(db, name, enclave_id, hio_app_id);
    
    if (!wg_end_write(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return HOBBES_INVALID_ID;
    }
    

    return app_id;
}


static int
__delete_app(hdb_db_t    db,
	     hobbes_id_t app_id)
{
    uint32_t      app_cnt = 0;
    void        * hdr_rec = NULL;
    hdb_app_t     app     = NULL;


    hdr_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_APP_HDR, NULL);
    
    if (!hdr_rec) {
	ERROR("Malformed database. Missing app Header\n");
	return -1;
    }
    
    app = __get_app_by_id(db, app_id);
  
    if (!app) {
	ERROR("Could not find app (id: %d)\n", app_id);
	return -1;
    }

    if (wg_delete_record(db, app) != 0) {
	ERROR("Could not delete app from database\n");
	return -1;
    }

    app_cnt = wg_decode_int(db, wg_get_field(db, hdr_rec, HDB_APP_HDR_CNT));
    wg_set_field(db, hdr_rec, HDB_APP_HDR_CNT, wg_encode_int(db, app_cnt - 1));

    return 0;
}



int 
hdb_delete_app(hdb_db_t    db,
	       hobbes_id_t app_id)
{
    wg_int lock_id;
    int    ret = 0;

    lock_id = wg_start_write(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    ret = __delete_app(db, app_id);
    
    if (!wg_end_write(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }
    

    return ret;
}




static int
__set_app_state(hdb_db_t        db, 
		hobbes_id_t     app_id, 
		app_state_t state)
{
    hdb_app_t app = NULL;

    app = __get_app_by_id(db, app_id);
    
    if (app == NULL) {
	ERROR("Could not find app (id: %d)\n", app_id);
	return -1;
    }

    wg_set_field(db, app, HDB_APP_STATE, wg_encode_int(db, state));

    return 0;
}

int
hdb_set_app_state(hdb_db_t        db, 
		  hobbes_id_t     app_id, 
		  app_state_t state)
{
    wg_int lock_id;
    int ret = 0;

    lock_id = wg_start_write(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    ret = __set_app_state(db, app_id, state);
    
    if (!wg_end_write(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }
    

    return ret;
}


static hobbes_id_t
__get_app_id(hdb_db_t   db, 
	     char     * app_name)
{
    hdb_app_t   app    = NULL;
    hobbes_id_t app_id = HOBBES_INVALID_ID;

    app = __get_app_by_name(db, app_name);
    
    if (app == NULL) {
	ERROR("Could not find app (name: %s)\n", app_name);
	return HOBBES_INVALID_ID;
    }

    app_id = wg_decode_int(db, wg_get_field(db, app, HDB_APP_ID));

    return app_id;
}

hobbes_id_t
hdb_get_app_id(hdb_db_t   db, 
	       char     * app_name)
{
    wg_int      lock_id;
    hobbes_id_t app_id = HOBBES_INVALID_ID;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return HOBBES_INVALID_ID;
    }

   app_id = __get_app_id(db, app_name);
    
    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return HOBBES_INVALID_ID;
    }
    
    return app_id;
}

static hobbes_id_t 
__get_app_enclave(hdb_db_t    db,
		  hobbes_id_t app_id)
{
    hdb_app_t     app        = NULL;
    hobbes_id_t   enclave_id = HOBBES_INVALID_ID;


    app = __get_app_by_id(db, app_id);
    
    if (app == NULL) {
	ERROR("Could not find app (id: %d)\n", app_id);
	return HOBBES_INVALID_ID;
    }

    enclave_id = wg_decode_int(db, wg_get_field(db, app, HDB_APP_ENCLAVE));

    return enclave_id;
}

hobbes_id_t
hdb_get_app_enclave(hdb_db_t    db,
		    hobbes_id_t app_id)
{
    wg_int      lock_id;
    hobbes_id_t enclave_id = HOBBES_INVALID_ID;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return HOBBES_INVALID_ID;
    }

    enclave_id = __get_app_enclave(db, app_id);
    
    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return HOBBES_INVALID_ID;
    }
    

    return enclave_id;
}

static char *
__get_app_name(hdb_db_t    db, 
	       hobbes_id_t app_id)
{
    hdb_app_t app = NULL;

    char * name = NULL;

    app = __get_app_by_id(db, app_id);
    
    if (app == NULL) {
	ERROR("Could not find app (id: %d)\n", app_id);
	return NULL;
    }

    name = wg_decode_str(db, wg_get_field(db, app, HDB_APP_NAME));

    return name;
}

char * 
hdb_get_app_name(hdb_db_t    db, 
		 hobbes_id_t app_id)
{
    wg_int lock_id;
    char * name = NULL;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return NULL;
    }

    name = __get_app_name(db, app_id);
    
    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return NULL;
    }
    
    return name;
}

static app_state_t
__get_app_state(hdb_db_t    db, 
		hobbes_id_t app_id)
{
    hdb_app_t   app   = NULL;
    app_state_t state = APP_ERROR;

    app = __get_app_by_id(db, app_id);
    
    if (app == NULL) {
	ERROR("Could not find app (id: %d)\n", app_id);
	return APP_ERROR;
    }

    state = wg_decode_int(db, wg_get_field(db, app, HDB_APP_STATE));

    return state;
}

app_state_t
hdb_get_app_state(hdb_db_t    db, 
		  hobbes_id_t app_id)
{
    wg_int lock_id;
    app_state_t state = 0;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return APP_ERROR;
    }

    state = __get_app_state(db, app_id);
    
    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return APP_ERROR;
    }
    

    return state;
}

static hobbes_id_t
__get_app_hio_id(hdb_db_t    db, 
		 hobbes_id_t app_id)
{
    hdb_app_t   app    = NULL;
    hobbes_id_t hio_id = HOBBES_INVALID_ID;

    app = __get_app_by_id(db, app_id);
    
    if (app == NULL) {
	ERROR("Could not find app (id: %d)\n", app_id);
	return HOBBES_INVALID_ID;
    }

    hio_id = wg_decode_int(db, wg_get_field(db, app, HDB_APP_HIO_APP_ID));

    return hio_id;
}

hobbes_id_t
hdb_get_app_hio_id(hdb_db_t    db, 
		   hobbes_id_t app_id)
{
    wg_int lock_id;
    hobbes_id_t hio_id = HOBBES_INVALID_ID;

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return HOBBES_INVALID_ID;
    }

    hio_id = __get_app_hio_id(db, app_id);
    
    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return HOBBES_INVALID_ID;
    }
    
    return hio_id;
}


static hobbes_id_t *
__get_apps(hdb_db_t   db,
	   int      * num_apps)
{
    hobbes_id_t * id_arr  = NULL;
    void        * db_rec  = NULL;
    void        * hdr_rec = NULL;
    int           cnt     = 0;
    int           i       = 0;
    
    hdr_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_APP_HDR, NULL);    

    if (!hdr_rec) {
	ERROR("Malformed database. Missing app Header\n");
	return NULL;
    }

    cnt = wg_decode_int(db, wg_get_field(db, hdr_rec, HDB_APP_HDR_CNT));

    if (cnt == 0) {
	*num_apps = 0;
	return NULL;
    }

    id_arr = calloc(sizeof(hobbes_id_t), cnt);

    for (i = 0; i < cnt; i++) {
	db_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_APP, db_rec);
	

	if (!db_rec) {
	    ERROR("Application Header state mismatch\n");
	    cnt = i;
	    break;
	}

	id_arr[i] = wg_decode_int(db, wg_get_field(db, db_rec, HDB_APP_ID));

    }

    *num_apps = cnt;
    return id_arr;
}


hobbes_id_t * 
hdb_get_apps(hdb_db_t   db,
	     int      * num_apps)
{
    hobbes_id_t * id_arr = NULL;
    wg_int        lock_id;

    if (!num_apps) {
	return NULL;
    }

    lock_id = wg_start_read(db);

    if (!lock_id) {
	ERROR("Could not lock database\n");
	return NULL;
    }

    id_arr = __get_apps(db, num_apps);

    if (!wg_end_read(db, lock_id)) {
	ERROR("Catastrophic database locking error\n");
	return NULL;
    }

    return id_arr;
}




/*
 * PMI Key Value Store
 */


/* This assumes the database lock is held */
static int
__put_pmi_keyval(hdb_db_t        db,
		 int             appid,
		 const char *    kvsname,
		 const char *    key,
		 const char *    val)
{
    void * rec = NULL;

    /* Insert the PMI Key/Value into the database */
    rec = wg_create_record(db, 5);
    wg_set_field(db, rec, HDB_TYPE_FIELD,            wg_encode_int(db, HDB_REC_PMI_KEYVAL));
    wg_set_field(db, rec, HDB_PMI_KVS_ENTRY_APPID,   wg_encode_int(db, appid));
    wg_set_field(db, rec, HDB_PMI_KVS_ENTRY_KVSNAME, wg_encode_str(db, (char *)kvsname, NULL));
    wg_set_field(db, rec, HDB_PMI_KVS_ENTRY_KEY,     wg_encode_str(db, (char *)key, NULL));
    wg_set_field(db, rec, HDB_PMI_KVS_ENTRY_VALUE,   wg_encode_str(db, (char *)val, NULL));

    return 0;
}


int
hdb_put_pmi_keyval(hdb_db_t      db,
		   int           appid,
		   const char *  kvsname,
		   const char *  key,
		   const char *  val)
{
    wg_int lock_id;
    int    ret;

    lock_id = wg_start_write(db);
    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    ret = __put_pmi_keyval(db, appid, kvsname, key, val);

    if (!wg_end_write(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }

    return ret;
}


static int
__get_pmi_keyval(hdb_db_t        db,
		 int             appid,
		 const char *    kvsname,
		 const char *    key,
		 const char **   val)
{
    hdb_pmi_keyval_t kvs_entry = NULL;
    wg_query *       query     = NULL;
    wg_query_arg     arglist[4];
    int              ret = -1;

    /* Build a database query to lookup the key */
    arglist[0].column = HDB_TYPE_FIELD;
    arglist[0].cond   = WG_COND_EQUAL;
    arglist[0].value  = wg_encode_query_param_int(db, HDB_REC_PMI_KEYVAL);

    arglist[1].column = HDB_PMI_KVS_ENTRY_APPID;
    arglist[1].cond   = WG_COND_EQUAL;
    arglist[1].value  = wg_encode_query_param_int(db, appid);

    arglist[2].column = HDB_PMI_KVS_ENTRY_KVSNAME;
    arglist[2].cond   = WG_COND_EQUAL;
    arglist[2].value  = wg_encode_query_param_str(db, (char *)kvsname, NULL);

    arglist[3].column = HDB_PMI_KVS_ENTRY_KEY;
    arglist[3].cond   = WG_COND_EQUAL;
    arglist[3].value  = wg_encode_query_param_str(db, (char *)key, NULL);

    /* Execute the query */
    query = wg_make_query(db, NULL, 0, arglist, 4);
    kvs_entry = wg_fetch(db, query);

    /* If the query succeeded, decode the value string */
    if (kvs_entry) {
	*val = wg_decode_str(db, wg_get_field(db, kvs_entry, HDB_PMI_KVS_ENTRY_VALUE));
	ret = 0;
    }

    /* Free memory */
    wg_free_query(db, query);
    wg_free_query_param(db, arglist[0].value);
    wg_free_query_param(db, arglist[1].value);
    wg_free_query_param(db, arglist[2].value);
    wg_free_query_param(db, arglist[3].value);

    return ret;
}


int
hdb_get_pmi_keyval(hdb_db_t      db,
		   int           appid,
		   const char *  kvsname,
		   const char *  key,
		   const char ** val)
{
    wg_int lock_id;
    int    ret;

    lock_id = wg_start_read(db);
    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    ret = __get_pmi_keyval(db, appid, kvsname, key, val);

    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }

    return ret;
}

static hdb_pmi_barrier_t
__get_pmi_barrier(hdb_db_t  db,
                  int       appid)
{
    hdb_pmi_barrier_t         barrier_entry = NULL;
    wg_query                * query         = NULL;
    wg_query_arg              arglist[2];

    arglist[0].column = HDB_TYPE_FIELD;
    arglist[0].cond   = WG_COND_EQUAL;
    arglist[0].value  = wg_encode_query_param_int(db, HDB_REC_PMI_BARRIER);

    arglist[1].column = HDB_PMI_BARRIER_APPID;
    arglist[1].cond   = WG_COND_EQUAL;
    arglist[1].value  = wg_encode_query_param_int(db, appid);

    query = wg_make_query(db, NULL, 0, arglist, 2);

    barrier_entry = wg_fetch(db, query);

    wg_free_query(db, query);
    wg_free_query_param(db, arglist[0].value);
    wg_free_query_param(db, arglist[1].value);

    return barrier_entry;
}

int
hdb_create_pmi_barrier(hdb_db_t      db,
		       int           appid,
		       int           rank,
		       int           size,
		       xemem_segid_t segid)
{
    wg_int lock_id;
    void * rec = NULL;

    if ((lock_id = wg_start_write(db)) == 0) {
	ERROR("Could not lock database\n");
	return -1;
    }

    if ((rec = __get_pmi_barrier(db, appid)) == NULL) {
	/* Create the pmi barrier entry if no earlier process has created the pmi barrier entry */
	rec = wg_create_record(db, size+3);
	wg_set_field(db, rec, HDB_TYPE_FIELD, wg_encode_int(db, HDB_REC_PMI_BARRIER));
	wg_set_field(db, rec, HDB_PMI_BARRIER_APPID, wg_encode_int(db, appid));
	wg_set_field(db, rec, HDB_PMI_BARRIER_COUNTER, wg_encode_int(db, 0));
    }

    wg_set_field(db, rec, HDB_PMI_BARRIER_COUNTER+rank+1, wg_encode_int(db, segid));

    if (!(wg_end_write(db, lock_id))) {
	ERROR("Could not unlock database\n");
	return -1;
    }

    return 0;
}

int
hdb_pmi_barrier_increment(hdb_db_t db,
			  int      appid)
{
    wg_int lock_id;

    if ((lock_id = wg_start_write(db)) == 0) {
	ERROR("Could not lock database\n");
	return -1;
    }

    hdb_pmi_barrier_t barrier_entry = __get_pmi_barrier(db, appid);

    /* Increment the barrier counter */
    int count = wg_decode_int(db, wg_get_field(db, barrier_entry, HDB_PMI_BARRIER_COUNTER));
    count++;
    wg_set_field(db, barrier_entry, HDB_PMI_BARRIER_COUNTER, wg_encode_int(db, count));

    if (!(wg_end_write(db, lock_id))) {
	ERROR("Could not unlock database\n");
	return -1;
    }

    return count;
}

xemem_segid_t *
hdb_pmi_barrier_retire(hdb_db_t         db,
		       int              appid,
		       int              size)
{
    wg_int lock_id;

    if ((lock_id = wg_start_write(db)) == 0) {
	ERROR("Could not lock database\n");
	return NULL;
    }

    hdb_pmi_barrier_t barrier_entry = __get_pmi_barrier(db, appid);
    
    xemem_segid_t* segids = (xemem_segid_t*) calloc(sizeof(xemem_segid_t), size);

    int i;
    for(i=0; i<size; ++i) {
	segids[i] = wg_decode_int(db, wg_get_field(db, barrier_entry, HDB_PMI_BARRIER_COUNTER+i+1));
    }
    
    /* Reset the counter and wake up all other processes in the barrier */
    wg_set_field(db, barrier_entry, HDB_PMI_BARRIER_COUNTER, wg_encode_int(db, 0));

    if (!(wg_end_write(db, lock_id))) {
	ERROR("Could not unlock database\n");
	return NULL;
    }

    return segids;
}







/*
 * Hobbes Notification 
 */


static hdb_notif_t 
__get_notifier_by_segid(hdb_db_t      db,
			xemem_segid_t segid)
{
    hdb_notif_t   notifier = NULL;
    wg_query    * query    = NULL;
    wg_query_arg  arglist[2];

    arglist[0].column = HDB_TYPE_FIELD;
    arglist[0].cond   = WG_COND_EQUAL;
    arglist[0].value  = wg_encode_query_param_int(db, HDB_REC_NOTIFIER);

    arglist[1].column = HDB_NOTIF_SEGID;
    arglist[1].cond   = WG_COND_EQUAL;
    arglist[1].value  = wg_encode_query_param_int(db, segid);

    query    = wg_make_query(db, NULL, 0, arglist, 2);
    notifier = wg_fetch(db, query);

    wg_free_query(db, query);
    wg_free_query_param(db, arglist[0].value);
    wg_free_query_param(db, arglist[1].value);
		      
    return notifier;
}



static int
__create_notifier(hdb_db_t      db,
		  xemem_segid_t segid,
		  uint64_t      evt_mask)
{
    hdb_notif_t notifier  = NULL;

    if (__get_notifier_by_segid(db, segid)) {
	ERROR("Notifier already exists for this segid (%lu)\n", segid);
	return -1;
    }

    notifier = wg_create_record(db, 3);
    wg_set_field(db, notifier, HDB_TYPE_FIELD,      wg_encode_int(db, HDB_REC_NOTIFIER ));
    wg_set_field(db, notifier, HDB_NOTIF_SEGID,     wg_encode_int(db, segid            ));
    wg_set_field(db, notifier, HDB_NOTIF_EVT_MASK,  wg_encode_int(db, evt_mask         ));

    return 0;
}


int
hdb_create_notifier(hdb_db_t      db,
		    xemem_segid_t segid,
		    uint64_t      events)
{
    wg_int lock_id;
    int    ret = 0;
	
    lock_id = wg_start_write(db);
    
    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    ret = __create_notifier(db, segid, events);

    if (!wg_end_write(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }
    
    return ret;
}


static int
__delete_notifier(hdb_db_t      db,
		  xemem_segid_t segid)
{
    hdb_notif_t notifier = NULL;
    
    notifier = __get_notifier_by_segid(db, segid);

    if (!notifier) {
	ERROR("Could not find notifier (segid=%lu)\n", segid);
	return -1;
    }
    
    if (wg_delete_record(db, notifier) != 0) {
	ERROR("Could not delete notifier record from database\n");
	return -1;
    }

    return 0;
}



int
hdb_delete_notifier(hdb_db_t      db,
		    xemem_segid_t segid)
{
    wg_int lock_id;
    int    ret = 0;
	
    lock_id = wg_start_write(db);
    
    if (!lock_id) {
	ERROR("Could not lock database\n");
	return -1;
    }

    ret = __delete_notifier(db, segid);

    if (!wg_end_write(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return -1;
    }
    
    return ret;
}


static xemem_segid_t *
__get_event_subscribers(hdb_db_t   db,
			uint64_t   evt_mask,
			uint32_t * subs_cnt)
{
    void         * db_rec  = NULL;

    xemem_segid_t * segids = NULL;

    uint32_t rec_idx = 0;
    uint32_t max_cnt = 16;

    segids  = calloc(sizeof(xemem_segid_t), max_cnt);

    if (segids == NULL) {
	ERROR("Could not allocate segid array\n");
	return NULL;
    }
    
    while ((db_rec = wg_find_record_int(db, HDB_TYPE_FIELD, WG_COND_EQUAL, HDB_REC_NOTIFIER, db_rec)) != 0) {
	uint64_t rec_evt_mask = 0;

	if (rec_idx >= max_cnt) {
	    max_cnt *= 2;
	    segids = realloc(segids, sizeof(xemem_segid_t) * max_cnt);
	}

	rec_evt_mask = wg_decode_int(db, wg_get_field(db, db_rec, HDB_NOTIF_EVT_MASK));

	if ((rec_evt_mask & evt_mask) != 0) {
	    segids[rec_idx] = wg_decode_int(db, wg_get_field(db, db_rec, HDB_NOTIF_SEGID));
	    rec_idx++;
	}
    }

    
    *subs_cnt = rec_idx;
    return segids;
}



xemem_segid_t * 
hdb_get_event_subscribers(hdb_db_t   db,
			  uint64_t   evt_mask,
			  uint32_t * subs_cnt)
{
    wg_int lock_id;
    xemem_segid_t * segids = NULL;
	
    lock_id = wg_start_read(db);
    
    if (!lock_id) {
	ERROR("Could not lock database\n");
	return NULL;
    }

    segids = __get_event_subscribers(db, evt_mask, subs_cnt);

    if (!wg_end_read(db, lock_id)) {
	ERROR("Apparently this is catastrophic...\n");
	return NULL;
    }
    
    return segids;
}
