/*
 * Copyright (C) 2008 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <sys/mman.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <linux/kd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <linux/if.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <linux/loop.h>
#include <cutils/partition_utils.h>

#include "init.h"
#include "keywords.h"
#include "property_service.h"
#include "devices.h"
#include "init_parser.h"
#include "util.h"
#include "log.h"

#include <private/android_filesystem_config.h>
extern struct mtd_part_t mtd_part_map[];

struct ubi_attach_req {
	int32_t ubi_num;
	int32_t mtd_num;
	int32_t vid_hdr_offset;
	uint8_t padding[12];
};
#define UBI_CTRL_IOC_MAGIC 'o'
#define UBI_IOCATT _IOW(UBI_CTRL_IOC_MAGIC, 64, struct ubi_attach_req)

void add_environment(const char *name, const char *value);

extern int init_module(void *, unsigned long, const char *);

static int write_file(const char *path, const char *value)
{
    int fd, ret, len;

    fd = open(path, O_WRONLY|O_CREAT, 0622);

    if (fd < 0)
        return -errno;

    len = strlen(value);

    do {
        ret = write(fd, value, len);
    } while (ret < 0 && errno == EINTR);

    close(fd);
    if (ret < 0) {
        return -errno;
    } else {
        return 0;
    }
}

static int insmod(const char *filename, char *options)
{
    int fd;
    off_t len;
    void *module;
    unsigned size;
    int ret;

    fd = open(filename, O_RDONLY);
    if (fd == -1) return -1;
    len = lseek(fd, 0, SEEK_END);
    if (len == -1)
    {
        close(fd);
        return -1;
    }
    size = (len + 4095) & ~4095;
    module = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (module == MAP_FAILED) return -1;

    ret = init_module(module, len, options);

    munmap(module, size);

    return ret;
}

static int setkey(struct kbentry *kbe)
{
    int fd, ret;

    fd = open("/dev/tty0", O_RDWR | O_SYNC);
    if (fd < 0)
        return -1;

    ret = ioctl(fd, KDSKBENT, kbe);

    close(fd);
    return ret;
}

static int __ifupdown(const char *interface, int up)
{
    struct ifreq ifr;
    int s, ret;

    strlcpy(ifr.ifr_name, interface, IFNAMSIZ);

    s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0)
        return -1;

    ret = ioctl(s, SIOCGIFFLAGS, &ifr);
    if (ret < 0) {
        goto done;
    }

    if (up)
        ifr.ifr_flags |= IFF_UP;
    else
        ifr.ifr_flags &= ~IFF_UP;

    ret = ioctl(s, SIOCSIFFLAGS, &ifr);
    
done:
    close(s);
    return ret;
}

static void service_start_if_not_disabled(struct service *svc)
{
    if (!(svc->flags & SVC_DISABLED)) {
        service_start(svc, NULL);
    }
}

int do_chdir(int nargs, char **args)
{
    chdir(args[1]);
    return 0;
}

int do_chroot(int nargs, char **args)
{
    chroot(args[1]);
    return 0;
}

int do_class_start(int nargs, char **args)
{
        /* Starting a class does not start services
         * which are explicitly disabled.  They must
         * be started individually.
         */
    service_for_each_class(args[1], service_start_if_not_disabled);
    return 0;
}

int do_class_stop(int nargs, char **args)
{
    service_for_each_class(args[1], service_stop);
    return 0;
}

int do_class_reset(int nargs, char **args)
{
    service_for_each_class(args[1], service_reset);
    return 0;
}

int do_domainname(int nargs, char **args)
{
    return write_file("/proc/sys/kernel/domainname", args[1]);
}

int do_exec(int nargs, char **args)
{
    return -1;
}

int do_export(int nargs, char **args)
{
    add_environment(args[1], args[2]);
    return 0;
}

int do_hostname(int nargs, char **args)
{
    return write_file("/proc/sys/kernel/hostname", args[1]);
}

int do_ifup(int nargs, char **args)
{
    return __ifupdown(args[1], 1);
}


static int do_insmod_inner(int nargs, char **args, int opt_len)
{
    char options[opt_len + 1];
    int i;

    options[0] = '\0';
    if (nargs > 2) {
        strcpy(options, args[2]);
        for (i = 3; i < nargs; ++i) {
            strcat(options, " ");
            strcat(options, args[i]);
        }
    }

    return insmod(args[1], options);
}

int do_insmod(int nargs, char **args)
{
    int i;
    int size = 0;

    if (nargs > 2) {
        for (i = 2; i < nargs; ++i)
            size += strlen(args[i]) + 1;
    }

    return do_insmod_inner(nargs, args, size);
}

struct async_arg
{
    int fd;
    int (*routine)(int nargs, char **args);
    int nargs;
    char **args;
    char buf[0];
};

void *do_async(void *_arg)
{
    struct async_arg *arg = (struct async_arg *)_arg;
    int ret = (arg->routine)(arg->nargs, arg->args);
    close(arg->fd);
    free(_arg);
    return (void *)ret;
}

int async_helper(int (*routine)(int nargs, char **args), int nargs, char **args)
{
    struct async_arg *arg;
    pthread_t thread;
    pthread_attr_t attr;
    int fd, i, size;

    if (nargs < 2) return -1;

    /* create/lock 2nd arg file */
    fd = open(args[1], O_CREAT | O_RDWR, 0600);
    if (fd == -1) return -1;
    flock(fd, LOCK_EX);

    /* prepare args and skip the 2nd args here */
    size = sizeof(*arg) + (nargs - 1) * sizeof(char *);
    for (i = 0; i < nargs; i++)
    {
        if (i == 1) continue;
        size += strlen(args[i]) + 1;
    }
    arg = malloc(size);
    if (arg == NULL)
    {
        close(fd);
        return -1;
    }
    arg->fd = fd;
    arg->routine = routine;
    arg->nargs = nargs - 1;
    arg->args = (char **)arg->buf;
    size = (nargs - 1) * sizeof(char *);
    for (i = 0; i < nargs; i++)
    {
        if (i == 1) continue;
        arg->args[i < 2 ? i : i - 1] = arg->buf + size;
        strcpy(arg->buf + size, args[i]);
        size += strlen(args[i]) + 1;
    }

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, do_async, arg);
    pthread_attr_destroy(&attr);

    return 0;
}

int do_insmod_async(int nargs, char **args)
{
    return async_helper(do_insmod, nargs, args);
}

int do_mount_ubifs_async(int nargs, char **args)
{
    return async_helper(do_mount_ubifs, nargs, args);
}

int do_waitlock(int nargs, char **args)
{
    int fd = open(args[1], O_CREAT | O_RDWR, 0600);
    if (fd == -1) return -1;
    flock(fd, LOCK_EX);
    close(fd);
	remove(args[1]);

    return 0;
}

int do_mkdir(int nargs, char **args)
{
    mode_t mode = 0755;
    int ret;

    /* mkdir <path> [mode] [owner] [group] */

    if (nargs >= 3) {
        mode = strtoul(args[2], 0, 8);
    }

    ret = mkdir(args[1], mode);
    /* chmod in case the directory already exists */
    if (ret == -1 && errno == EEXIST) {
        ret = chmod(args[1], mode);
    }
    if (ret == -1) {
        return -errno;
    }

    if (nargs >= 4) {
        uid_t uid = decode_uid(args[3]);
        gid_t gid = -1;

        if (nargs == 5) {
            gid = decode_uid(args[4]);
        }

        if (chown(args[1], uid, gid)) {
            return -errno;
        }
    }

    return 0;
}

static struct {
    const char *name;
    unsigned flag;
} mount_flags[] = {
    { "noatime",    MS_NOATIME },
    { "nosuid",     MS_NOSUID },
    { "nodev",      MS_NODEV },
    { "nodiratime", MS_NODIRATIME },
    { "ro",         MS_RDONLY },
    { "rw",         0 },
    { "remount",    MS_REMOUNT },
    { "defaults",   0 },
    { 0,            0 },
};

#define DATA_MNT_POINT "/data"

/* mount <type> <device> <path> <flags ...> <options> */
int do_mount(int nargs, char **args)
{
    char tmp[64];
    char *source, *target, *system;
    char *options = NULL;
    unsigned flags = 0;
    int n, i;
    int wait = 0;

    for (n = 4; n < nargs; n++) {
        for (i = 0; mount_flags[i].name; i++) {
            if (!strcmp(args[n], mount_flags[i].name)) {
                flags |= mount_flags[i].flag;
                break;
            }
        }

        if (!mount_flags[i].name) {
            if (!strcmp(args[n], "wait"))
                wait = 1;
            /* if our last argument isn't a flag, wolf it up as an option string */
            else if (n + 1 == nargs)
                options = args[n];
        }
    }

    system = args[1];
    source = args[2];
    target = args[3];

    if (!strncmp(source, "mtd@", 4)) {
        n = mtd_name_to_number(source + 4);
        if (n < 0) {
            return -1;
        }

        sprintf(tmp, "/dev/block/mtdblock%d", n);

        if (wait)
            wait_for_file(tmp, COMMAND_RETRY_TIMEOUT);
        if (mount(tmp, target, system, flags, options) < 0) {
            return -1;
        }

        goto exit_success;
    } else if (!strncmp(source, "loop@", 5)) {
        int mode, loop, fd;
        struct loop_info info;

        mode = (flags & MS_RDONLY) ? O_RDONLY : O_RDWR;
        fd = open(source + 5, mode);
        if (fd < 0) {
            return -1;
        }

        for (n = 0; ; n++) {
            sprintf(tmp, "/dev/block/loop%d", n);
            loop = open(tmp, mode);
            if (loop < 0) {
                return -1;
            }

            /* if it is a blank loop device */
            if (ioctl(loop, LOOP_GET_STATUS, &info) < 0 && errno == ENXIO) {
                /* if it becomes our loop device */
                if (ioctl(loop, LOOP_SET_FD, fd) >= 0) {
                    close(fd);

                    if (mount(tmp, target, system, flags, options) < 0) {
                        ioctl(loop, LOOP_CLR_FD, 0);
                        close(loop);
                        return -1;
                    }

                    close(loop);
                    goto exit_success;
                }
            }

            close(loop);
        }

        close(fd);
        ERROR("out of loopback devices");
        return -1;
    } else {
        if (wait)
            wait_for_file(source, COMMAND_RETRY_TIMEOUT);
        if (mount(source, target, system, flags, options) < 0) {
            /* If this fails, it may be an encrypted filesystem
             * or it could just be wiped.  If wiped, that will be
             * handled later in the boot process.
             * We only support encrypting /data.  Check
             * if we're trying to mount it, and if so,
             * assume it's encrypted, mount a tmpfs instead.
             * Then save the orig mount parms in properties
             * for vold to query when it mounts the real
             * encrypted /data.
             */
            if (!strcmp(target, DATA_MNT_POINT) && !partition_wiped(source)) {
                const char *tmpfs_options;

                tmpfs_options = property_get("ro.crypto.tmpfs_options");

                if (mount("tmpfs", target, "tmpfs", MS_NOATIME | MS_NOSUID | MS_NODEV,
                    tmpfs_options) < 0) {
                    return -1;
                }

                /* Set the property that triggers the framework to do a minimal
                 * startup and ask the user for a password
                 */
                property_set("ro.crypto.state", "encrypted");
                property_set("vold.decrypt", "1");
            } else {
                return -1;
            }
        }

        if (!strcmp(target, DATA_MNT_POINT)) {
            char fs_flags[32];

            /* Save the original mount options */
            property_set("ro.crypto.fs_type", system);
            property_set("ro.crypto.fs_real_blkdev", source);
            property_set("ro.crypto.fs_mnt_point", target);
            if (options) {
                property_set("ro.crypto.fs_options", options);
            }
            snprintf(fs_flags, sizeof(fs_flags), "0x%8.8x", flags);
            property_set("ro.crypto.fs_flags", fs_flags);
        }
    }

exit_success:
    /* If not running encrypted, then set the property saying we are
     * unencrypted, and also trigger the action for a nonencrypted system.
     */
    if (!strcmp(target, DATA_MNT_POINT)) {
        const char *prop;

        prop = property_get("ro.crypto.state");
        if (! prop) {
            prop = "notset";
        }
        if (strcmp(prop, "encrypted")) {
            property_set("ro.crypto.state", "unencrypted");
            action_for_each_trigger("nonencrypted", action_add_queue_tail);
        }
    }

    return 0;

}

int do_setkey(int nargs, char **args)
{
    struct kbentry kbe;
    kbe.kb_table = strtoul(args[1], 0, 0);
    kbe.kb_index = strtoul(args[2], 0, 0);
    kbe.kb_value = strtoul(args[3], 0, 0);
    return setkey(&kbe);
}

int do_setprop(int nargs, char **args)
{
    const char *name = args[1];
    const char *value = args[2];

    if (value[0] == '$') {
        /* Use the value of a system property if value starts with '$' */
        value++;
        if (value[0] != '$') {
            value = property_get(value);
            if (!value) {
                ERROR("property %s has no value for assigning to %s\n", value, name);
                return -EINVAL;
            }
        } /* else fall through to support double '$' prefix for setting properties
           * to string literals that start with '$'
           */
    }

    property_set(name, value);
    return 0;
}

int do_setrlimit(int nargs, char **args)
{
    struct rlimit limit;
    int resource;
    resource = atoi(args[1]);
    limit.rlim_cur = atoi(args[2]);
    limit.rlim_max = atoi(args[3]);
    return setrlimit(resource, &limit);
}

int do_start(int nargs, char **args)
{
    struct service *svc;
    svc = service_find_by_name(args[1]);
    if (svc) {
        service_start(svc, NULL);
    }
    return 0;
}

int do_stop(int nargs, char **args)
{
    struct service *svc;
    svc = service_find_by_name(args[1]);
    if (svc) {
        service_stop(svc);
    }
    return 0;
}

int do_restart(int nargs, char **args)
{
    struct service *svc;
    svc = service_find_by_name(args[1]);
    if (svc) {
        service_stop(svc);
        service_start(svc, NULL);
    }
    return 0;
}

int do_trigger(int nargs, char **args)
{
    action_for_each_trigger(args[1], action_add_queue_tail);
    return 0;
}

int do_symlink(int nargs, char **args)
{
    return symlink(args[1], args[2]);
}

int do_rm(int nargs, char **args)
{
    return unlink(args[1]);
}

int do_rmdir(int nargs, char **args)
{
    return rmdir(args[1]);
}

int do_sysclktz(int nargs, char **args)
{
    struct timezone tz;

    if (nargs != 2)
        return -1;

    memset(&tz, 0, sizeof(tz));
    tz.tz_minuteswest = atoi(args[1]);   
    if (settimeofday(NULL, &tz))
        return -1;
    return 0;
}

int do_write(int nargs, char **args)
{
    const char *path = args[1];
    const char *value = args[2];
    if (value[0] == '$') {
        /* Write the value of a system property if value starts with '$' */
        value++;
        if (value[0] != '$') {
            value = property_get(value);
            if (!value) {
                ERROR("property %s has no value for writing to %s\n", value, path);
                return -EINVAL;
            }
        } /* else fall through to support double '$' prefix for writing
           * string literals that start with '$'
           */
    }

    return write_file(path, value);
}

int do_copy(int nargs, char **args)
{
    char *buffer = NULL;
    int rc = 0;
    int fd1 = -1, fd2 = -1;
    struct stat info;
    int brtw, brtr;
    char *p;

    if (nargs != 3)
        return -1;

    if (stat(args[1], &info) < 0) 
        return -1;

    if ((fd1 = open(args[1], O_RDONLY)) < 0) 
        goto out_err;

    if ((fd2 = open(args[2], O_WRONLY|O_CREAT|O_TRUNC, 0660)) < 0)
        goto out_err;

    if (!(buffer = malloc(info.st_size)))
        goto out_err;

    p = buffer;
    brtr = info.st_size;
    while(brtr) {
        rc = read(fd1, p, brtr);
        if (rc < 0)
            goto out_err;
        if (rc == 0)
            break;
        p += rc;
        brtr -= rc;
    }

    p = buffer;
    brtw = info.st_size;
    while(brtw) {
        rc = write(fd2, p, brtw);
        if (rc < 0)
            goto out_err;
        if (rc == 0)
            break;
        p += rc;
        brtw -= rc;
    }

    rc = 0;
    goto out;
out_err:
    rc = -1;
out:
    if (buffer)
        free(buffer);
    if (fd1 >= 0)
        close(fd1);
    if (fd2 >= 0)
        close(fd2);
    return rc;
}

int do_chown(int nargs, char **args) {
    /* GID is optional. */
    if (nargs == 3) {
        if (chown(args[2], decode_uid(args[1]), -1) < 0)
            return -errno;
    } else if (nargs == 4) {
        if (chown(args[3], decode_uid(args[1]), decode_uid(args[2])))
            return -errno;
    } else {
        return -1;
    }
    return 0;
}

static mode_t get_mode(const char *s) {
    mode_t mode = 0;
    while (*s) {
        if (*s >= '0' && *s <= '7') {
            mode = (mode<<3) | (*s-'0');
        } else {
            return -1;
        }
        s++;
    }
    return mode;
}

int do_chmod(int nargs, char **args) {
    mode_t mode = get_mode(args[1]);
    if (chmod(args[2], mode) < 0) {
        return -errno;
    }
    return 0;
}

int do_loglevel(int nargs, char **args) {
    if (nargs == 2) {
        klog_set_level(atoi(args[1]));
        return 0;
    }
    return -1;
}

int do_load_persist_props(int nargs, char **args) {
    if (nargs == 1) {
        load_persist_props();
        return 0;
    }
    return -1;
}
int ubi_attach_mtd(const char *node,
		   int dev_num,int mtd_num)
{
	int fd, ret;
	struct ubi_attach_req r;

	memset(&r, sizeof(struct ubi_attach_req), '\0');

	r.ubi_num = dev_num;
	r.mtd_num = mtd_num;
	r.vid_hdr_offset = 0;
	fd = open(node, O_RDONLY);
	if (fd == -1)
	{	
		ERROR ("%s(),open %s failed.\n",__FUNCTION__,node);
		return -1;
	}
	ret = ioctl(fd, UBI_IOCATT, &r);
	close(fd);
	if (ret == -1)
	{	
		ERROR ("%s(),attaching mtd%d to ubi%d failed.\n",__FUNCTION__,mtd_num,dev_num);
		return -1;
	}
	
	return ret;
}

int do_wait(int nargs, char **args)
{
    if (nargs == 2) {
        return wait_for_file(args[1], COMMAND_RETRY_TIMEOUT);
    }
    return -1;
}
int do_mount_ubifs(int nargs, char **args)
{
    char tmp[64];
    char *target,*vol_name,*source;
    char *options = NULL;
    unsigned flags = 0;
    int n, i;
	int ubi_dev_num = 0;
	int ret;
	int ubi_number = 0;

    for (n = 5; n < nargs; n++) {
        for (i = 0; mount_flags[i].name; i++) {
            if (!strcmp(args[n], mount_flags[i].name)) {
                flags |= mount_flags[i].flag;
                break;
            }
        }

        /* if our last argument isn't a flag, wolf it up as an option string */
        if (n + 1 == nargs && !mount_flags[i].name)
            options = args[n];
    }

	ubi_dev_num = atoi(args[1]);
	vol_name = args[2];
	source = args[3];
    target = args[4];

    if (!strncmp(source, "mtd@", 4)) {
        n = mtd_name_to_number(source + 4);
        if (n < 0) {
            return -1;
        }

		if (MS_REMOUNT == (flags & MS_REMOUNT))
		{
			for (i=0;i < MAX_MTD_PARTITIONS;i++)
			{
				if (n == mtd_part_map[i].number)
				{
					ubi_number = mtd_part_map[i].ubi_number;
					break;
				}
			}
			sprintf (tmp,"ubi%d:%s",ubi_number,vol_name);
		}
		else
		{
			ret = ubi_attach_mtd ("/dev/ubi_ctrl",ubi_dev_num,n);

			if (0 != ret){
				return -1;
			}
			for (i=0;i < MAX_MTD_PARTITIONS;i++)
			{
				if (n == mtd_part_map[i].number)
				{
					mtd_part_map[i].ubi_number = ubi_dev_num;
					break;
				}
			}
			sprintf (tmp,"ubi%d:%s",ubi_dev_num,vol_name);
		}

        if (mount(tmp, target, "ubifs", flags, options) < 0) {
            return -1;
        }       
    }
	else {
        if (mount(source, target, "ubifs", flags, options) < 0) {
            return -1;
        }
    }
	return 0;
}
/* do_mount_ubifs <vol_name> <device> <path> <flags ...> <options> */
int do_attach_ubifs(int nargs, char **args)
{
    char tmp[64];
    char *target,*vol_name,*source;
    char *options = NULL;
    unsigned flags = 0;
    int n, i;
	static int ubi_dev_num = 1;
	int ret;
	int ubi_number = 0;

    for (n = 4; n < nargs; n++) {
        for (i = 0; mount_flags[i].name; i++) {
            if (!strcmp(args[n], mount_flags[i].name)) {
                flags |= mount_flags[i].flag;
                break;
            }
        }

        /* if our last argument isn't a flag, wolf it up as an option string */
        if (n + 1 == nargs && !mount_flags[i].name)
            options = args[n];
    }

	vol_name = args[1];
	source = args[2];
    target = args[3];

    if (!strncmp(source, "mtd@", 4)) {
        n = mtd_name_to_number(source + 4);
        if (n < 0) {
            return -1;
        }

		if (MS_REMOUNT == (flags & MS_REMOUNT))
		{
			for (i=0;i < MAX_MTD_PARTITIONS;i++)
			{
				if (n == mtd_part_map[i].number)
				{
					ubi_number = mtd_part_map[i].ubi_number;
					break;
				}
			}
			sprintf (tmp,"ubi%d:%s",ubi_number,vol_name);
		}
		else
		{
			ret = ubi_attach_mtd ("/dev/ubi_ctrl",ubi_dev_num,n);

			if (0 != ret){
				return -1;
			}
			for (i=0;i < MAX_MTD_PARTITIONS;i++)
			{
				if (n == mtd_part_map[i].number)
				{
					mtd_part_map[i].ubi_number = ubi_dev_num;
					break;
				}
			}
			sprintf (tmp,"ubi%d:%s",ubi_dev_num++,vol_name);
		}

//        if (mount(tmp, target, "ubifs", flags, options) < 0) {
//            return -1;
//        }       
    }
	else {
//        if (mount(source, target, "ubifs", flags, options) < 0) {
//            return -1;
//        }
    }
	return 0;
}