#if !defined(_WIN32)

/**
 * platform_posix.c — POSIX storage device enumeration (Linux + macOS).
 *
 * Implements platform_enum_storage_devices() for non-Windows platforms.
 *
 * Linux strategy:
 *   - Parse /proc/mounts via setmntent() / getmntent() (mntent.h).
 *   - statvfs() per mount for capacity figures.
 *   - Filesystem type string → filesystem_type_from_name().
 *   - is_network: fstype in {cifs, smb2, smb3, smbfs, nfs, nfs3, nfs4, davfs, davfs2}.
 *   - is_removable: heuristic — mount point starts with /media or /run/media.
 *   - MTP (best-effort): detect mounts with fstype "fuse.gvfsd-fuse" or paths
 *     under ~/.gvfs / /run/user/*/gvfs; marked FS_MTP, SCAN_CAP_GENERIC.
 *   - Pseudo-filesystems (proc, sys, devtmpfs, tmpfs, cgroup*, etc.) are skipped.
 *
 * macOS strategy:
 *   - getmntinfo() returns a struct statfs array without /proc/mounts.
 *   - statfs.f_fstypename → filesystem_type_from_name().
 *   - MNT_RDONLY flag → is_read_only.
 *   - MNT_LOCAL absent → is_network.
 *   - f_mntfromname heuristic for is_removable (disk2, disk3, … vs disk0/disk1).
 *
 * Error tolerance:
 *   - Per-mount statvfs / statfs failures → skip that entry silently.
 *   - Permission errors are non-fatal.
 */

#include "diskatlas_storage.h"
#include "da_platform.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/statvfs.h>

#if defined(__linux__)
#  include <mntent.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
      defined(__OpenBSD__)
#  include <sys/param.h>
#  include <sys/mount.h>
#endif

/* -------------------------------------------------------------------------- */
/* Pseudo-filesystem filter (Linux)                                          */
/* -------------------------------------------------------------------------- */

#if defined(__linux__)

/* Returns non-zero if the filesystem type should be excluded from results.
 * We skip virtual/kernel filesystems that hold no user data. */
static int is_pseudo_fs(const char *fstype) {
  static const char *const pseudo[] = {
    "proc", "sysfs", "devtmpfs", "devpts", "tmpfs", "hugetlbfs",
    "mqueue", "securityfs", "debugfs", "tracefs", "configfs",
    "pstore", "efivarfs", "fusectl", "bpf", "cgroup", "cgroup2",
    "autofs", "rpc_pipefs", "nfsd", "squashfs", "overlay",
    "aufs", "ramfs", "rootfs", NULL
  };
  if (!fstype) {
    return 1;
  }
  for (int i = 0; pseudo[i]; i++) {
    if (strcmp(fstype, pseudo[i]) == 0) {
      return 1;
    }
  }
  /* cgroup variants like cgroup2, cgroupv2 */
  if (strncmp(fstype, "cgroup", 6) == 0) {
    return 1;
  }
  return 0;
}

/* Returns non-zero if the mount point looks like a removable/external device
 * based on conventional Linux mount locations. */
static int is_removable_mount_path(const char *mnt_dir) {
  if (!mnt_dir) {
    return 0;
  }
  /* /media/... or /run/media/... are the standard udisks2 mount points. */
  if (strncmp(mnt_dir, "/media/", 7)     == 0) {
    return 1;
  }
  if (strncmp(mnt_dir, "/run/media/", 11) == 0) {
    return 1;
  }
  /* Some older distributions use /mnt/usb* or /mnt/sd* */
  if (strncmp(mnt_dir, "/mnt/usb", 8)    == 0) {
    return 1;
  }
  if (strncmp(mnt_dir, "/mnt/sd", 7)     == 0) {
    return 1;
  }
  return 0;
}

/* Returns non-zero if the fstype indicates a GVFS/MTP virtual mount. */
static int is_gvfs_mtp_mount(const char *fstype, const char *mnt_dir) {
  if (!fstype || !mnt_dir) {
    return 0;
  }
  if (strcmp(fstype, "fuse.gvfsd-fuse") == 0 ||
      strcmp(fstype, "fuse.gvfs-fuse-daemon") == 0) {
    /* GVFS FUSE bridge — check if the mount path looks like an MTP share. */
    if (strstr(mnt_dir, "mtp") || strstr(mnt_dir, "gvfs") ||
        strncmp(mnt_dir, "/run/user/", 10) == 0) {
      return 1;
    }
    /* ~/.gvfs style (older GVFS). */
    if (strstr(mnt_dir, "/.gvfs")) {
      return 1;
    }
  }
  return 0;
}

#endif /* __linux__ */

/* -------------------------------------------------------------------------- */
/* platform_enum_storage_devices — Linux implementation                      */
/* -------------------------------------------------------------------------- */

#if defined(__linux__)

DISKATLAS_API size_t platform_enum_storage_devices(storage_device_t **devices) {
  if (!devices) {
    return 0;
  }

  storage_device_t *arr = NULL;
  size_t len = 0;
  size_t cap = 0;

  FILE *fp = setmntent("/proc/mounts", "r");
  if (!fp) {
    /* Fallback to /etc/mtab if /proc/mounts is unavailable. */
    fp = setmntent("/etc/mtab", "r");
  }
  if (!fp) {
    *devices = NULL;
    return 0;
  }

  struct mntent ent;
  char mntbuf[4096];

  while (getmntent_r(fp, &ent, mntbuf, (int)sizeof(mntbuf)) != NULL) {
    const char *fstype  = ent.mnt_type;
    const char *mnt_dir = ent.mnt_dir;
    const char *mnt_fsname = ent.mnt_fsname;

    /* Skip pseudo / virtual filesystems. */
    if (is_pseudo_fs(fstype)) {
      continue;
    }

    /* Skip bind mounts (same device mounted at multiple paths). */
    if (hasmntopt(&ent, "bind") != NULL) {
      continue;
    }

    storage_device_t *dev = da_device_array_push(&arr, &len, &cap);
    if (!dev) {
      break;
    }

    /* Mount path */
    strncpy(dev->mount_path, mnt_dir, sizeof(dev->mount_path) - 1);

    /* Display name: use the source device, falling back to mount point. */
    if (mnt_fsname && mnt_fsname[0] != '\0' && mnt_fsname[0] != '/') {
      /* e.g. "//server/share" for CIFS */
      snprintf(dev->display_name, sizeof(dev->display_name),
               "%s", mnt_fsname);
    } else if (mnt_fsname && mnt_fsname[0] == '/') {
      /* Local block device: use the basename for brevity. */
      const char *base = strrchr(mnt_fsname, '/');
      base = base ? base + 1 : mnt_fsname;
      if (base[0] != '\0') {
        snprintf(dev->display_name, sizeof(dev->display_name), "%s", base);
      } else {
        snprintf(dev->display_name, sizeof(dev->display_name), "%s", mnt_dir);
      }
    } else {
      snprintf(dev->display_name, sizeof(dev->display_name), "%s", mnt_dir);
    }

    /* Filesystem type */
    if (is_gvfs_mtp_mount(fstype, mnt_dir)) {
      dev->fs_type      = FS_MTP;
      dev->is_removable = 1;
    } else {
      dev->fs_type = filesystem_type_from_name(fstype);

      /* Network filesystems */
      if (dev->fs_type == FS_NETWORK ||
          strcmp(fstype, "cifs")   == 0 || strcmp(fstype, "smbfs") == 0 ||
          strcmp(fstype, "nfs")    == 0 || strcmp(fstype, "nfs4")  == 0 ||
          strcmp(fstype, "davfs")  == 0 || strcmp(fstype, "davfs2") == 0) {
        dev->is_network = 1;
      }

      dev->is_removable = is_removable_mount_path(mnt_dir) ? 1 : 0;
    }

    /* Read-only: check mount options string. */
    dev->is_read_only = (hasmntopt(&ent, "ro") != NULL) ? 1 : 0;

    /* Disk space via statvfs. */
    struct statvfs svfs;
    if (statvfs(mnt_dir, &svfs) == 0) {
      dev->total_bytes = (uint64_t)svfs.f_blocks * (uint64_t)svfs.f_frsize;
      dev->free_bytes  = (uint64_t)svfs.f_bavail * (uint64_t)svfs.f_frsize;
    }
    /* statvfs failure (e.g. network share offline) → leave bytes at 0. */
  }

  endmntent(fp);

  if (len == 0) {
    free(arr);
    *devices = NULL;
    return 0;
  }

  *devices = arr;
  return len;
}

#endif /* __linux__ */

/* -------------------------------------------------------------------------- */
/* platform_enum_storage_devices — macOS / BSD implementation                */
/* -------------------------------------------------------------------------- */

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__)

/* Returns non-zero if the block device name suggests a removable/external
 * drive on macOS. Internal drives are typically disk0 or disk1; external
 * drives and USB volumes appear as disk2, disk3, etc., or under /Volumes. */
static int macos_is_removable(const struct statfs *sf) {
  /* MNT_LOCAL: present for local filesystems. Network = !MNT_LOCAL. */
  if (!(sf->f_flags & MNT_LOCAL)) {
    return 0; /* Network drives are not removable in the physical sense. */
  }
  /* Heuristic: mount points under /Volumes (not "/") are external/removable. */
  if (strncmp(sf->f_mntonname, "/Volumes/", 9) == 0) {
    return 1;
  }
  /* Disk number heuristic: disk0 / disk1 are typically internal SSDs. */
  const char *dev = sf->f_mntfromname;
  /* Skip "/dev/" prefix if present. */
  if (strncmp(dev, "/dev/", 5) == 0) {
    dev += 5;
  }
  if (strncmp(dev, "disk", 4) == 0) {
    char *endptr = NULL;
    long disknum = strtol(dev + 4, &endptr, 10);
    if (endptr && endptr != dev + 4 && disknum >= 2) {
      return 1;
    }
  }
  return 0;
}

DISKATLAS_API size_t platform_enum_storage_devices(storage_device_t **devices) {
  if (!devices) {
    return 0;
  }

  struct statfs *mntbuf = NULL;
  int count = getmntinfo(&mntbuf, MNT_NOWAIT);
  if (count <= 0 || !mntbuf) {
    *devices = NULL;
    return 0;
  }

  storage_device_t *arr = NULL;
  size_t len = 0;
  size_t cap = 0;

  for (int i = 0; i < count; i++) {
    const struct statfs *sf = &mntbuf[i];

    /* Skip devfs, autofs, and other virtual filesystems. */
    if (strcmp(sf->f_fstypename, "devfs")   == 0 ||
        strcmp(sf->f_fstypename, "autofs")  == 0 ||
        strcmp(sf->f_fstypename, "nullfs")  == 0 ||
        strcmp(sf->f_fstypename, "union")   == 0 ||
        strcmp(sf->f_fstypename, "fdesc")   == 0 ||
        strcmp(sf->f_fstypename, "procfs")  == 0) {
      continue;
    }

    storage_device_t *dev = da_device_array_push(&arr, &len, &cap);
    if (!dev) {
      break;
    }

    /* Mount path */
    strncpy(dev->mount_path, sf->f_mntonname, sizeof(dev->mount_path) - 1);

    /* Display name: volume name if available, otherwise mount point basename. */
    {
      const char *base = strrchr(sf->f_mntonname, '/');
      base = (base && base[1] != '\0') ? base + 1 : sf->f_mntonname;
      strncpy(dev->display_name, base, sizeof(dev->display_name) - 1);
    }

    /* Filesystem type */
    dev->fs_type = filesystem_type_from_name(sf->f_fstypename);

    /* Network */
    dev->is_network = (sf->f_flags & MNT_LOCAL) ? 0 : 1;
    if (dev->is_network && dev->fs_type == FS_UNKNOWN) {
      dev->fs_type = FS_NETWORK;
    }

    /* Read-only */
    dev->is_read_only = (sf->f_flags & MNT_RDONLY) ? 1 : 0;

    /* Removable */
    dev->is_removable = macos_is_removable(sf) ? 1 : 0;

    /* Disk space */
    dev->total_bytes = (uint64_t)sf->f_blocks * (uint64_t)sf->f_bsize;
    dev->free_bytes  = (uint64_t)sf->f_bavail * (uint64_t)sf->f_bsize;
  }

  if (len == 0) {
    free(arr);
    *devices = NULL;
    return 0;
  }

  *devices = arr;
  return len;
}

#endif /* __APPLE__ || BSD */

#endif /* !_WIN32 */
