#if defined(_WIN32)

/**
 * platform_win32.c — Windows storage device enumeration.
 *
 * Implements platform_enum_storage_devices() for Windows.
 *
 * Enumeration strategy:
 *   1. GetLogicalDrives() → bitmask of drive letters A–Z
 *   2. Per drive letter:
 *        GetDriveTypeW()          → is_removable / is_network
 *        GetVolumeInformationW()  → display label, filesystem name, read-only flag
 *        GetDiskFreeSpaceExW()    → total_bytes, free_bytes
 *        filesystem_type_from_name() → fs_type
 *   3. MTP portable devices: best-effort via Windows Portable Devices (WPD)
 *      COM API. If COM init or device manager creation fails, MTP detection
 *      is silently skipped — it never blocks startup or the enumeration result.
 *
 * UNC / network volumes:
 *   Drives reported as DRIVE_REMOTE are included with is_network=1 and
 *   fs_type=FS_NETWORK (NTFS/FAT detection still attempted; overridden to
 *   FS_NETWORK when GetVolumeInformation fails for network paths).
 *
 * Requires: kernel32 (GetLogicalDrives, GetDriveType, GetVolumeInformation,
 *            GetDiskFreeSpaceEx) — already linked via advapi32/kernel32 in the
 *            CMakeLists.txt diskatlas_core target.
 * Optional: portabledeviceapi (WPD/MTP) — COM-based, loaded at runtime if
 *            CoInitializeEx succeeds; failure is non-fatal.
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
/* Require at least Windows Vista for WPD / IPortableDeviceManager. */
#ifndef WINVER
#define WINVER 0x0600
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
/* COM base APIs: CoInitializeEx, CoCreateInstance, CoTaskMemAlloc/Free.
 * Must be included explicitly after windows.h when WIN32_LEAN_AND_MEAN is set. */
#include <objbase.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "diskatlas_storage.h"
#include "da_platform.h"

/* -------------------------------------------------------------------------- */
/* Forward declaration: MTP enumeration (defined later in this file)         */
/* -------------------------------------------------------------------------- */

static void enum_mtp_devices(storage_device_t **arr, size_t *len, size_t *cap);

/* -------------------------------------------------------------------------- */
/* platform_enum_storage_devices — Windows implementation                    */
/* -------------------------------------------------------------------------- */

DISKATLAS_API size_t platform_enum_storage_devices(storage_device_t **devices) {
  if (!devices) {
    return 0;
  }

  storage_device_t *arr = NULL;
  size_t len = 0;
  size_t cap = 0;

  DWORD drive_mask = GetLogicalDrives();

  for (int i = 0; i < 26; i++) {
    if (!(drive_mask & (1u << i))) {
      continue;
    }

    /* Build "X:\\" root path (wide and UTF-8). */
    WCHAR root_w[8];
    root_w[0] = (WCHAR)('A' + i);
    root_w[1] = L':';
    root_w[2] = L'\\';
    root_w[3] = L'\0';

    char root_utf8[8];
    root_utf8[0] = (char)('A' + i);
    root_utf8[1] = ':';
    root_utf8[2] = '\\';
    root_utf8[3] = '\0';

    /* Skip drives that are not ready (e.g. empty CD-ROM tray). */
    UINT drive_type = GetDriveTypeW(root_w);
    if (drive_type == DRIVE_NO_ROOT_DIR || drive_type == DRIVE_UNKNOWN) {
      continue;
    }

    storage_device_t *dev = da_device_array_push(&arr, &len, &cap);
    if (!dev) {
      break; /* Out of memory; return what we have. */
    }

    /* Mount path */
    strncpy(dev->mount_path, root_utf8, sizeof(dev->mount_path) - 1);

    /* Removable / network flags */
    dev->is_removable = (drive_type == DRIVE_REMOVABLE) ? 1 : 0;
    dev->is_network   = (drive_type == DRIVE_REMOTE)    ? 1 : 0;

    /* Volume label, filesystem name, and flags */
    WCHAR vol_label_w[256]  = {0};
    WCHAR fs_name_w[64]     = {0};
    DWORD fs_flags          = 0;

    BOOL vi_ok = GetVolumeInformationW(
        root_w,
        vol_label_w, (DWORD)(sizeof(vol_label_w) / sizeof(vol_label_w[0])),
        NULL,   /* serial number — unused */
        NULL,   /* max component length — unused */
        &fs_flags,
        fs_name_w, (DWORD)(sizeof(fs_name_w) / sizeof(fs_name_w[0])));

    if (vi_ok) {
      /* Display name: "X: VolLabel" or just "X:" when label is empty. */
      char label_utf8[256] = {0};
      da_wide_to_utf8(vol_label_w, label_utf8, (int)sizeof(label_utf8));

      if (label_utf8[0] != '\0') {
        snprintf(dev->display_name, sizeof(dev->display_name),
                 "%c: %s", 'A' + i, label_utf8);
      } else {
        snprintf(dev->display_name, sizeof(dev->display_name),
                 "%c:", 'A' + i);
      }

      /* Read-only: FILE_READ_ONLY_VOLUME flag */
      dev->is_read_only = (fs_flags & FILE_READ_ONLY_VOLUME) ? 1 : 0;

      /* Filesystem type */
      char fs_name_utf8[64] = {0};
      da_wide_to_utf8(fs_name_w, fs_name_utf8, (int)sizeof(fs_name_utf8));
      dev->fs_type = filesystem_type_from_name(fs_name_utf8);

      /* Network drives: override fs_type if detection failed. */
      if (dev->is_network && dev->fs_type == FS_UNKNOWN) {
        dev->fs_type = FS_NETWORK;
      }
    } else {
      /* GetVolumeInformation failed (e.g. network drive offline, no media). */
      snprintf(dev->display_name, sizeof(dev->display_name), "%c:", 'A' + i);
      dev->fs_type = dev->is_network ? FS_NETWORK : FS_UNKNOWN;
    }

    /* Disk space */
    ULARGE_INTEGER free_caller = {0}, total_all = {0}, total_free = {0};
    if (GetDiskFreeSpaceExW(root_w, &free_caller, &total_all, &total_free)) {
      dev->total_bytes = (uint64_t)total_all.QuadPart;
      dev->free_bytes  = (uint64_t)total_free.QuadPart;
    }
    /* On failure (e.g. no media) leave total_bytes and free_bytes as 0. */
  }

  /* --- MTP / Portable devices (best-effort, never blocks on failure) --- */
  enum_mtp_devices(&arr, &len, &cap);

  if (len == 0) {
    free(arr);
    *devices = NULL;
    return 0;
  }

  *devices = arr;
  return len;
}

/* -------------------------------------------------------------------------- */
/* MTP / Portable Device enumeration (Windows Portable Devices API)          */
/*                                                                            */
/* WPD is COM-based. We use late binding via CoCreateInstance so that the    */
/* binary still loads on systems without portabledeviceapi.dll.              */
/* All failure paths are silent and non-fatal.                               */
/* -------------------------------------------------------------------------- */

/* WPD GUIDs and interface definitions — replicate the minimum needed so we
 * do not require the WPD SDK headers (which may be absent in MinGW).       */

/* CLSID_PortableDeviceManager = {0AF10CEC-2ECD-4B92-9581-34F6AE0637F3} */
static const GUID DA_CLSID_PortableDeviceManager = {
  0x0AF10CEC, 0x2ECD, 0x4B92,
  {0x95, 0x81, 0x34, 0xF6, 0xAE, 0x06, 0x37, 0xF3}
};

/* IID_IPortableDeviceManager = {A1567595-4C2F-4574-A6FA-ECEF917B9A40} */
static const GUID DA_IID_IPortableDeviceManager = {
  0xA1567595, 0x4C2F, 0x4574,
  {0xA6, 0xFA, 0xEC, 0xEF, 0x91, 0x7B, 0x9A, 0x40}
};

/*
 * Minimal vtable for IPortableDeviceManager — only the methods we call.
 * The real interface has more methods; we only need GetDevices and
 * GetDeviceFriendlyName (indices 3 and 4 in the vtable after the 3
 * IUnknown methods).
 *
 * Vtable order (from WPD SDK):
 *   0: QueryInterface
 *   1: AddRef
 *   2: Release
 *   3: GetDevices
 *   4: RefreshDeviceList
 *   5: GetDeviceFriendlyName
 *   6: GetDeviceDescription
 *   7: GetDeviceManufacturer
 *   8: GetDeviceProperty
 *   9: GetPrivateDevices
 */
typedef struct IPortableDeviceManagerVtbl {
  /* IUnknown */
  HRESULT (STDMETHODCALLTYPE *QueryInterface)(void *This, REFIID riid,
                                              void **ppvObject);
  ULONG   (STDMETHODCALLTYPE *AddRef)(void *This);
  ULONG   (STDMETHODCALLTYPE *Release)(void *This);
  /* IPortableDeviceManager */
  HRESULT (STDMETHODCALLTYPE *GetDevices)(void *This, LPWSTR *pPnPDeviceIDs,
                                          DWORD *pcPnPDeviceIDs);
  HRESULT (STDMETHODCALLTYPE *RefreshDeviceList)(void *This);
  HRESULT (STDMETHODCALLTYPE *GetDeviceFriendlyName)(
      void *This, LPCWSTR pszPnPDeviceID,
      WCHAR *pDeviceFriendlyName, DWORD *pcchDeviceFriendlyName);
} IPortableDeviceManagerVtbl;

typedef struct IPortableDeviceManager {
  IPortableDeviceManagerVtbl *lpVtbl;
} IPortableDeviceManager;

static void enum_mtp_devices(storage_device_t **arr, size_t *len,
                              size_t *cap) {
  /* Initialise COM. COINIT_MULTITHREADED is appropriate for a worker thread;
   * if the caller already initialised COM with a different model, this will
   * return RPC_E_CHANGED_MODE — that is fine; we proceed anyway (existing
   * COM apartment can still create the device manager). */
  HRESULT hr_co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
  BOOL co_inited = SUCCEEDED(hr_co) || hr_co == RPC_E_CHANGED_MODE;

  IPortableDeviceManager *pdm = NULL;
  HRESULT hr = CoCreateInstance(
      &DA_CLSID_PortableDeviceManager,
      NULL,
      CLSCTX_INPROC_SERVER,
      &DA_IID_IPortableDeviceManager,
      (void **)&pdm);

  if (FAILED(hr) || !pdm) {
    /* WPD not available on this system — skip silently. */
    if (co_inited && SUCCEEDED(hr_co)) {
      CoUninitialize();
    }
    return;
  }

  /* First call: get count. */
  DWORD device_count = 0;
  hr = pdm->lpVtbl->GetDevices(pdm, NULL, &device_count);
  if (FAILED(hr) || device_count == 0) {
    pdm->lpVtbl->Release(pdm);
    if (co_inited && SUCCEEDED(hr_co)) {
      CoUninitialize();
    }
    return;
  }

  /* Allocate PnP ID array. */
  LPWSTR *pnp_ids = (LPWSTR *)CoTaskMemAlloc(
      device_count * sizeof(LPWSTR));
  if (!pnp_ids) {
    pdm->lpVtbl->Release(pdm);
    if (co_inited && SUCCEEDED(hr_co)) {
      CoUninitialize();
    }
    return;
  }
  memset(pnp_ids, 0, device_count * sizeof(LPWSTR));

  hr = pdm->lpVtbl->GetDevices(pdm, pnp_ids, &device_count);
  if (SUCCEEDED(hr)) {
    for (DWORD di = 0; di < device_count; di++) {
      if (!pnp_ids[di]) {
        continue;
      }

      storage_device_t *dev = da_device_array_push(arr, len, cap);
      if (!dev) {
        break;
      }

      dev->fs_type      = FS_MTP;
      dev->is_removable = 1;
      dev->is_network   = 0;
      dev->is_read_only = 0;

      /* Mount path: use PnP device ID as the path identifier (UTF-8). */
      da_wide_to_utf8(pnp_ids[di], dev->mount_path,
                      (int)sizeof(dev->mount_path));

      /* Friendly name */
      DWORD name_cch = 256;
      WCHAR name_w[256] = {0};
      HRESULT hr_name = pdm->lpVtbl->GetDeviceFriendlyName(
          pdm, pnp_ids[di], name_w, &name_cch);
      if (SUCCEEDED(hr_name) && name_w[0] != L'\0') {
        da_wide_to_utf8(name_w, dev->display_name,
                        (int)sizeof(dev->display_name));
      } else {
        /* Fallback: truncate PnP ID to display_name. */
        strncpy(dev->display_name, dev->mount_path,
                sizeof(dev->display_name) - 1);
      }

      CoTaskMemFree(pnp_ids[di]);
    }
  }

  CoTaskMemFree(pnp_ids);
  pdm->lpVtbl->Release(pdm);

  if (co_inited && SUCCEEDED(hr_co)) {
    CoUninitialize();
  }
}

#endif /* _WIN32 */
