/*
Copyright (C) 2015- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/
#include "debug.h"

#ifdef CCTOOLS_OPSYS_LINUX

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>

#include "disk_alloc.h"
#include "stringtools.h"
#include "path.h"

int disk_alloc_create(char *loc, char *fs, int64_t size) {

	if(size <= 0) {
		debug(D_NOTICE, "Mountpoint pathname argument nonexistant.\n");
		return 1;
	}

	//Check for trailing '/'
	path_remove_trailing_slashes(loc);
	int result;
	char *device_loc = NULL;
	char *dd_args[] = {"/bin/dd", "if=/dev/zero", "of", "bs=1024", "count", NULL};
	char *losetup_args[] = {"/sbin/losetup", "dev_num", "location", NULL};
	char *losetup_rm_args[] = {"/sbin/losetup", "-d", "loc", NULL};
	char *mkfs_args[] = {"/sbin/mkfs", "dev_num", "-t=", NULL};
	char *mount_args = NULL;

	//Set Loopback Device Location
	device_loc = string_format("%s/alloc.img", loc);
	//Make Directory for Loop Device
	if(mkdir(loc, 0777) != 0) {
		debug(D_NOTICE, "Failed to make directory at requested mountpoint: %s.\n", strerror(errno));
		goto error;
	}

	//Create Image
	char *dd_arg = string_format("of=%s", device_loc);
	dd_args[2] = dd_arg;
	dd_arg = string_format("count=%"PRId64"", size);
	dd_args[4] = dd_arg;

	pid_t pid = fork();
	if(pid == 0) {
		execv(dd_args[0], &dd_args[0]);
	}
	else if(pid > 0) {
		int status;
		waitpid(pid, &status, 0);
	}
	else {
		debug(D_NOTICE, "Failed to instantiate forked process for allocating junk space.\n");
	}

	int img_fd = open(device_loc, O_RDONLY);
	if(img_fd == -1) {
		debug(D_NOTICE, "Failed to allocate junk space for loop device image: %s.\n", strerror(errno));
		if(unlink(device_loc) == -1) {
			debug(D_NOTICE, "Failed to unlink loop device image while attempting to clean up after failure: %s.\n", strerror(errno));
			goto error;
		}
		if(rmdir(loc) == -1) {
			debug(D_NOTICE, "Failed to remove directory of loop device image while attempting to clean up after failure: %s.\n", strerror(errno));
		}
		goto error;
	}
	close(img_fd);

	char *loop_dev_num = NULL;
	//Attach Image to Loop Device
	int j, losetup_flag = 0;
	for(j = 0; ; j++) {

		if(j >= 256) {
			losetup_flag = 1;
			break;
		}

		//Binds the first available loop device to the specified mount point from input
		loop_dev_num = string_format("/dev/loop%d", j);
		losetup_args[1] = loop_dev_num;
		losetup_args[2] = device_loc;
		
		//Makes the specified filesystem from input at the first available loop device
		mkfs_args[1] = loop_dev_num;
		char *mkfs_arg = string_format("-t%s", fs);
		mkfs_args[2] = mkfs_arg;
		
		//Mounts the first available loop device
		mount_args = string_format("/dev/loop%d", j);
	
		pid = fork();
		if(pid == 0) {
			execv(losetup_args[0], &losetup_args[0]);
		}
		else if(pid > 0) {
			int status;
			waitpid(pid, &status, 0);
			if(WIFEXITED(status) && WEXITSTATUS(status) == 0) {
				break;
			}
		}
		else {
			debug(D_NOTICE, "Failed to instantiate forked process for attaching image to loop device.\n");
		}
	}

	if(losetup_flag == 1) {
		debug(D_NOTICE, "Failed to attach image to loop device: %s.\n", strerror(errno));
		if(unlink(device_loc) == -1) {
			debug(D_NOTICE, "Failed to unlink loop device image while attempting to clean up after failure: %s.\n", strerror(errno));
			goto error;
		}
		if(rmdir(loc) == -1) {
			debug(D_NOTICE, "Failed to remove directory of loop device image while attempting to clean up after failure: %s.\n", strerror(errno));
		}
		goto error;
	}

	//Create Filesystem
	pid = fork();
	if(pid == 0) {
		execv(mkfs_args[0], &mkfs_args[0]);
	}
	else if(pid > 0) {
		int status;
		waitpid(pid, &status, 0);
		if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
			debug(D_NOTICE, "Failed to initialize filesystem on loop device: %s.\n", strerror(errno));
			char *losetup_rm_arg = string_format("/dev/loop%d", j);
			losetup_rm_args[2] = losetup_rm_arg;
			pid_t pid2 = fork();
			if(pid2 == 0) {
				execv(losetup_rm_args[0], &losetup_rm_args[0]);
			}
			else if(pid2 > 0) {
				int status;
				waitpid(pid, &status, 0);
				if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
					debug(D_NOTICE, "Failed to detach loop device and remove its contents while attempting to clean up after failure: %s.\n", strerror(errno));
					rmdir(loc);
					goto error;
				}
			}
			else {
				debug(D_NOTICE, "Failed to instantiate forked process for detaching loop device and removing its contents.\n");
			}
		}
	}
	else {
		debug(D_NOTICE, "Failed to instantiate forked process for initializing filesystem on loop device.\n");
	}

	//Mount Loop Device
	result = mount(loop_dev_num, loc, fs, 0, "");
	if(result != 0) {

		debug(D_NOTICE, "Failed to mount loop device: %s.\n", strerror(errno));
		pid_t pid2 = fork();
		if(pid2 == 0) {
			execv(losetup_rm_args[0], &losetup_rm_args[0]);
		}
		else if(pid2 > 0) {
			int status;
			waitpid(pid, &status, 0);
			if(!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
				debug(D_NOTICE, "Failed to detach loop device and remove its contents while attempting to clean up after failure: %s.\n", strerror(errno));
				rmdir(loc);
				goto error;
			}
		}
		else {
			debug(D_NOTICE, "Failed to instantiate forked process for detaching loop device and removing its contents.\n");
		}
	}

	free(device_loc);
	free(loop_dev_num);
	return 0;

	error:
		return 1;
}

int disk_alloc_delete(char *loc) {

	int result;
	char *losetup_args = NULL;
	char *rm_args = NULL;
	char *device_loc = NULL;
	char *losetup_rm_args[] = {"/sbin/losetup", "-d", "loc", NULL};

	//Check for trailing '/'
	path_remove_trailing_slashes(loc);
	//Check if location is relative or absolute
	result = strncmp(loc, "/", 1);
	if(result != 0) {
		char *pwd = get_current_dir_name();
		path_remove_trailing_slashes(pwd);
		device_loc = string_format("%s/%s/alloc.img", pwd, loc);
		free(pwd);
	}
	else {
		device_loc = string_format("%s/alloc.img", loc);
	}

	//Find Used Device
	char *dev_num = "-1";

	//Loop Device Unmounted
	result = umount2(loc, MNT_FORCE);
	if(result != 0) {
		if(errno != ENOENT) {
			debug(D_NOTICE, "Failed to unmount loop device: %s.\n", strerror(errno));
			goto error;
		}
	}

	//Find pathname of mountpoint associated with loop device
	char loop_dev[128], loop_info[128], loop_mount[128];
	FILE *loop_find;
	losetup_args = string_format("losetup -j %s", device_loc);
	loop_find = popen(losetup_args, "r");
	fscanf(loop_find, "%s %s %s", loop_dev, loop_info, loop_mount);
	pclose(loop_find);
	int loop_dev_path_length = strlen(loop_mount);
	loop_mount[loop_dev_path_length - 1] = '\0';
	loop_dev[strlen(loop_dev) - 1] = '\0';
	char loop_mountpoint_array[128];
	int k;
	int max_mount_path_length = 62;

	//Copy only pathname of the mountpoint without extraneous paretheses
	for(k = 1; k < loop_dev_path_length; k++) {
		loop_mountpoint_array[k-1] = loop_mount[k];
	}
	loop_mountpoint_array[k] = '\0';

	if(strncmp(loop_mountpoint_array, device_loc, max_mount_path_length) == 0) {

		dev_num = loop_dev;
	}

	//Device Not Found
	if(strcmp(dev_num, "-1") == 0) {
		debug(D_NOTICE, "Failed to locate loop device associated with given mountpoint: %s.\n", strerror(errno));
		goto error;
	}

	rm_args = string_format("%s/alloc.img", loc);

	//Loop Device Deleted
	losetup_rm_args[2] = dev_num;
	pid_t pid = fork();
	if(pid == 0) {
		execv(losetup_rm_args[0], &losetup_rm_args[0]);
	}
	else if(pid > 0) {
		int status;
		waitpid(pid, &status, 0);
		if((!WIFEXITED(status) || WEXITSTATUS(status) != 0) && errno != ENOENT) {
			debug(D_NOTICE, "Failed to detach loop device and remove its contents: %s.\n", strerror(errno));
			rmdir(loc);
			goto error;
		}
	}
	else {
		debug(D_NOTICE, "Failed to instantiate forked process for detaching loop device and removing its contents.\n");
	}

	//Image Deleted
	result = unlink(rm_args);
	if(result != 0) {
		debug(D_NOTICE, "Failed to delete image file associated with given mountpoint: %s.\n", strerror(errno));
		goto error;
	}

	//Directory Deleted
	result = rmdir(loc);
	if(result != 0) {
		debug(D_NOTICE, "Failed to delete directory associated with given mountpoint: %s.\n", strerror(errno));
		goto error;
	}

	free(losetup_args);
	free(rm_args);
	free(device_loc);

	return 0;

	error:
		if(losetup_args) {
			free(losetup_args);
		}
		if(rm_args) {
			free(rm_args);
		}
		if(device_loc) {
			free(device_loc);
		}

		return 1;
}

#else
int disk_alloc_create(char *loc, int64_t size) {

	debug(D_NOTICE, "Platform not supported by this library.\n");
	return 1;
}

int disk_alloc_delete(char *loc) {

	debug(D_NOTICE, "Platform not supported by this library.\n");
	return 1;
}
#endif
