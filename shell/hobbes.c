/* Hobbes Management interface
 * (c) 2015, Jack Lange <jacklange@cs.pitt.edu>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xpmem.h>
#include <pet_log.h>

#include <hobbes_enclave.h>
#include <hobbes_client.h>

const char * hobbes_prog_version = "Hobbes 0.1";
const char * bug_email_addr      = "<jacklange@cs.pitt.edu>";



struct args 
{


};



static int
create_enclave_handler(int argc, char ** argv) 
{
    char * cfg_file = NULL;
    char * name     = NULL;

    if (argc < 1) {
	printf("Usage: hobbes create_enclave <cfg_file> [name] [-t <host_enclave>]\n");
	return -1;
    }

    cfg_file = argv[1];
    
    if (argc >= 2) {
	name = argv[2];
    }

    return hobbes_create_enclave(cfg_file, name);
}


static int 
destroy_enclave_handler(int argc, char ** argv)
{
    if (argc < 1) {
	printf("Usage: hobbes destroy_enclave <enclave name>\n");
	return -1;
    }

    return hobbes_destroy_enclave(argv[1]);
}



static int
list_enclaves_handler(int argc, char ** argv)
{
    struct enclave_info * enclaves = NULL;
    int num_enclaves = -1;
    int i = 0;

    enclaves = hobbes_get_enclave_list(&num_enclaves);

    if (enclaves == NULL) {
	ERROR("Could not retrieve enclave list\n");
	return -1;
    }
	
    printf("%d Active Enclaves:\n", num_enclaves);
 
    for (i = 0; i < num_enclaves; i++) {
	printf("%d: %-*s [%-*s] <%s>\n", 
	       enclaves[i].id,
	       35, enclaves[i].name,
	       16, enclave_type_to_str(enclaves[i].type), 
	       enclave_state_to_str(enclaves[i].state));

    }

    free(enclaves);

    return 0;
}

static int
list_segments_handler(int argc, char ** argv)
{
    struct xemem_segment * seg_arr = NULL;
    int num_segments = 0;
    int i = 0;

    if (argc != 1) {
        printf("Usage: hobbes list_segments\n");
        return -1;
    }

    seg_arr = xemem_get_segment_list(&num_segments);

    if (seg_arr == NULL) {
        ERROR("Could not retrieve XEMEM segment list\n");
        return -1;
    }

    printf("%d segments:\n", num_segments);

    for (i = 0; i < num_segments; i++) {
        printf("%s: %lu\n",
            seg_arr[i].name,
            seg_arr[i].segid);
    }

    free(seg_arr);

    return 0;
}



struct hobbes_cmd {
    char * name;
    int (*handler)(int argc, char ** argv);   
    char * desc;
};

extern int launch_app_main(int argc, char ** argv);

static struct hobbes_cmd cmds[] = {
    {"create_enclave",  create_enclave_handler,  "Create Native Enclave"},
    {"destroy_enclave", destroy_enclave_handler, "Destroy Native Enclave"},
    {"list_enclaves",   list_enclaves_handler,   "List all running enclaves"},
    {"list_segments",   list_segments_handler,   "List all exported xpmem segments"},
    {"launch_app",      launch_app_main,         "Launch an application in an enclave"},
    {0, 0, 0}
};



static void 
usage() 
{
    int i = 0;

    printf("Usage: hobbes <command> [args...]\n");
    printf("Commands:\n");

    while (cmds[i].name) {
	printf("\t%-17s -- %s\n", cmds[i].name, cmds[i].desc);
	i++;
    }

    return;
}


int 
main(int argc, char ** argv) 
{
    int i = 0;

    if (argc < 2) {
	usage();
	exit(-1);
    }

    if (hobbes_client_init() != 0) {
	ERROR("Could not initialize hobbes client\n");
	return -1;
    }

    while (cmds[i].name) {

	if (strncmp(cmds[i].name, argv[1], strlen(cmds[i].name)) == 0) {
	    return cmds[i].handler(argc - 1, &argv[1]);
	}

	i++;
    }
    
    hobbes_client_deinit();


    return 0;
}
