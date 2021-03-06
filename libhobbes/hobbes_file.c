/* 
 * Copyright (c) 2015, Jack Lange <jacklange@cs.pitt.edu>
 * All rights reserved.
 *
 * This is free software.  You are permitted to use,
 * redistribute, and modify it as specified in the file "PETLAB_LICENSE".
 */


#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#include <pet_log.h>
#include <pet_xml.h>

#include "hobbes_enclave.h"
#include "hobbes_util.h"
#include "hobbes_file.h"
#include "hobbes_cmd_queue.h"

#define MAX_XFER_SIZE (4096)


struct hobbes_file_state {
    hcq_handle_t hcq; 
    uint64_t     file_handle;
};



struct hfio_wr_req {
    uint64_t file_handle;
    uint64_t data_size;
    uint8_t  data[0];
} __attribute__((packed));

struct hfio_rd_req {
    uint64_t file_handle;
    uint64_t data_size;
} __attribute__((packed));


struct hfio_seek_req {
    uint64_t file_handle;
    uint64_t offset;
    uint32_t whence;
} __attribute__((packed));




int 
hfio_stat(hcq_handle_t   hcq, 
	  char         * path, 
	  struct stat  * buf)
{
    hcq_cmd_t cmd   = HCQ_INVALID_CMD;
    
    int       ret      = -1;
    int       ret_size =  0;
    uint8_t * ret_data =  NULL;


    cmd = hcq_cmd_issue(hcq, HOBBES_CMD_FILE_STAT, smart_strlen(path) + 1, path);

    if (cmd == HCQ_INVALID_CMD) {
	ERROR("Could not issue HFIO stat command\n");
	goto out;
    }

    ret = hcq_get_ret_code(hcq, cmd);
    
    if (ret == -1) {
	ERROR("Error executing HFIO stat command\n");
	goto out;
    }


    ret_data = hcq_get_ret_data(hcq, cmd, (void *)&ret_size);

    if (ret_data == NULL) {
	ERROR("Invalid return data in HFIO stat command\n");
	goto out;
    }

    if (ret_size != sizeof(struct stat)) {
	ERROR("Compatibility Error! Inconsistent 'struct stat' types across enclaves\n");
	goto out;
    }

    memcpy(buf, ret_data, ret_size);

 out:
    if (cmd != HCQ_INVALID_CMD) hcq_cmd_complete(hcq, cmd);

    return ret;
}


int 
hfio_fstat(hobbes_file_t   file, 
	   struct stat   * buf)
{
    hcq_cmd_t cmd   = HCQ_INVALID_CMD;

    int       ret      = -1;
    int       ret_size =  0;
    uint8_t * ret_data =  NULL;

    cmd = hcq_cmd_issue(file->hcq, 
			HOBBES_CMD_FILE_FSTAT, 
			sizeof(uint64_t),
			(void *)&(file->file_handle));

    if (cmd == HCQ_INVALID_CMD) {
	ERROR("Could not issue HFIO fstat command\n");
	goto out;
    }

    ret = hcq_get_ret_code(file->hcq, cmd);

    if (ret == -1) {
	ERROR("Error executing HFIO fstat command\n");
	goto out;
    }

    ret_data = hcq_get_ret_data(file->hcq, cmd, (void *)&ret_size);

    if (ret_data == NULL) {
	ERROR("Invalid return data in HFIO fstat command\n");
	goto out;
    }

    if (ret_size != sizeof(struct stat)) {
	ERROR("Compatibility Error! Inconsistent 'struct stat' types across enclaves\n");
	goto out;
    }

    memcpy(buf, ret_data, ret_size);

 out:
    if (cmd != HCQ_INVALID_CMD) hcq_cmd_complete(file->hcq, cmd);
    return ret;

}


static inline int
__sanitize_flags(int flags)
{
    int safe_flags = (O_RDONLY    | O_WRONLY  | O_RDWR     | \
		      O_APPEND    | O_CREAT   | O_EXCL     | \
		      O_LARGEFILE | O_NOATIME | O_NOFOLLOW | \
		      O_TRUNC);

    return (flags & safe_flags);
}





hobbes_file_t 
hfio_open(hcq_handle_t   hcq,
	  char         * path,
	  int            flags,
	  ...)
{
    struct hobbes_file_state * file = NULL;

    hcq_cmd_t   cmd         = HCQ_INVALID_CMD;
    pet_xml_t   cmd_xml     = PET_INVALID_XML;
    uint64_t  * file_handle = 0;

    char * tmp_str  = NULL;
    int    ret_size = 0;
    int    ret      = 0;


    file = calloc(sizeof(struct hobbes_file_state), 1);

    if (file == NULL) {
	ERROR("Could not allocate Hobbes file state\n");
	goto err;
    }

    cmd_xml = pet_xml_new_tree("file");

    if (cmd_xml == PET_INVALID_XML) {
	ERROR("Could not create xml command\n");
	goto err;
    }
    

    pet_xml_add_val(cmd_xml, "path",  path);
    
    if (asprintf(&tmp_str, "%u", __sanitize_flags(flags)) == -1) {
	tmp_str = NULL;
	goto err;
    }

    pet_xml_add_val(cmd_xml, "flags", tmp_str);

    smart_free(tmp_str);

    if (flags & O_CREAT) {
	va_list  ap;
	uint32_t mode = 0;
 
	va_start(ap, flags);
	mode = va_arg(ap, uint32_t);
	va_end(ap);

	if (asprintf(&tmp_str, "%u", mode) == -1) {
	    tmp_str = NULL;
	    goto err;
	}

	pet_xml_add_val(cmd_xml, "mode", tmp_str);
	
	smart_free(tmp_str);
    }


    tmp_str = pet_xml_get_str(cmd_xml);

    cmd = hcq_cmd_issue(hcq, HOBBES_CMD_FILE_OPEN, strlen(tmp_str) + 1, tmp_str);
    
    smart_free(tmp_str);

    if (cmd == HCQ_INVALID_CMD) {
	ERROR("Error issueing open file command (%s)\n", path);
	goto err;
    }

    ret = hcq_get_ret_code(hcq, cmd);

    if (ret != 0) {
	ERROR("Error opening hobbes file (%s) [ret=%d]\n", path, ret);
	goto err;
    }

    file_handle = (uint64_t *)hcq_get_ret_data(hcq, cmd, (void *)&ret_size);

    if (ret_size != sizeof(uint64_t)) {
	ERROR("Invalid return size from Hobbes File Open\n");
	goto err;
    }

    /* Store state info in the file structure */
    file->hcq         =  hcq;
    file->file_handle = *file_handle;

    hcq_cmd_complete(hcq, cmd);

    pet_xml_free(cmd_xml);

    return file;

    
 err:
    if (tmp_str != NULL)            smart_free(tmp_str);		 
    if (file    != NULL)            smart_free(file);
    if (cmd_xml != PET_INVALID_XML) pet_xml_free(cmd_xml);
    if (cmd     != HCQ_INVALID_CMD) hcq_cmd_complete(hcq, cmd);
    return HOBBES_INVALID_FILE;
}


void
hfio_close(hobbes_file_t file)
{
    hcq_cmd_t cmd = HCQ_INVALID_CMD;
    
    cmd = hcq_cmd_issue(file->hcq, HOBBES_CMD_FILE_CLOSE, sizeof(uint64_t), (void *)&file->file_handle);

    if (cmd == HCQ_INVALID_CMD) {
	return;
    }


    hcq_cmd_complete(file->hcq, cmd);

    smart_free(file);
    return;
}


ssize_t
hfio_read(hobbes_file_t   file, 
	  char          * buf,
	  size_t          count)
{
    hcq_cmd_t cmd = HCQ_INVALID_CMD;
    ssize_t   ret = 0;

    struct hfio_rd_req req;


    if (count > MAX_XFER_SIZE) {
	count = MAX_XFER_SIZE;
    }

    memset(&req, 0, sizeof(struct hfio_rd_req));


    req.file_handle = file->file_handle;
    req.data_size   = count;
    
    cmd = hcq_cmd_issue(file->hcq, HOBBES_CMD_FILE_READ, sizeof(struct hfio_rd_req), &req);


    if (cmd == HCQ_INVALID_CMD) {
	ERROR("Could not issue HFIO read command\n");
	goto out;
    }
    
    ret = hcq_get_ret_code(file->hcq, cmd);

    if (ret > 0) {
	void     * data_ptr  = NULL;
	uint32_t   data_size = 0;
	
	data_ptr = hcq_get_ret_data(file->hcq, cmd, &data_size);

	if (count < data_size) {
	    ERROR("Read more than the requested amount of data\n");
	    ERROR("File position is likely inconsistent\n");
	} else if (count > data_size) {
	    count = data_size;
	}

	memcpy(buf, data_ptr, count);
	
	ret = count;
    }

 out:
    
    if (cmd != HCQ_INVALID_CMD) hcq_cmd_complete(file->hcq, cmd);

    return ret;
}


ssize_t
hfio_write(hobbes_file_t file,
	   const char  * buf, 
	   size_t        count)
{
    hcq_cmd_t cmd =  HCQ_INVALID_CMD;
    ssize_t   ret = -1;

    struct hfio_wr_req * req = NULL;


    if (count > MAX_XFER_SIZE) {
	count = MAX_XFER_SIZE;
    }

    req = calloc(sizeof(struct hfio_wr_req) + count, 1);
    
    if (req == NULL) {
	ERROR("Could not allocate memory for write request\n");
	goto out;
    }

    req->file_handle = file->file_handle;
    req->data_size   = count;

    memcpy(req->data, buf, count);
    
    cmd = hcq_cmd_issue(file->hcq, HOBBES_CMD_FILE_WRITE, sizeof(struct hfio_wr_req) + count, req);

    if (cmd == HCQ_INVALID_CMD) {
	ERROR("Could not issue HFIO write command\n");
	goto out;
    }

    ret = hcq_get_ret_code(file->hcq, cmd);

 out:
    if (req != NULL)            smart_free(req);
    if (cmd != HCQ_INVALID_CMD) hcq_cmd_complete(file->hcq, cmd);

    return ret;
}


off_t
hfio_lseek(hobbes_file_t file, 
	   off_t         offset, 
	   int           whence)
{
    hcq_cmd_t cmd     =  HCQ_INVALID_CMD;
    off_t     ret     = -1;

    void   * ret_data = NULL;
    uint32_t ret_size = 0;

    struct hfio_seek_req req;

    memset(&req, 0, sizeof(struct hfio_seek_req));

    req.file_handle = file->file_handle;
    req.offset      = offset;
    req.whence      = whence;

    cmd = hcq_cmd_issue(file->hcq, HOBBES_CMD_FILE_SEEK, sizeof(struct hfio_seek_req), (void *)&req);
    
    if (cmd == HCQ_INVALID_CMD) {
	ERROR("Could not issue HFIO seek command\n");
	goto out;
    }

    if (hcq_get_ret_code(file->hcq, cmd) != 0) {
	ERROR("Could not execute HFIO seek command\n");
	goto out;
    }
    
    ret_data = hcq_get_ret_data(file->hcq, cmd, &ret_size);

    if (ret_size == 0) {
	ERROR("Invalid return value from HFIO seek command\n");
	goto out;
    }
    
    ret = *(off_t *)ret_data;

 out:
    if (cmd != HCQ_INVALID_CMD) hcq_cmd_complete(file->hcq, cmd);
    return ret;
}


ssize_t
hfio_read_file(hobbes_file_t hfile,
       	       char        * buf,
	       size_t        count)
{
    ssize_t bytes_total     = 0;
    ssize_t bytes_requested = 0;
    ssize_t bytes_read      = 0;

    while (1) {
	bytes_requested = count;
	if (bytes_requested > HFIO_MAX_XFER_SIZE)
	    bytes_requested = HFIO_MAX_XFER_SIZE;
	
	/* Read a chunk */
	bytes_read = hfio_read(hfile, &(buf[bytes_total]), bytes_requested);
	if (bytes_read == 0)
	    break;

	/* Update the bytes read and truncate the destination buf */
	bytes_total += bytes_read;
	count       -= bytes_read;
	buf[bytes_read] = 0;

	if (count == 0)
	    break;
    }

    return bytes_read;
}

ssize_t
hfio_write_file(hobbes_file_t hfile,
		const char  * buf,
		size_t        count)
{
    ssize_t bytes_total     = 0;
    ssize_t bytes_requested = 0;
    ssize_t bytes_written   = 0;

    while (1) {
	bytes_requested = count;
	if (bytes_requested > HFIO_MAX_XFER_SIZE)
	    bytes_requested = HFIO_MAX_XFER_SIZE;
	
	/* Write a chunk */
	bytes_written = hfio_write(hfile, &(buf[bytes_total]), bytes_requested);
	if (bytes_written == 0)
	    break;

	/* Update the bytes read and truncate the destination buf */
	bytes_total += bytes_written;
	count       -= bytes_written;

	if (count == 0)
	    break;
    }

    return bytes_written;
}

int
hobbes_copy_file(char      * path,
		 hobbes_id_t src_enclave,
		 hobbes_id_t dst_enclave)
{
    hcq_handle_t  src_hcq   = HCQ_INVALID_HANDLE;
    hcq_handle_t  dst_hcq   = HCQ_INVALID_HANDLE;
    hobbes_file_t src_hfile = HOBBES_INVALID_FILE;
    hobbes_file_t dst_hfile = HOBBES_INVALID_FILE;

    ssize_t bytes         = 0;
    ssize_t bytes_read    = 0;
    ssize_t bytes_written = 0;

    int    status = -1;
    char * buffer = NULL;
    struct stat st;

    /* Open command queues */
    {
	src_hcq = hobbes_open_enclave_cmdq(src_enclave);
	if (src_hcq == HCQ_INVALID_HANDLE) {
	    ERROR("Cannot open enclave %d command queue\n", src_enclave);
	    return -1;
	}

	dst_hcq = hobbes_open_enclave_cmdq(dst_enclave);
	if (dst_hcq == HCQ_INVALID_HANDLE) {
	    ERROR("Cannot open enclave %d command queue\n", dst_enclave);
	    goto dst_hcq_out;
	}
    }

    /* Open Hobbes files */
    {
	src_hfile = hfio_open(src_hcq, path, O_RDONLY);
	if (src_hfile == HOBBES_INVALID_FILE) {
	    ERROR("Cannot open %s in enclave %d\n", path, src_enclave);
	    goto src_hfile_out;
	}

	dst_hfile = hfio_open(dst_hcq, path, O_WRONLY | O_CREAT, S_IRWXU | S_IRWXG | S_IROTH);
	if (dst_hfile == HOBBES_INVALID_FILE) {
	    ERROR("Cannot open %s in enclave %d\n", path, dst_enclave);
	    goto dst_hfile_out;
	}
    }

    /* Stat to determine size */
    status = hfio_fstat(src_hfile, &st);
    if (status != 0) {
	ERROR("Cannot stat %s in enclave %d\n", path, src_enclave);
	goto stat_out;
    }

    bytes  = st.st_size;
    buffer = malloc(bytes);
    if (buffer == NULL) {
	ERROR("malloc: %s\n", strerror(errno));
	status = -errno;
	goto malloc_out;
    }
    
    /* Read it */
    bytes_read = hfio_read_file(src_hfile, buffer, bytes);
    if (bytes_read != bytes) {
	ERROR("Only read %lu bytes out of %lu bytes of file %s\n", bytes_read, bytes, path);
	status = -1;
	goto read_out;
    }

    /* Write it */
    bytes_written = hfio_write_file(dst_hfile, (const char *)buffer, bytes);
    if (bytes_written != bytes) {
	ERROR("Only wrote %lu bytes out of %lu bytes of file %s\n", bytes_written, bytes, path);
	status = -1;
	goto write_out;
    }

    status = 0;

write_out:
read_out:
    free(buffer);

malloc_out:
stat_out:
    hfio_close(dst_hfile);

dst_hfile_out:
    hfio_close(src_hfile);

src_hfile_out:
    hobbes_close_enclave_cmdq(dst_hcq);

dst_hcq_out:
    hobbes_close_enclave_cmdq(src_hcq);

    return status;
}
