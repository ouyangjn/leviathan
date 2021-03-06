#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <getopt.h>

#include <hobbes_db.h>
#include <hobbes_file.h>
#include <hobbes_util.h>
#include <hobbes_notifier.h>

#include <pet_xml.h>
#include <pet_log.h>


#include "enclave.h"
#include "vm.h"
#include "pisces.h"


extern hdb_db_t hobbes_master_db;

int 
hobbes_create_enclave(char * cfg_file_name, 
		      char * name)
{
    pet_xml_t   xml  = NULL;
    char      * type = NULL; 

    xml = pet_xml_open_file(cfg_file_name);
    
    if (xml == NULL) {
	ERROR("Error loading Enclave config file (%s)\n", cfg_file_name);
	return -1;
    }

    type =  pet_xml_tag_name(xml);

    if (strncmp("pisces", type, strlen("pisces")) != 0) {
	ERROR("Invalid Enclave Type (%s)\n", type);
	return -1;
    }

    DEBUG("Creating Pisces Enclave\n");
    if (pisces_enclave_create(xml, name) != 0 ) {
	ERROR("Could not create Pisces Enclave\n");
	return -1;
    }

    {
	hnotif_signal(HNOTIF_EVT_ENCLAVE);
    }

    return 0;

}



int 
hobbes_destroy_enclave(hobbes_id_t enclave_id)
{
    enclave_type_t enclave_type = INVALID_ENCLAVE;

    enclave_type = hdb_get_enclave_type(hobbes_master_db, enclave_id);

    if (enclave_type == INVALID_ENCLAVE) {
	ERROR("Could not find enclave (%d)\n", enclave_id);
	return -1;
    }
   
    if (enclave_type != PISCES_ENCLAVE) {
	ERROR("Enclave (%d) is not a native enclave\n", enclave_id);
	return -1;
    }

    if (pisces_enclave_destroy(enclave_id) != 0) {
	ERROR("Could not destroy pisces enclave\n");
	return -1;
    }

    {
	hnotif_signal(HNOTIF_EVT_ENCLAVE);
    }

    return 0;

}



int 
create_enclave_main(int argc, char ** argv)
{
    char * cfg_file = NULL;
    char * name     = NULL;


    if (argc < 1) {
	printf("Usage: hobbes create_enclave <cfg_file> [name]\n");
	return -1;
    }

    cfg_file = argv[1];
    
    if (argc >= 2) {
	name = argv[2];
    }

    return hobbes_create_enclave(cfg_file, name);
}


int 
destroy_enclave_main(int argc, char ** argv)
{
    hobbes_id_t enclave_id = HOBBES_INVALID_ID;

    if (argc < 1) {
	printf("Usage: hobbes destroy_enclave <enclave name>\n");
	return -1;
    }
    
    enclave_id = hobbes_get_enclave_id(argv[1]);
    

    return hobbes_destroy_enclave(enclave_id);
}



static void
__ping_usage()
{
    printf("ping: Ping an enclave via the command queue\n\n"		\
	   "Test utility to debug command queues and enclaves.\n\n"	\
	   "Usage: ping [options] <enclave name>\n"			\
	   "Options: \n"						\
	   "\t[-s|--size <size in bytes>    : Specify the size of the ping message\n" \
	   "\t[-w|--walk]                   : Ping size spectrum sweep (1 byte - 1MB in powers of 2)\n"
	   );
    exit(-1);
}

int
ping_enclave_main(int argc, char ** argv)
{
    hobbes_id_t enclave_id    = HOBBES_INVALID_ID;
    int         size_in_bytes = 0;
    int         do_walk       = 0;
    

    {
	int  opt_idx = 0;
	char c       = 0;
	
	opterr = 1;

	static struct option long_options[] = {
	    {"size",    required_argument, 0, 's'},
	    {"walk",    no_argument,       0, 'w'},
	    {0, 0, 0, 0}
	};


	while ((c = getopt_long(argc, argv, "s:w", long_options, &opt_idx)) != -1) {
	    switch (c) {
		case 's':
		    size_in_bytes = smart_atoi(0, optarg);
		    
		    if (size_in_bytes == 0) {
			__ping_usage();
			return -1;
		    }

		    break;
		case 'w':
		    do_walk = 1;
		    break;
		default:
		    __ping_usage();
		    return -1;
	    }
	    
	}
    }

    if (optind > argc) {
	__ping_usage();
	return -1;
    }

    enclave_id = hobbes_get_enclave_id(argv[optind]);
       
    if (enclave_id == HOBBES_INVALID_ID) {
	printf("Invalid Enclave\n");
	return -1;
    }


    if (do_walk == 0) {
	hobbes_ping_enclave(enclave_id, size_in_bytes);
    } else {
	// walk from 1 byte to 1MB

    }

    return 0;
}



int 
cat_file_main(int argc, char ** argv) 
{
    hobbes_id_t   enclave_id = HOBBES_INVALID_ID;
    hcq_handle_t  hcq        = HCQ_INVALID_HANDLE; 
    hobbes_file_t hfile      = HOBBES_INVALID_FILE;
    int           status     = -1;
    
    if (argc < 2) {
	printf("Usage: hobbes cat_file <enclave name> <path>\n");
	return -1;
    }

    enclave_id = hobbes_get_enclave_id(argv[1]);

    if (enclave_id == HOBBES_INVALID_ID) {
	ERROR("Invalid Enclave name (%s)\n", argv[1]);
	return -1;
    }

    hcq = hobbes_open_enclave_cmdq(enclave_id);

    if (hcq == HCQ_INVALID_HANDLE) {
	ERROR("Could not open command queue for enclave (%s)\n", argv[1]);
	return -1;
    }
    
    hfile = hfio_open(hcq, argv[2], O_RDONLY);

    if (hfile == HOBBES_INVALID_FILE) {
	ERROR("Could not open file (%s)\n", argv[2]);
	goto hfile_out;
    }

    {
	char  * tmp_buf    = NULL;
	off_t   bytes      = 0;
	ssize_t bytes_read = 0;
	struct  stat st;

	if (hfio_fstat(hfile, &st) != 0) {
	    ERROR("Could not stat file %s\n", argv[2]);
	    goto stat_out;
	}
	bytes = st.st_size;

	tmp_buf = calloc(bytes, 1);
	if (tmp_buf == NULL) {
	    ERROR("Could not allocate temporary read buffer\n");
	    goto calloc_out;
	}

	bytes_read = hfio_read_file(hfile, tmp_buf, bytes);
	if (bytes_read > 0)
	    printf("%s", tmp_buf);

	free(tmp_buf);
    }
    
calloc_out:
stat_out:
    hfio_close(hfile);

hfile_out:
    hobbes_close_enclave_cmdq(hcq);

    return status;
}


static void
sig_term_handler(int sig)
{
    return;
}


int
cat_into_file_main(int argc, char ** argv)
{
    hobbes_id_t   enclave_id    = HOBBES_INVALID_ID;
    hcq_handle_t  hcq           = HCQ_INVALID_HANDLE; 
    hobbes_file_t hfile         = HOBBES_INVALID_FILE;

    /* Prevent SIGINT from killing, rely on ctrl-d */
    {
        struct sigaction action;
        
        memset(&action, 0, sizeof(struct sigaction));
        action.sa_handler = sig_term_handler;
        
        if (sigaction(SIGINT, &action, 0)) {
            perror("sigaction");
            return -1;
        }
    }

     if (argc < 2) {
	printf("Usage: hobbes cat_file <enclave name> <path>\n");
	return -1;
    }

    enclave_id = hobbes_get_enclave_id(argv[1]);

    if (enclave_id == HOBBES_INVALID_ID) {
	ERROR("Invalid Enclave name (%s)\n", argv[1]);
	return -1;
    }

    hcq = hobbes_open_enclave_cmdq(enclave_id);

    if (hcq == HCQ_INVALID_HANDLE) {
	ERROR("Could not open command queue for enclave (%s)\n", argv[1]);
	return -1;
    }
    
    hfile = hfio_open(hcq, argv[2], O_WRONLY | O_CREAT, S_IRWXU | S_IRWXG | S_IROTH);

    if (hfile == HOBBES_INVALID_FILE) {
	ERROR("Could not open file (%s)\n", argv[2]);
	return -1;
    }

    {
	char tmp_buf[1024] = {[0 ... 1023] = 0};

	while (1) {
	    int     offset         = 0;
	    ssize_t bytes_read     = read(STDIN_FILENO, tmp_buf, sizeof(tmp_buf));
	    size_t  bytes_to_write = bytes_read;
	
	    if (bytes_read <= 0) {
		if (errno == EAGAIN)  continue;
		break;
	    }

	    printf("Sending %lu bytes\n", bytes_to_write);
	    while (bytes_to_write > 0) {
		ssize_t bytes_wrote = hfio_write(hfile, tmp_buf + offset, bytes_to_write);
		
		printf("wrote %ld bytes\n", bytes_wrote);

		if (bytes_wrote <= 0) {
		    break;
		}
	
		bytes_to_write -= bytes_wrote;
		offset         += bytes_wrote;
	    }
	
	    printf("sent\n");

	    if (bytes_to_write > 0) {
		break;
	    }
	}
    }

    hfio_close(hfile);

    hobbes_close_enclave_cmdq(hcq);

    return 0;
}


int 
dump_cmd_queue_main(int argc, char ** argv)
{
    hobbes_id_t  enclave_id = HOBBES_INVALID_ID;
    hcq_handle_t hcq        = HCQ_INVALID_HANDLE;

    if (argc < 1) {
	printf("Usage: hobbes ping_enclave <enclave name>\n");
	return -1;
    }

    enclave_id = hobbes_get_enclave_id(argv[1]);

    if (enclave_id == HOBBES_INVALID_ID) {
	printf("Invalid Enclave\n");
	return -1;
    }
  
    hcq = hobbes_open_enclave_cmdq(enclave_id);
    
    hcq_dump_queue(hcq);

    hobbes_close_enclave_cmdq(hcq);

    return 0;
}

int
list_enclaves_main(int argc, char ** argv)
{
    struct enclave_info * enclaves = NULL;
    uint32_t num_enclaves = 0;
    uint32_t i = 0;

    enclaves = hobbes_get_enclave_list(&num_enclaves);

    if (enclaves == NULL) {
	ERROR("Could not retrieve enclave list\n");
	return -1;
    }
	
    printf("%d Active Enclaves:\n", num_enclaves);
    printf("--------------------------------------------------------------------------------\n");
    printf("| ID       | Enclave name                     | Type             | State       |\n");
    printf("--------------------------------------------------------------------------------\n");

 
    for (i = 0; i < num_enclaves; i++) {
	printf("| %-*d | %-*s | %-*s | %-*s |\n", 
	       8, enclaves[i].id,
	       32, enclaves[i].name,
	       16, enclave_type_to_str(enclaves[i].type), 
	       11, enclave_state_to_str(enclaves[i].state));
    }

    printf("--------------------------------------------------------------------------------\n");

    free(enclaves);

    return 0;
}



int
console_main(int argc, char ** argv)
{
    hobbes_id_t    enclave_id   = HOBBES_INVALID_ID;
    enclave_type_t enclave_type = INVALID_ENCLAVE;

    if (argc < 2) {
	printf("Usage: hobbes enclave <enclave name>\n");
	return -1;
    }

    enclave_id = hobbes_get_enclave_id(argv[1]);

    if (enclave_id == HOBBES_INVALID_ID) {
	ERROR("Invalid enclave\n");
	return -1;
    }

    enclave_type = hobbes_get_enclave_type(enclave_id);

    if (enclave_type == INVALID_ENCLAVE) {
	ERROR("Could not find enclave (%d)\n", enclave_id);
	return -1;
    }



    switch (enclave_type) {
	case VM_ENCLAVE:
	    return vm_enclave_console(enclave_id);

	case PISCES_ENCLAVE:
	    return pisces_enclave_console(enclave_id);

	default:
	    ERROR("No console available for enclave type %d\n", enclave_type);
	    return -1;
    }
} 
