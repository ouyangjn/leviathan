#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#include <stdint.h>

#include <pet_log.h>

#include <cmd_queue.h>
#include <xemem.h>
#include <enclave.h>


int main(int argc, char ** argv) {

    hcq_handle_t hcq = HCQ_INVALID_HANDLE;
    xemem_segid_t segid = -1;
    hcq_cmd_t cmd = HCQ_INVALID_CMD;


    char * data_buf = "Hello Hobbes";

    hobbes_client_init();

    hcq = enclave_open_cmd_queue("master");

    cmd = hcq_cmd_issue(hcq, 5, strlen(data_buf) + 1, data_buf);

    
    printf("cmd = %llu\n", cmd);
			    

    hcq_disconnect(hcq);
    hobbes_client_deinit();

    return 0;

}
