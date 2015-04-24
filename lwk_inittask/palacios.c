#include <stdint.h>

#include <hobbes.h>
#include <hobbes_cmd_queue.h>
#include <hobbes_db.h>

#include <v3vee.h>
#include <pet_log.h>
#include <pet_xml.h>

#include "palacios.h"
#include "hobbes_ctrl.h"


extern hdb_db_t hobbes_master_db;

static int
__hobbes_launch_vm(hcq_handle_t hcq,
		   uint64_t     cmd)
{
    hobbes_id_t enclave_id    = -1;

    pet_xml_t   xml           =  NULL;
    char      * xml_str       =  NULL;
    uint32_t    data_size     =  0;

    char      * enclave_name  =  NULL;
    int         vm_id         = -1;

    int         ret           = -1;


    xml_str = hcq_get_cmd_data(hcq, cmd, &data_size);

    if (xml_str == NULL) {
	ERROR("Could not read VM spec\n");
	goto out;
    }

    /* ensure null termination */
    xml_str[data_size] = '\0';

    xml = pet_xml_parse_str(xml_str);

    /* Add VM to the Master DB */
    {

	enclave_name = pet_xml_get_val(xml, "name");

	enclave_id = hdb_create_enclave(hobbes_master_db, 
					enclave_name, 
					vm_id, 
					PISCES_VM_ENCLAVE, 
					0);

	if (enclave_id == -1) {
            ERROR("Could not create enclave in database\n");
	    goto out;
	}

	enclave_name = hdb_get_enclave_name(hobbes_master_db, enclave_id);

    }

    /* Load VM Image */
    {
	u8 * img_data = NULL;
	u32  img_size = 0;

	img_data = v3_build_vm_image(xml, &img_size);

	if (img_data) {
	    vm_id = v3_create_vm(enclave_name, img_data, img_size);
	    
	    if (vm_id == -1) {
		ERROR("Could not create VM (%s)\n", enclave_name);
	    }
	} else {
	    ERROR("Could not build VM image from xml\n");
	}


	/* Cleanup if there was an error */
       	if ((img_data == NULL) || 
	    (vm_id    == -1)) {

	    printf("Creation error\n");

	    hdb_delete_enclave(hobbes_master_db, enclave_id);
	    
	    goto out;
	}


	hdb_set_enclave_dev_id(hobbes_master_db, enclave_id, vm_id);
    }

    
    /* Launch VM */
    {
	ret = v3_launch_vm(vm_id);

	if (ret != 0) {
	    ERROR("Could not launch VM enclave (%d)\n", vm_id);
	    ERROR("ERROR ERROR ERROR: We really need to implement this: v3_free(vm_id);\n");

	    hdb_set_enclave_state(hobbes_master_db, enclave_id, ENCLAVE_CRASHED);
	    
	    goto out;
	}

	hdb_set_enclave_state(hobbes_master_db, enclave_id, ENCLAVE_RUNNING);
    }

out:
    hcq_cmd_return(hcq, cmd, ret, 0, NULL);
    return 0;
}

int
palacios_init(void)
{

    // Register Hobbes commands
    if (hobbes_enabled) {
	register_hobbes_cmd(HOBBES_CMD_VM_LAUNCH, __hobbes_launch_vm);

	
    }

    // Register Pisces commands

    return 0;
}

bool 
palacios_is_available(void)
{
    return (v3_is_vmm_present() != 0);
}



/* Legacy Pisces Interfaces */
#if 0

	    case ENCLAVE_CMD_CREATE_VM: {
		struct pisces_user_file_info * file_info = NULL;
		struct cmd_create_vm vm_cmd;
		struct pmem_region rgn;

		id_t    my_aspace_id;
		vaddr_t file_addr;
		size_t  file_size =  0;
		int     path_len  =  0;
		int     vm_id     = -1;
		int     status    =  0;


		memset(&vm_cmd,    0, sizeof(struct cmd_create_vm));
		memset(&rgn,       0, sizeof(struct pmem_region));
			  
			    
		ret = read(pisces_fd, &vm_cmd, sizeof(struct cmd_create_vm));

		if (ret != sizeof(struct cmd_create_vm)) {
		    send_resp(pisces_fd, -1);
		    printf("Error: CREATE_VM command could not be read\n");
		    break;
		}


		path_len = strlen((char *)vm_cmd.path.file_name) + 1;

		file_info = malloc(sizeof(struct pisces_user_file_info) + path_len);
		memset(file_info, 0, sizeof(struct pisces_user_file_info) + path_len);

		file_info->path_len = path_len;
		strncpy(file_info->path, (char *)vm_cmd.path.file_name, path_len - 1);
			    
		file_size = ioctl(pisces_fd, PISCES_STAT_FILE, file_info);

		
		status = aspace_get_myid(&my_aspace_id);
		if (status != 0) 
		    return status;

		if (pmem_alloc_umem(file_size, PAGE_SIZE, &rgn)) {
		    printf("Error: Could not allocate umem for guest image (size=%lu)\n", file_size);
		    break;
		}
		pmem_zero(&rgn);
				
		status =
		    aspace_map_region_anywhere(
					       my_aspace_id,
					       &file_addr,
					       round_up(file_size, PAGE_SIZE),
					       (VM_USER|VM_READ|VM_WRITE),
					       PAGE_SIZE,
					       "VM Image",
					       rgn.start
					       );


		file_info->user_addr = file_addr;
		
		ioctl(pisces_fd, PISCES_LOAD_FILE, file_info);
				
				
		/* Issue VM Create command to Palacios */
		vm_id = v3_create_vm(vm_cmd.path.vm_name, (u8 *)file_info->user_addr, file_size);
				
		aspace_unmap_region(my_aspace_id, file_addr, round_up(file_size, PAGE_SIZE));
		pmem_free_umem(&rgn);
		

		if (vm_id < 0) {
		    printf("Error: Could not create VM (%s) at (%s) (err=%d)\n", 
			   vm_cmd.path.vm_name, vm_cmd.path.file_name, vm_id);
		    send_resp(pisces_fd, vm_id);
		    break;
		}

		printf("Created VM (%d)\n", vm_id);

		send_resp(pisces_fd, vm_id);
		break;
	    }
	    case ENCLAVE_CMD_FREE_VM: {
		struct cmd_vm_ctrl vm_cmd;

		ret = read(pisces_fd, &vm_cmd, sizeof(struct cmd_vm_ctrl));

		if (ret != sizeof(struct cmd_vm_ctrl)) {
		    send_resp(pisces_fd, -1);
		    break;
		}

		if (v3_free_vm(vm_cmd.vm_id) == -1) {
		    send_resp(pisces_fd, -1);
		    break;
		}


		send_resp(pisces_fd, 0);

		break;
	    }


   case ENCLAVE_CMD_FREE_V3_PCI: {
		struct cmd_add_pci_dev cmd;
		//		    struct v3_hw_pci_dev   v3_pci_spec;
		int ret = 0;

		memset(&cmd, 0, sizeof(struct cmd_add_pci_dev));

		printf("Removing V3 PCI Device\n");

		ret = read(pisces_fd, &cmd, sizeof(struct cmd_add_pci_dev));

		if (ret != sizeof(struct cmd_add_pci_dev)) {
		    send_resp(pisces_fd, -1);
		    break;
		}

		/*
		  memcpy(v3_pci_spec.name, cmd.spec.name, 128);
		  v3_pci_spec.bus  = cmd.spec.bus;
		  v3_pci_spec.dev  = cmd.spec.dev;
		  v3_pci_spec.func = cmd.spec.func;
		*/

		/* Issue Device Add operation to Palacios */
		/*
		  if (issue_v3_cmd(V3_REMOVE_PCI, (uintptr_t)&(v3_pci_spec)) == -1) {
		  printf("Error: Could not remove PCI device from Palacios\n");
		  send_resp(pisces_fd, -1);
		  break;
		  }
		*/
		send_resp(pisces_fd, 0);
		break;
	    }
	    case ENCLAVE_CMD_LAUNCH_VM: {
		struct cmd_vm_ctrl vm_cmd;

		ret = read(pisces_fd, &vm_cmd, sizeof(struct cmd_vm_ctrl));

		if (ret != sizeof(struct cmd_vm_ctrl)) {
		    send_resp(pisces_fd, -1);
		    break;
		}

		/* Signal Palacios to Launch VM */
		if (v3_launch_vm(vm_cmd.vm_id) == -1) {
		    send_resp(pisces_fd, -1);
		    break;
		}


		/*
		  if (xpmem_pisces_add_dom(palacios_fd, vm_cmd.vm_id)) {
		  printf("ERROR: Could not add connect to Palacios VM %d XPMEM channel\n", 
		  vm_cmd.vm_id);
		  }
		*/

		send_resp(pisces_fd, 0);

		break;
	    }
	    case ENCLAVE_CMD_STOP_VM: {
		struct cmd_vm_ctrl vm_cmd;

		ret = read(pisces_fd, &vm_cmd, sizeof(struct cmd_vm_ctrl));

		if (ret != sizeof(struct cmd_vm_ctrl)) {
		    send_resp(pisces_fd, -1);
		    break;
		}

		if (v3_stop_vm(vm_cmd.vm_id) == -1) {
		    send_resp(pisces_fd, -1);
		    break;
		}

		send_resp(pisces_fd, 0);

		break;
	    }

	    case ENCLAVE_CMD_PAUSE_VM: {
		struct cmd_vm_ctrl vm_cmd;

		ret = read(pisces_fd, &vm_cmd, sizeof(struct cmd_vm_ctrl));

		if (ret != sizeof(struct cmd_vm_ctrl)) {
		    send_resp(pisces_fd, -1);
		    break;
		}

		if (v3_pause_vm(vm_cmd.vm_id) == -1) {
		    send_resp(pisces_fd, -1);
		    break;
		}

		send_resp(pisces_fd, 0);

		break;
	    }
	    case ENCLAVE_CMD_CONTINUE_VM: {
		struct cmd_vm_ctrl vm_cmd;

		ret = read(pisces_fd, &vm_cmd, sizeof(struct cmd_vm_ctrl));

		if (ret != sizeof(struct cmd_vm_ctrl)) {
		    send_resp(pisces_fd, -1);
		    break;
		}

		if (v3_continue_vm(vm_cmd.vm_id) == -1) {
		    send_resp(pisces_fd, -1);
		    break;
		}

		send_resp(pisces_fd, 0);

		break;
	    }
	    case ENCLAVE_CMD_VM_CONS_CONNECT: {
		struct cmd_vm_ctrl vm_cmd;
		u64 cons_ring_buf = 0;

		ret = read(pisces_fd, &vm_cmd, sizeof(struct cmd_vm_ctrl));

		if (ret != sizeof(struct cmd_vm_ctrl)) {
		    printf("Error reading console command\n");

		    send_resp(pisces_fd, -1);
		    break;
		}

		/* Signal Palacios to connect the console */
		/*
		  if (issue_vm_cmd(vm_cmd.vm_id, V3_VM_CONSOLE_CONNECT, (uintptr_t)&cons_ring_buf) == -1) {
		  cons_ring_buf        = 0;
		  }
		*/	

		printf("Cons Ring Buf=%p\n", (void *)cons_ring_buf);
		send_resp(pisces_fd, cons_ring_buf);

		break;
	    }

	    case ENCLAVE_CMD_VM_CONS_DISCONNECT: {
		struct cmd_vm_ctrl vm_cmd;

		ret = read(pisces_fd, &vm_cmd, sizeof(struct cmd_vm_ctrl));

		if (ret != sizeof(struct cmd_vm_ctrl)) {
		    send_resp(pisces_fd, -1);
		    break;
		}


		/* Send Disconnect Request to Palacios */
		/*
		  if (issue_vm_cmd(vm_cmd.vm_id, V3_VM_CONSOLE_DISCONNECT, (uintptr_t)NULL) == -1) {
		  send_resp(pisces_fd, -1);
		  break;
		  }
		*/

		send_resp(pisces_fd, 0);
		break;
	    }

	    case ENCLAVE_CMD_VM_CONS_KEYCODE: {
		struct cmd_vm_cons_keycode vm_cmd;

		ret = read(pisces_fd, &vm_cmd, sizeof(struct cmd_vm_cons_keycode));

		if (ret != sizeof(struct cmd_vm_cons_keycode)) {
		    send_resp(pisces_fd, -1);
		    break;
		}

		/* Send Keycode to Palacios */
		/*
		  if (issue_vm_cmd(vm_cmd.vm_id, V3_VM_KEYBOARD_EVENT, vm_cmd.scan_code) == -1) {
		  send_resp(pisces_fd, -1);
		  break;
		  }
		*/
		send_resp(pisces_fd, 0);
		break;
	    }

	    case ENCLAVE_CMD_VM_DBG: {
		struct cmd_vm_debug pisces_cmd;
			    
		ret = read(pisces_fd, &pisces_cmd, sizeof(struct cmd_vm_debug));
			    
		if (ret != sizeof(struct cmd_vm_debug)) {
		    send_resp(pisces_fd, -1);
		    break;
		}
			    
			    
		if (v3_debug_vm(pisces_cmd.spec.vm_id, 
				pisces_cmd.spec.core, 
				pisces_cmd.spec.cmd) == -1) {
		    send_resp(pisces_fd, -1);
		    break;
		}
			    
		send_resp(pisces_fd, 0);
		break;
	    }




#endif
