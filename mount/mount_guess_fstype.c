/*
 * Thu Jul 14 07:32:40 1994: faith@cs.unc.edu added changes from Adam
 * J. Richter (adam@adam.yggdrasil.com) so that /proc/filesystems is used
 * if no -t option is given.  I modified his patches so that, if
 * /proc/filesystems is not available, the behavior of mount is the same as
 * it was previously.
 *
 * Wed Feb 8 09:23:18 1995: Mike Grupenhoff <kashmir@umiacs.UMD.EDU> added
 * a probe of the superblock for the type before /proc/filesystems is
 * checked.
 *
 * Fri Apr  5 01:13:33 1996: quinlan@bucknell.edu, fixed up iso9660 autodetect
 *
 * Wed Nov  11 11:33:55 1998: K.Garloff@ping.de, try /etc/filesystems before
 * /proc/filesystems
 *
 * aeb - many changes.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include "linux_fs.h"
#include "mount_guess_fstype.h"
#include "sundries.h"		/* for xstrdup */

#define ETC_FILESYSTEMS		"/etc/filesystems"
#define PROC_FILESYSTEMS	"/proc/filesystems"

#define SIZE(a) (sizeof(a)/sizeof(a[0]))

/* Most file system types can be recognized by a `magic' number
   in the superblock.  Note that the order of the tests is
   significant: by coincidence a filesystem can have the
   magic numbers for several file system types simultaneously.
   For example, the romfs magic lives in the 1st sector;
   xiafs does not touch the 1st sector and has its magic in
   the 2nd sector; ext2 does not touch the first two sectors. */

static inline unsigned short
swapped(unsigned short a) {
     return (a>>8) | (a<<8);
}

/*
    char *guess_fstype_from_superblock(const char *device);

    Probes the device and attempts to determine the type of filesystem
    contained within.

    Original routine by <jmorriso@bogomips.ww.ubc.ca>; made into a function
    for mount(8) by Mike Grupenhoff <kashmir@umiacs.umd.edu>.
    Read the superblock only once - aeb
    Added a test for iso9660 - aeb
    Added a test for high sierra (iso9660) - quinlan@bucknell.edu
    Corrected the test for xiafs - aeb
    Added romfs - aeb
    Added ufs from a patch by jj. But maybe there are several types of ufs?

    Currently supports: minix, ext, ext2, xiafs, iso9660, romfs, ufs
*/
static char
*magic_known[] = { "minix", "ext", "ext2", "xiafs", "iso9660", "romfs",
		   "ufs" };

static int
tested(const char *device) {
    char **m;

    for (m = magic_known; m - magic_known < SIZE(magic_known); m++)
        if (!strcmp(*m, device))
	    return 1;
    return 0;
}

static char *
fstype(const char *device) {
    int fd;
    char *type = NULL;
    union {
	struct minix_super_block ms;
	struct ext_super_block es;
	struct ext2_super_block e2s;
    } sb;
    union {
	struct xiafs_super_block xiasb;
	char romfs_magic[8];
    } xsb;
    struct ufs_super_block ufssb;
    union {
	struct iso_volume_descriptor iso;
	struct hs_volume_descriptor hs;
    } isosb;
    struct stat statbuf;

    /* opening and reading an arbitrary unknown path can have
       undesired side effects - first check that `device' refers
       to a block device */
    if (stat (device, &statbuf) || !S_ISBLK(statbuf.st_mode))
      return 0;

    fd = open(device, O_RDONLY);
    if (fd < 0)
      return 0;

    if (lseek(fd, 1024, SEEK_SET) != 1024
	|| read(fd, (char *) &sb, sizeof(sb)) != sizeof(sb))
	 goto io_error;

    if (ext2magic(sb.e2s) == EXT2_SUPER_MAGIC
	|| ext2magic(sb.e2s) == EXT2_PRE_02B_MAGIC
	|| ext2magic(sb.e2s) == swapped(EXT2_SUPER_MAGIC))
	 type = "ext2";

    else if (minixmagic(sb.ms) == MINIX_SUPER_MAGIC
	     || minixmagic(sb.ms) == MINIX_SUPER_MAGIC2)
	 type = "minix";

    else if (extmagic(sb.es) == EXT_SUPER_MAGIC)
	 type = "ext";

    if (!type) {
	 if (lseek(fd, 0, SEEK_SET) != 0
	     || read(fd, (char *) &xsb, sizeof(xsb)) != sizeof(xsb))
	      goto io_error;

	 if (xiafsmagic(xsb.xiasb) == _XIAFS_SUPER_MAGIC)
	      type = "xiafs";
	 else if(!strncmp(xsb.romfs_magic, "-rom1fs-", 8))
	      type = "romfs";
    }

    if (!type) {
	 if (lseek(fd, 8192, SEEK_SET) != 8192
	     || read(fd, (char *) &ufssb, sizeof(ufssb)) != sizeof(ufssb))
	      goto io_error;

	 if (ufsmagic(ufssb) == UFS_SUPER_MAGIC) /* also test swapped version? */
	      type = "ufs";
    }

    if (!type) {
	 if (lseek(fd, 0x8000, SEEK_SET) != 0x8000
	     || read(fd, (char *) &isosb, sizeof(isosb)) != sizeof(isosb))
	      goto io_error;

	 if(strncmp(isosb.iso.id, ISO_STANDARD_ID, sizeof(isosb.iso.id)) == 0
	    || strncmp(isosb.hs.id, HS_STANDARD_ID, sizeof(isosb.hs.id)) == 0)
	      type = "iso9660";
    }

    close (fd);
    return(type);

io_error:
    perror(device);
    close(fd);
    return 0;
}

char *
guess_fstype_from_superblock(const char *spec) {
      char *type = fstype(spec);
      if (verbose) {
	  printf ("mount: you didn't specify a filesystem type for %s\n",
		  spec);
	  if (type)
	    printf ("       I will try type %s\n", type);
	  else
	    printf ("       I will try all types mentioned in %s or %s\n",
		    ETC_FILESYSTEMS, PROC_FILESYSTEMS);
      }
      return type;
}

static FILE *procfs;

static void
procfsclose(void) {
	if (procfs)
		fclose (procfs);
	procfs = 0;
}

static int
procfsopen(void) {
	procfs = fopen(ETC_FILESYSTEMS, "r");
	if (!procfs)
		procfs = fopen(PROC_FILESYSTEMS, "r");
	return (procfs != NULL);
}

static char *
procfsnext(void) {
   char line[100];
   static char fsname[50];

   while (fgets(line, sizeof(line), procfs)) {
      if (sscanf (line, "nodev %[^\n]\n", fsname) == 1) continue;
      if (sscanf (line, " %[^ \n]\n", fsname) != 1) continue;
      return fsname;
   }
   return 0;
}

int
is_in_procfs(const char *type) {
    char *fsname;

    if (procfsopen()) {
	while ((fsname = procfsnext()) != NULL)
	  if (!strcmp(fsname, type))
	    return 1;
    }
    return 0;
}

int
procfsloop(int (*mount_fn)(struct mountargs *), struct mountargs *args,
	   char **type) {
   char *fsname;

   if (!procfsopen())
     return -1;
   while ((fsname = procfsnext()) != NULL) {
      if (tested (fsname))
	 continue;
      args->type = fsname;
      if ((*mount_fn) (args) == 0) {
	 *type = xstrdup(fsname);
	 procfsclose();
	 return 0;
      } else if (errno != EINVAL) {
         *type = "guess";
	 procfsclose();
	 return 1;
      }
   }
   procfsclose();
   *type = NULL;

   return -1;
}

int
have_procfs(void) {
	return procfs != NULL;
}
