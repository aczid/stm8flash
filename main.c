/* stlink/v2 stm8 memory programming utility
   (c) Valentin Dudouyt, 2012 - 2014 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <stdbool.h>
#include <assert.h>
#include "pgm.h"
#include "stlink.h"
#include "stlinkv2.h"
#include "stm8.h"
#include "ihex.h"

#ifdef __APPLE__
extern char *optarg;
extern int optind;
extern int optopt;
extern int opterr;
extern int optreset;
#endif

programmer_t pgms[] = {
	{ 	"stlink",
		0x0483, // USB vid
		0x3744, // USB pid
		stlink_open,
		stlink_close,
		stlink_swim_srst,
		stlink_swim_read_range,
		stlink_swim_write_range,
	},
	{ 
		"stlinkv2", 
		0x0483,
		0x3748,
		stlink2_open,
		stlink_close,
		stlink2_srst,
		stlink2_swim_read_range,
		stlink2_swim_write_range,
	},
	{ NULL },
};

void print_help_and_exit(const char *name) {
	fprintf(stderr, "Usage: %s [-c programmer] [-p partno] [-s memtype] [-b bytes] [-r|-w|-v] <filename>\n", name);
	exit(-1);
}

void spawn_error(const char *msg) {
	fprintf(stderr, "%s\n", msg);
	exit(-1);
}

void dump_pgms(programmer_t *pgms) {
	// Dump programmers list in stderr
	int i;
	for(i = 0; pgms[i].name; i++)
		fprintf(stderr, "%s\n", pgms[i].name);
}

void dump_devices(stm8_device_t *devices) {
	// Dump parts list in stderr
	int i;
	for(i = 0; devices[i].name; i++)
		fprintf(stderr, "%s\n", devices[i].name);
}

bool is_ext(const char *filename, const char *ext) {
	char *ext_begin = strrchr(filename, '.');
	return(ext_begin && strcmp(ext_begin, ext) == 0);
}

bool usb_init(programmer_t *pgm, unsigned int vid, unsigned int pid) {
	libusb_device **devs;
	libusb_context *ctx = NULL;

	int r;
	ssize_t cnt;
	r = libusb_init(&ctx);
	if(r < 0) return(false);

	libusb_set_debug(ctx, 3);
	cnt = libusb_get_device_list(ctx, &devs);
	if(cnt < 0) return(false);

	pgm->dev_handle = libusb_open_device_with_vid_pid(ctx, vid, pid);
	pgm->ctx = ctx;
	assert(pgm->dev_handle);

	libusb_free_device_list(devs, 1); //free the list, unref the devices in it

	if(libusb_kernel_driver_active(pgm->dev_handle, 0) == 1) { //find out if kernel driver is attached
		int r = libusb_detach_kernel_driver(pgm->dev_handle, 0);
		assert(r == 0);
	}

#ifdef __APPLE__
	r = libusb_claim_interface(pgm->dev_handle, 0);
	assert(r == 0);
#endif

	return(true);
}

int main(int argc, char **argv) {
	int start, bytes_count = 0;
	char filename[256];
	memset(filename, 0, sizeof(filename));
	// Parsing command line
	char c;
	action_t action = NONE;
	bool start_addr_specified = false,
		pgm_specified = false,
		part_specified = false,
        bytes_count_specified = false;
	memtype_t memtype = FLASH;
	int i;
	programmer_t *pgm = NULL;
	stm8_device_t *part = NULL;
	while((c = getopt (argc, argv, "r:w:v:nc:p:s:b:")) != -1) {
		switch(c) {
			case 'c':
				pgm_specified = true;
				for(i = 0; pgms[i].name; i++) {
					if(!strcmp(optarg, pgms[i].name))
						pgm = &pgms[i];
				}
				break;
			case 'p':
				part_specified = true;
				for(i = 0; stm8_devices[i].name; i++) {
					if(!strcmp(optarg, stm8_devices[i].name))
						part = &stm8_devices[i];
				}
				break;
			case 'r':
				action = READ;
				strcpy(filename, optarg);
				break;
			case 'w':
				action = WRITE;
				strcpy(filename, optarg);
				break;
			case 'v':
				action = VERIFY;
				strcpy(filename, optarg);
				break;
			case 's':
                // Start addr is depending on MCU type
				if(strcasecmp(optarg, "flash") == 0) {
					memtype = FLASH;
                } else if(strcasecmp(optarg, "eeprom") == 0) {
					memtype = EEPROM;
                } else if(strcasecmp(optarg, "ram") == 0) {
					memtype = RAM;
                } else if(strcasecmp(optarg, "opt") == 0) {
					memtype = OPT;
				} else {
					// Start addr is specified explicitely
					memtype = UNKNOWN;
					int success = sscanf(optarg, "%x", &start);
					assert(success);
                    start_addr_specified = true;
				}
				break;
			case 'b':
				bytes_count = atoi(optarg);
                bytes_count_specified = true;
				break;
			default:
				print_help_and_exit(argv[0]);
		}
	}
	if(argc <= 1)
		print_help_and_exit(argv[0]);
	if(pgm_specified && !pgm) {
		fprintf(stderr, "No valid programmer specified. Possible values are:\n");
		dump_pgms( (programmer_t *) &pgms);
		exit(-1);
	}
	if(!pgm)
		spawn_error("No programmer has been specified");
	if(part_specified && !part) {
		fprintf(stderr, "No valid part specified. Possible values are:\n");
		dump_devices( (stm8_device_t *) &stm8_devices);
		exit(-1);
	}
	if(!part)
		spawn_error("No part has been specified");

    // Try define memory type by address
	if(memtype == UNKNOWN) {
        if((start >= 0x4800) && (start < 0x4880)) {
            memtype = OPT;
        }
        if((start >= part->ram_start) && (start < part->ram_start + part->ram_size)) {
            memtype = RAM;
        }
        else if((start >= part->flash_start) && (start < part->flash_start + part->flash_size)) {
            memtype = FLASH;
        }
        else if((start >= part->eeprom_start) && (start < part->eeprom_start + part->eeprom_size)) {
            memtype = EEPROM;
        }
    }

	if(memtype != UNKNOWN) {
		// Selecting start addr depending on 
		// specified part and memtype
		switch(memtype) {
			case RAM:
                if(!start_addr_specified) {
                    start = part->ram_start;
                }
                if(!bytes_count_specified || bytes_count > part->ram_size) {
                    bytes_count = part->ram_size;
                }
                fprintf(stderr, "Determine RAM area\r\n");
				break;
			case EEPROM:
                if(!start_addr_specified) {
                    start = part->eeprom_start;
                }
                if(!bytes_count_specified || bytes_count > part->eeprom_size) {
                    bytes_count = part->eeprom_size;
                }
                fprintf(stderr, "Determine EEPROM area\r\n");
				break;
			case FLASH:
                if(!start_addr_specified) {
                    start = part->flash_start;
                }
                if(!bytes_count_specified || bytes_count > part->flash_size) {
                    bytes_count = part->flash_size;
                }
                fprintf(stderr, "Determine FLASH area\r\n");
				break;
			case OPT:
                if(!start_addr_specified) {
                    start = 0x4800;
                }
                size_t opt_size = (part->flash_size <= 8*1024 ? 0x40 : 0x80);
                if(!bytes_count_specified || bytes_count > opt_size) {
                    bytes_count = opt_size;
                }
                fprintf(stderr, "Determine OPT area\r\n");
                break;
		}
		start_addr_specified = true;
	}
	if(!action)
		spawn_error("No action has been specified");
	if(!start_addr_specified)
		spawn_error("No memtype or start_addr has been specified");
	if(!strlen(filename))
		spawn_error("No filename has been specified");
	if(!action || !start_addr_specified || !strlen(filename))
		print_help_and_exit(argv[0]);
	if(!usb_init(pgm, pgm->usb_vid, pgm->usb_pid))
		spawn_error("Couldn't initialize stlink");
	if(!pgm->open(pgm))
		spawn_error("Error communicating with MCU. Please check your SWIM connection.");
	FILE *f;
	if(action == READ) {
		fprintf(stderr, "Reading %d bytes at 0x%x... ", bytes_count, start);
		fflush(stderr);
        int bytes_count_align = ((bytes_count-1)/256+1)*256; // Reading should be done in blocks of 256 bytes
		unsigned char *buf = malloc(bytes_count_align);
		if(!buf) spawn_error("malloc failed");
		int recv = pgm->read_range(pgm, part, buf, start, bytes_count_align);
        if(recv < bytes_count_align) {
            fprintf(stderr, "\r\nRequested %d bytes but received only %d.\r\n", bytes_count_align, recv);
			spawn_error("Failed to read MCU");
        }
		if(!(f = fopen(filename, "w")))
			spawn_error("Failed to open file");
		fwrite(buf, 1, bytes_count, f);
		fclose(f);
		fprintf(stderr, "OK\n");
		fprintf(stderr, "Bytes received: %d\n", bytes_count);
    } else if (action == VERIFY) {
		fprintf(stderr, "Verifing %d bytes at 0x%x... ", bytes_count, start);
		fflush(stderr);

        int bytes_count_align = ((bytes_count-1)/256+1)*256; // Reading should be done in blocks of 256 bytes
		unsigned char *buf = malloc(bytes_count_align);
		if(!buf) spawn_error("malloc failed");
		int recv = pgm->read_range(pgm, part, buf, start, bytes_count_align);
        if(recv < bytes_count_align) {
            fprintf(stderr, "\r\nRequested %d bytes but received only %d.\r\n", bytes_count_align, recv);
			spawn_error("Failed to read MCU");
        }

		if(!(f = fopen(filename, "r")))
			spawn_error("Failed to open file");
		unsigned char *buf2 = malloc(bytes_count);
		if(!buf2) spawn_error("malloc failed");
		int bytes_to_verify;
		/* reading bytes to RAM */
		if(is_ext(filename, ".ihx")) {
			bytes_to_verify = ihex_read(f, buf, start, start + bytes_count);
		} else {
			fseek(f, 0L, SEEK_END);
			bytes_to_verify = ftell(f);
            if(bytes_count_specified) {
                bytes_to_verify = bytes_count; 
            } else if(bytes_count < bytes_to_verify) {
                bytes_to_verify = bytes_count; 
            }
			fseek(f, 0, SEEK_SET);
			fread(buf2, 1, bytes_to_verify, f);
		}
		fclose(f);

        if(memcmp(buf, buf2, bytes_to_verify) == 0) {
            fprintf(stderr, "OK\n");
            fprintf(stderr, "Bytes verified: %d\n", bytes_to_verify);
        } else {
            fprintf(stderr, "FAILED\n");
            exit(-1);
        }
	} else if (action == WRITE) {
		if(!(f = fopen(filename, "r")))
			spawn_error("Failed to open file");
        int bytes_count_align = ((bytes_count-1)/part->flash_block_size+1)*part->flash_block_size;
		unsigned char *buf = malloc(bytes_count_align);
		if(!buf) spawn_error("malloc failed");
        memset(buf, 0, bytes_count_align); // Clean aligned buffer
		int bytes_to_write;

		/* reading bytes to RAM */
		if(is_ext(filename, ".ihx")) {
			fprintf(stderr, "Writing Intel hex file ");
			bytes_to_write = ihex_read(f, buf, start, start + bytes_count);
		} else {
			fprintf(stderr, "Writing binary file ");
			fseek(f, 0L, SEEK_END);
			bytes_to_write = ftell(f);
            if(bytes_count_specified) {
                bytes_to_write = bytes_count; 
            } else if(bytes_count < bytes_to_write) {
                bytes_to_write = bytes_count; 
            }
			fseek(f, 0, SEEK_SET);
			fread(buf, 1, bytes_to_write, f);
		}
		fprintf(stderr, "%d bytes at 0x%x... ", bytes_to_write, start);

		/* flashing MCU */
		int sent = pgm->write_range(pgm, part, buf, start, bytes_to_write, memtype);
		if(pgm->reset) {
			// Restarting core (if applicable)
			pgm->reset(pgm);
		}
		fprintf(stderr, "OK\n");
		fprintf(stderr, "Bytes written: %d\n", sent);
		fclose(f);
	}
	return(0);
}
