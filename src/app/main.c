/*
 * DiskAtlas — Win32 GUI (ListView report mode, no GTK).
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdint.h>

#include "diskatlas.h"

#define DA_IDC_LISTVIEW 1001
#define DA_IDC_STATUS 1002
#define DA_IDC_SEARCH 1003
#define DA_IDC_SCAN 1004
#define DA_TIMER_SCAN 1
#define DA_TIMER_FILL 2
#define DA_TIMER_SEARCH_DEBOUNCE 3
#define DA_TIMER_FILTER_CHUNK 4
/** Rows per WM_TIMER tick (filter pass over master indices, keeps pump responsive). */
#define DA_FILTER_BATCH 4000
#define DA_SEARCH_DEBOUNCE_MS 200

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

typedef struct {
  HWND hwnd;
  HWND hwndScan;
  HWND hwndSearch;
  HWND hwndList;
  HWND hwndStatus;
  scan_result_t *scan;
  BOOL listPopulated;
  char *scan_root_utf8;
  /** Full result set: node indices sorted by size (desc). Owned; lives until WM_DESTROY. */
  size_t *masterIndices;
  size_t masterCount;
  /** Filtered view: node indices in same size order; owned when non-NULL. */
  size_t *filteredIndices;
  size_t filteredCount;
  size_t filteredCap;
  WCHAR filterText[512];
  BOOL filterActive;
  size_t filterScanPos;
  BOOL filterBuildRunning;
  size_t populateTotal;
} AppState;

static void EnableScanButton(AppState *app, BOOL enable) {
  if (app != NULL && app->hwndScan != NULL) {
    EnableWindow(app->hwndScan, enable);
  }
}

static char *DupWideToUtf8(const wchar_t *wide) {
  if (wide == NULL) {
    wide = L"";
  }
  int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
  if (n <= 0) {
    return NULL;
  }
  char *u = (char *)malloc((size_t)n);
  if (!u) {
    return NULL;
  }
  WideCharToMultiByte(CP_UTF8, 0, wide, -1, u, n, NULL, NULL);
  return u;
}

static void Utf8BasenameToWideBuf(const char *path_utf8, WCHAR *out, int cchOut) {
  if (!out || cchOut <= 0) {
    return;
  }
  out[0] = L'\0';
  const char *p = (path_utf8 != NULL) ? path_utf8 : "";
  const char *base = p;
  for (const char *q = p; *q; ++q) {
    if (*q == '/' || *q == '\\') {
      base = q + 1;
    }
  }
  if (MultiByteToWideChar(CP_UTF8, 0, base, -1, out, cchOut) <= 0) {
    wcsncpy(out, L"?", (size_t)cchOut - 1);
    out[(size_t)cchOut - 1] = L'\0';
  }
}

static const WCHAR *WideType(uint32_t attr) {
  uint32_t kind = attr & DISKATLAS_NODE_KIND_MASK;
  if (kind == DISKATLAS_NODE_KIND_DIR) {
    return L"Folder";
  }
  if (kind == DISKATLAS_NODE_KIND_SYMLINK) {
    return L"Symlink";
  }
  if (kind == DISKATLAS_NODE_KIND_FILE) {
    return L"File";
  }
  return L"Unknown";
}

static void FormatSizeW(uint64_t n, WCHAR *dst, DWORD cch) {
  WCHAR tmp[96];
  if (n < 1024ull) {
    swprintf(tmp, ARRAYSIZE(tmp), L"%llu B", (unsigned long long)n);
  } else if (n < (1024ull * 1024ull)) {
    WCHAR kb[96];
    uint64_t v = (n * 100 + 512) / 1024ull;
    swprintf(kb, ARRAYSIZE(kb), L"%llu.%02llu KiB", (unsigned long long)(v / 100),
             (unsigned long long)(v % 100));
    wcscpy(tmp, kb);
  } else if (n < (1024ull * 1024ull * 1024ull)) {
    uint64_t v = (n * 100 + ((1024ull * 1024ull) / 2)) / (1024ull * 1024ull);
    swprintf(tmp, ARRAYSIZE(tmp), L"%llu.%02llu MiB", (unsigned long long)(v / 100),
             (unsigned long long)(v % 100));
  } else {
    uint64_t v =
        (n * 100 + ((1024ull * 1024ull * 1024ull) / 2)) / (1024ull * 1024ull * 1024ull);
    swprintf(tmp, ARRAYSIZE(tmp), L"%llu.%02llu GiB", (unsigned long long)(v / 100),
             (unsigned long long)(v % 100));
  }
  if (wcslen(tmp) < cch) {
    wcscpy(dst, tmp);
  } else {
    dst[0] = L'\0';
  }
}

static const file_node_t *da_qsort_nodes;

static int CmpIndexBySizeDesc(const void *pa, const void *pb) {
  size_t ia = *(const size_t *)pa;
  size_t ib = *(const size_t *)pb;
  uint64_t sa = da_qsort_nodes[ia].size_bytes;
  uint64_t sb = da_qsort_nodes[ib].size_bytes;
  if (sa > sb) {
    return -1;
  }
  if (sa < sb) {
    return 1;
  }
  return (ia > ib) - (ia < ib);
}

static WCHAR CharLowerInvariant(WCHAR ch) {
  WCHAR o[2];
  o[0] = ch;
  o[1] = 0;
  CharLowerBuffW(o, 1);
  return o[0];
}

static BOOL FilterHasWildcard(const WCHAR *filter) {
  if (!filter) {
    return FALSE;
  }
  for (const WCHAR *s = filter; *s; ++s) {
    if (*s == L'*' || *s == L'?') {
      return TRUE;
    }
  }
  return FALSE;
}

static BOOL WcsContainsCi(const WCHAR *haystack, const WCHAR *needle) {
  if (!needle || !needle[0]) {
    return TRUE;
  }
  if (!haystack) {
    haystack = L"";
  }
  for (const WCHAR *p = haystack; *p; ++p) {
    const WCHAR *h = p;
    const WCHAR *n = needle;
    while (*n != L'\0' && *h != L'\0' &&
           CharLowerInvariant(*h) == CharLowerInvariant(*n)) {
      ++h;
      ++n;
    }
    if (*n == L'\0') {
      return TRUE;
    }
  }
  return FALSE;
}

static BOOL WildMatchCiRecursive(const WCHAR *pat, const WCHAR *str, int depthGuard) {
  if (depthGuard++ > 10000) {
    return FALSE;
  }
  if (*pat == L'\0') {
    return *str == L'\0';
  }
  if (*pat == L'*') {
    if (pat[1] == L'\0') {
      return TRUE;
    }
    while (*str != L'\0') {
      if (WildMatchCiRecursive(pat + 1, str, depthGuard)) {
        return TRUE;
      }
      ++str;
    }
    return WildMatchCiRecursive(pat + 1, str, depthGuard);
  }
  if (*pat == L'?') {
    return (*str != L'\0') && WildMatchCiRecursive(pat + 1, str + 1, depthGuard);
  }
  if (CharLowerInvariant(*pat) != CharLowerInvariant(*str)) {
    return FALSE;
  }
  return WildMatchCiRecursive(pat + 1, str + 1, depthGuard);
}

static BOOL WildMatchCi(const WCHAR *pat, const WCHAR *str) {
  return WildMatchCiRecursive(pat != NULL ? pat : L"", str != NULL ? str : L"", 0);
}

static BOOL WideNameMatchesFilter(const WCHAR *name, const WCHAR *filter) {
  if (!filter || !filter[0]) {
    return TRUE;
  }
  if (FilterHasWildcard(filter)) {
    return WildMatchCi(filter, name);
  }
  return WcsContainsCi(name, filter);
}

static void ListDispInfo(HWND hwndLV, LPARAM lParam, AppState *app) {
  (void)hwndLV;
  if (!app || !app->scan) {
    return;
  }
  NMLVDISPINFOW *di = (NMLVDISPINFOW *)lParam;
  int iItem = di->item.iItem;
  int iSub = di->item.iSubItem;
  const size_t visCount = app->filterActive ? app->filteredCount : app->masterCount;
  if (iItem < 0 || visCount == 0 || (size_t)iItem >= visCount) {
    return;
  }

  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    return;
  }

  size_t nodeIdx;
  if (!app->filterActive) {
    if (app->masterIndices == NULL) {
      return;
    }
    nodeIdx = app->masterIndices[(size_t)iItem];
  } else {
    if (app->filteredIndices == NULL) {
      return;
    }
    nodeIdx = app->filteredIndices[(size_t)iItem];
  }
  if (nodeIdx >= v.count) {
    return;
  }
  const file_node_t *n = &v.nodes[nodeIdx];

  WCHAR buf[1024];

  if (di->item.mask & LVIF_TEXT) {
    WCHAR *dst = di->item.pszText;
    int cch = di->item.cchTextMax;
    if (dst == NULL || cch <= 1) {
      return;
    }
    if (iSub == 0) {
      Utf8BasenameToWideBuf(n->path, buf, ARRAYSIZE(buf));
      wcsncpy(dst, buf, (size_t)cch - 1);
      dst[(size_t)cch - 1] = L'\0';
    } else if (iSub == 1) {
      FormatSizeW(n->size_bytes, buf, ARRAYSIZE(buf));
      wcsncpy(dst, buf, (size_t)cch - 1);
      dst[(size_t)cch - 1] = L'\0';
    } else if (iSub == 2) {
      const WCHAR *t = WideType(n->attributes);
      wcsncpy(dst, t, (size_t)cch - 1);
      dst[(size_t)cch - 1] = L'\0';
    }
  }
}

static int CountToLvInt(size_t n) { return (n > (size_t)INT_MAX) ? INT_MAX : (int)n; }

static void SyncVirtualListCount(AppState *app) {
  if (!app->hwndList) {
    return;
  }
  const size_t vis = app->filterActive ? app->filteredCount : app->masterCount;
  ListView_SetItemCountEx(app->hwndList, CountToLvInt(vis),
                          LVSICF_NOINVALIDATEALL);
  InvalidateRect(app->hwndList, NULL, TRUE);
}

static void SetScanDoneStatus(AppState *app) {
  if (!app->hwndStatus || !app->scan) {
    return;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  WCHAR line[1024];

  scan_results_view_t v = scan_get_results(app->scan);
  unsigned long long ntot = v.count <= (size_t)ULLONG_MAX ? (unsigned long long)v.count : 0;
  unsigned long long nshow =
      app->filterActive ? (unsigned long long)app->filteredCount : ntot;

  if (app->filterActive && app->filterBuildRunning) {
    swprintf(line, ARRAYSIZE(line),
             L"Filtering… showing %llu (scan %llu)%s.",
             (unsigned long long)(app->filteredCount),
             (unsigned long long)(app->filterScanPos <= app->masterCount ? app->filterScanPos : app->masterCount),
             pr.is_cancel_observed ? L", cancelled scan" : L"");
  } else if (app->filterActive) {
    swprintf(line, ARRAYSIZE(line),
             L"Showing %llu of %llu (by size)%s — %llu bytes accounted.",
             nshow,
             ntot,
             pr.is_cancel_observed ? L", cancelled" : L"",
             (unsigned long long)pr.bytes_accounted);
  } else {
    swprintf(line, ARRAYSIZE(line),
             L"Done — %llu entries (by size)%s — %llu bytes accounted.",
             ntot,
             pr.is_cancel_observed ? L", cancelled" : L"",
             (unsigned long long)pr.bytes_accounted);
  }
  SendMessageW(app->hwndStatus, WM_SETTEXT, 0, (LPARAM)line);
}

static BOOL EnsureFilteredCapacity(AppState *app) {
  if (app->filteredCap >= app->masterCount &&
      app->filteredIndices != NULL) {
    return TRUE;
  }
  size_t nc = app->masterCount > 256 ? app->masterCount : 256;
  size_t *nb = (size_t *)realloc(app->filteredIndices, nc * sizeof(size_t));
  if (nb == NULL) {
    return FALSE;
  }
  app->filteredIndices = nb;
  app->filteredCap = nc;
  return TRUE;
}

static void ApplySearchFilter(HWND hwnd, AppState *app) {
  KillTimer(hwnd, DA_TIMER_SEARCH_DEBOUNCE);

  if (!app->hwndSearch || !app->scan) {
    return;
  }

  GetWindowTextW(app->hwndSearch, app->filterText, ARRAYSIZE(app->filterText));
  app->filterText[ARRAYSIZE(app->filterText) - 1] = L'\0';

  const BOOL want = (app->filterText[0] != L'\0');

  KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);
  app->filterBuildRunning = FALSE;

  if (!app->listPopulated || app->masterCount == 0) {
    return;
  }

  app->filterActive = want;

  if (!want) {
    app->filteredCount = 0;
    app->filterScanPos = 0;
    SyncVirtualListCount(app);
    SetScanDoneStatus(app);
    return;
  }

  if (!EnsureFilteredCapacity(app)) {
    MessageBoxW(app->hwnd, L"Could not allocate filter buffer.", L"DiskAtlas",
                MB_ICONWARNING);
    app->filterActive = FALSE;
    SyncVirtualListCount(app);
    return;
  }

  app->filteredCount = 0;
  app->filterScanPos = 0;
  app->filterBuildRunning = TRUE;
  SyncVirtualListCount(app);
  SetScanDoneStatus(app);

  SetTimer(hwnd, DA_TIMER_FILTER_CHUNK, 12, NULL);
}

static void FilterChunkTimer(HWND hwnd, AppState *app) {
  if (!app->filterActive || !app->filterBuildRunning || !app->scan ||
      app->masterIndices == NULL || app->masterCount == 0 ||
      app->filteredIndices == NULL) {
    KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);
    app->filterBuildRunning = FALSE;
    SyncVirtualListCount(app);
    SetScanDoneStatus(app);
    return;
  }

  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL || v.count != app->masterCount) {
    KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);
    app->filterBuildRunning = FALSE;
    SetScanDoneStatus(app);
    return;
  }

  WCHAR nameBuf[4096];

  size_t scanned = 0;
  for (; app->filterScanPos < app->masterCount &&
         scanned < (size_t)DA_FILTER_BATCH;
       ++scanned, ++app->filterScanPos) {
    size_t nid = app->masterIndices[app->filterScanPos];
    Utf8BasenameToWideBuf(v.nodes[nid].path, nameBuf, ARRAYSIZE(nameBuf));
    if (!WideNameMatchesFilter(nameBuf, app->filterText)) {
      continue;
    }

    size_t nf = app->filteredCount;

    /** Grow filtered buffer when matches exceed capacity. */
    if (nf >= app->filteredCap) {
      size_t grow = app->filteredCap ? app->filteredCap * 2u : app->masterCount;
      if (grow < nf + 1) {
        grow = nf + 1;
      }
      size_t *nb = (size_t *)realloc(app->filteredIndices, grow * sizeof(size_t));
      if (nb == NULL) {
        KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);
        app->filterBuildRunning = FALSE;
        SyncVirtualListCount(app);
        SetScanDoneStatus(app);
        MessageBoxW(app->hwnd, L"Out of memory while filtering.", L"DiskAtlas",
                    MB_ICONWARNING);
        return;
      }
      app->filteredIndices = nb;
      app->filteredCap = grow;
    }
    app->filteredIndices[nf] = nid;
    app->filteredCount = nf + 1;
  }

  ListView_SetItemCountEx(app->hwndList, CountToLvInt(app->filteredCount),
                          LVSICF_NOINVALIDATEALL);
  InvalidateRect(app->hwndList, NULL, TRUE);
  SetScanDoneStatus(app);

  if (app->filterScanPos >= app->masterCount) {
    KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);
    app->filterBuildRunning = FALSE;
    SetScanDoneStatus(app);
  }
}

/** Clears list and starts DA_TIMER_FILL (sort phase, then virtual SetItemCount). */
static void BeginPopulateList(HWND hwnd, AppState *app) {
  scan_results_view_t v = scan_get_results(app->scan);

  KillTimer(hwnd, DA_TIMER_FILL);
  KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);

  free(app->masterIndices);
  app->masterIndices = NULL;
  app->masterCount = 0;

  free(app->filteredIndices);
  app->filteredIndices = NULL;
  app->filteredCap = 0;
  app->filteredCount = 0;
  app->filterScanPos = 0;
  app->filterActive = FALSE;
  app->filterBuildRunning = FALSE;

  ListView_SetItemCountEx(app->hwndList, 0, LVSICF_NOINVALIDATEALL);

  app->populateTotal = 0;

  if (v.nodes == NULL || v.count == 0) {
    app->listPopulated = TRUE;
    EnableScanButton(app, TRUE);
    return;
  }

  app->populateTotal = v.count;
  SendMessage(app->hwndList, WM_SETREDRAW, FALSE, 0);
  SetTimer(hwnd, DA_TIMER_FILL, 15, NULL);
}

static void ListFillTimerChunk(HWND hwnd, AppState *app) {
  if (!app->scan || app->populateTotal == 0) {
    KillTimer(hwnd, DA_TIMER_FILL);
    return;
  }

  scan_results_view_t v = scan_get_results(app->scan);

  if (app->masterIndices == NULL) {
    if (v.nodes == NULL || v.count != app->populateTotal) {
      KillTimer(hwnd, DA_TIMER_FILL);
      SendMessage(app->hwndList, WM_SETREDRAW, TRUE, 0);
      InvalidateRect(app->hwndList, NULL, TRUE);
      app->listPopulated = TRUE;
      EnableScanButton(app, TRUE);
      return;
    }
    SendMessageW(app->hwndStatus, WM_SETTEXT, 0,
                 (LPARAM)L"Sorting entries by size…");
    size_t *indices = (size_t *)calloc(v.count, sizeof(size_t));
    if (!indices) {
      KillTimer(hwnd, DA_TIMER_FILL);
      SendMessage(app->hwndList, WM_SETREDRAW, TRUE, 0);
      InvalidateRect(app->hwndList, NULL, TRUE);
      SendMessageW(app->hwndStatus, WM_SETTEXT, 0,
                   (LPARAM)L"Could not allocate sort index.");
      MessageBoxW(hwnd,
                  L"Out of memory while preparing the sorted file list.", L"DiskAtlas",
                  MB_ICONWARNING);
      app->listPopulated = TRUE;
      EnableScanButton(app, TRUE);
      return;
    }
    for (size_t i = 0; i < v.count; ++i) {
      indices[i] = i;
    }
    da_qsort_nodes = v.nodes;
    qsort(indices, v.count, sizeof(size_t), CmpIndexBySizeDesc);
    da_qsort_nodes = NULL;
    app->masterIndices = indices;
    app->masterCount = v.count;
    return;
  }

  if (v.nodes == NULL || v.count != app->populateTotal) {
    KillTimer(hwnd, DA_TIMER_FILL);
    free(app->masterIndices);
    app->masterIndices = NULL;
    app->masterCount = 0;
    SendMessage(app->hwndList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(app->hwndList, NULL, TRUE);
    SendMessageW(app->hwndStatus, WM_SETTEXT, 0,
                 (LPARAM)L"Scan results changed during list build; list partial or empty.");
    app->listPopulated = TRUE;
    EnableScanButton(app, TRUE);
    return;
  }

  SendMessage(app->hwndList, WM_SETREDRAW, TRUE, 0);
  ListView_SetItemCountEx(app->hwndList, CountToLvInt(app->masterCount),
                          LVSICF_NOINVALIDATEALL);
  InvalidateRect(app->hwndList, NULL, TRUE);
  KillTimer(hwnd, DA_TIMER_FILL);

  app->listPopulated = TRUE;
  SetScanDoneStatus(app);
  EnableScanButton(app, TRUE);

  if (app->hwndSearch != NULL) {
    WCHAR peek[ARRAYSIZE(app->filterText)];
    if (GetWindowTextW(app->hwndSearch, peek, ARRAYSIZE(peek)) > 0 &&
        peek[0] != L'\0') {
      ApplySearchFilter(hwnd, app);
    }
  }
}

/** Clears list/filter state and previous result; starts a new scan. Returns FALSE if busy or scan_start fails. */
static BOOL StartScan(HWND hwnd, AppState *app) {
  if (app == NULL || app->scan_root_utf8 == NULL) {
    return FALSE;
  }

  if (app->scan != NULL) {
    scan_progress_t prev = scan_get_progress(app->scan);
    if (!prev.is_complete) {
      MessageBoxW(hwnd, L"A scan is already in progress.", L"DiskAtlas",
                  MB_ICONINFORMATION);
      return FALSE;
    }
  }

  KillTimer(hwnd, DA_TIMER_SCAN);
  KillTimer(hwnd, DA_TIMER_FILL);
  KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);
  KillTimer(hwnd, DA_TIMER_SEARCH_DEBOUNCE);

  if (app->hwndList != NULL) {
    ListView_SetItemCountEx(app->hwndList, 0, LVSICF_NOINVALIDATEALL);
    InvalidateRect(app->hwndList, NULL, TRUE);
  }

  free(app->masterIndices);
  app->masterIndices = NULL;
  app->masterCount = 0;
  free(app->filteredIndices);
  app->filteredIndices = NULL;
  app->filteredCap = 0;
  app->filteredCount = 0;
  app->filterScanPos = 0;
  app->filterActive = FALSE;
  app->filterBuildRunning = FALSE;
  app->populateTotal = 0;
  app->listPopulated = FALSE;

  if (app->scan != NULL) {
    scan_result_free(app->scan);
    app->scan = NULL;
  }

  app->scan = scan_start(app->scan_root_utf8, NULL);
  if (app->scan == NULL) {
    MessageBoxW(hwnd, L"scan_start failed (path or allocator).", L"DiskAtlas",
                MB_ICONERROR);
    EnableScanButton(app, TRUE);
    SendMessageW(app->hwndStatus, WM_SETTEXT, 0,
                 (LPARAM)L"Could not start scan.");
    return FALSE;
  }

  EnableScanButton(app, FALSE);
  SetTimer(hwnd, DA_TIMER_SCAN, 120, NULL);
  SendMessageW(app->hwndStatus, WM_SETTEXT, 0, (LPARAM)L"Starting scan…");
  return TRUE;
}

static AppState *AppFromHwnd(HWND hwnd) {
  LONG_PTR ptr = GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  return ptr ? (AppState *)(void *)ptr : NULL;
}

static void LayoutClients(HWND hwnd) {
  AppState *app = AppFromHwnd(hwnd);
  if (!app || !app->hwndScan || !app->hwndSearch || !app->hwndList || !app->hwndStatus) {
    return;
  }

  RECT rc;
  GetClientRect(hwnd, &rc);

  const int margin = 4;
  const int searchH = 24;
  const int gap = 6;
  const int btnW = 88;
  const int rowGap = 4;
  const int statusH = 22;
  const int cw = rc.right - rc.left;
  const int totalH = rc.bottom - rc.top;

  const int btnX = margin;
  const int searchX = btnX + btnW + gap;
  const int searchW =
      cw - margin - searchX > 48 ? cw - margin - searchX : (cw > searchX + 48 ? 48 : 0);

  if (totalH < searchH + rowGap + statusH + 8) {
    MoveWindow(app->hwndScan, btnX, margin, btnW, searchH, TRUE);
    MoveWindow(app->hwndSearch, searchX, margin, searchW > 0 ? searchW : (cw - searchX),
               searchH, TRUE);
    MoveWindow(app->hwndList, 0, margin + searchH + gap, cw, 0, TRUE);
    MoveWindow(app->hwndStatus, 0, totalH - statusH, cw, statusH, TRUE);
    return;
  }

  int listTop = margin + searchH + gap;
  int listH = totalH - listTop - statusH;
  if (listH < 0) {
    listH = 0;
  }

  MoveWindow(app->hwndScan, btnX, margin, btnW, searchH, TRUE);
  MoveWindow(app->hwndSearch, searchX, margin,
             searchW > 0 ? searchW : (cw > searchX + margin ? cw - searchX - margin : 48),
             searchH, TRUE);
  MoveWindow(app->hwndList, 0, listTop, cw, listH, TRUE);
  MoveWindow(app->hwndStatus, 0, listTop + listH, cw, statusH, TRUE);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  AppState *app = AppFromHwnd(hwnd);

  switch (msg) {
  case WM_CREATE: {
    CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
    app = (AppState *)cs->lpCreateParams;
    app->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)(void *)app);

    INITCOMMONCONTROLSEX icc;
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    RECT rc;
    GetClientRect(hwnd, &rc);

    const int btnW = 88;
    const int gap = 6;

    HWND hScan =
        CreateWindowExW(0, L"BUTTON", L"Scan",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 4, 4,
                        btnW, 24, hwnd, (HMENU)(UINT_PTR)DA_IDC_SCAN,
                        GetModuleHandleW(NULL), NULL);

    HWND hSearch =
        CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 96, 4,
            rc.right - rc.left - 96 - 4, 24, hwnd, (HMENU)(UINT_PTR)DA_IDC_SEARCH,
            GetModuleHandleW(NULL), NULL);

    HWND hLv =
        CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER | LVS_REPORT |
                            LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_OWNERDATA,
                        0, 36, 720, 400, hwnd, (HMENU)(UINT_PTR)DA_IDC_LISTVIEW,
                        GetModuleHandleW(NULL), NULL);

    HWND hStat =
        CreateWindowExW(0, L"STATIC", L"",
                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_SIMPLE, 0, 0,
                        rc.right - rc.left, 22, hwnd, (HMENU)(UINT_PTR)DA_IDC_STATUS,
                        GetModuleHandleW(NULL), NULL);

    app->hwndScan = hScan;
    app->hwndSearch = hSearch;
    app->hwndList = hLv;
    app->hwndStatus = hStat;

    SendMessage(hLv, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    WCHAR *colTitles[] = {L"Name", L"Size", L"Type"};
    int widths[] = {460, 120, 100};
    for (int ci = 0; ci < 3; ++ci) {
      LVCOLUMNW c;
      memset(&c, 0, sizeof(c));
      c.mask = LVCF_FMT | LVCF_WIDTH | LVCF_SUBITEM | LVCF_TEXT;
      c.fmt = (ci == 1) ? LVCFMT_RIGHT : LVCFMT_LEFT;
      c.cx = widths[ci];
      c.iSubItem = ci;
      c.pszText = colTitles[ci];
      c.cchTextMax = (int)wcslen(colTitles[ci]);
      ListView_InsertColumn(hLv, ci, &c);
    }

    HFONT guiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(hScan, WM_SETFONT, (WPARAM)guiFont, TRUE);
    SendMessageW(hSearch, WM_SETFONT, (WPARAM)guiFont, TRUE);
    SendMessageW(hLv, WM_SETFONT, (WPARAM)guiFont, TRUE);
    SendMessageW(hStat, WM_SETFONT, (WPARAM)guiFont, TRUE);

    LayoutClients(hwnd);

    SendMessageW(hStat, WM_SETTEXT, 0, (LPARAM)L"Click Scan to analyze the configured folder.");

    return 0;
  }

  case WM_SIZE:
    LayoutClients(hwnd);
    return 0;

  case WM_COMMAND:
    if (app != NULL && HIWORD(wParam) == BN_CLICKED &&
        LOWORD(wParam) == DA_IDC_SCAN) {
      StartScan(hwnd, app);
      return 0;
    }
    if (app != NULL && HIWORD(wParam) == EN_CHANGE &&
        LOWORD(wParam) == DA_IDC_SEARCH) {
      KillTimer(hwnd, DA_TIMER_SEARCH_DEBOUNCE);
      SetTimer(hwnd, DA_TIMER_SEARCH_DEBOUNCE, DA_SEARCH_DEBOUNCE_MS, NULL);
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);

  case WM_NOTIFY:
    if (app != NULL && lParam != 0) {
      LPNMHDR nh = (LPNMHDR)lParam;
      if (nh->idFrom == DA_IDC_LISTVIEW && nh->code == LVN_GETDISPINFOW) {
        ListDispInfo(app->hwndList, lParam, app);
        return 0;
      }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);

  case WM_TIMER: {
    if (!app) {
      return 0;
    }

    if (wParam == (WPARAM)DA_TIMER_SEARCH_DEBOUNCE) {
      KillTimer(hwnd, DA_TIMER_SEARCH_DEBOUNCE);
      ApplySearchFilter(hwnd, app);
      return 0;
    }

    if (wParam == (WPARAM)DA_TIMER_FILTER_CHUNK) {
      FilterChunkTimer(hwnd, app);
      return 0;
    }

    if (wParam == (WPARAM)DA_TIMER_FILL) {
      ListFillTimerChunk(hwnd, app);
      return 0;
    }

    if (!app->scan) {
      return 0;
    }

    if (wParam != (WPARAM)DA_TIMER_SCAN) {
      return 0;
    }

    scan_progress_t p = scan_get_progress(app->scan);

    if (!p.is_complete) {
      WCHAR line[768];
      swprintf(line, ARRAYSIZE(line),
               L"Scanning… %llu bytes — %llu visits",
               (unsigned long long)p.bytes_accounted,
               (unsigned long long)p.entry_count_visits);
      SendMessageW(app->hwndStatus, WM_SETTEXT, 0, (LPARAM)line);
      return 0;
    }

    WCHAR doneLine[768];
    swprintf(doneLine, ARRAYSIZE(doneLine),
             L"Complete — %llu bytes, %llu visits%s",
             (unsigned long long)p.bytes_accounted,
             (unsigned long long)p.entry_count_visits,
             p.is_cancel_observed ? L" (cancel)" : L"");
    SendMessageW(app->hwndStatus, WM_SETTEXT, 0, (LPARAM)doneLine);

    KillTimer(hwnd, DA_TIMER_SCAN);

    if (!app->listPopulated) {
      BeginPopulateList(hwnd, app);
    }

    return 0;
  }

  case WM_DESTROY: {
    if (app) {
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);

      KillTimer(hwnd, DA_TIMER_SCAN);
      KillTimer(hwnd, DA_TIMER_FILL);
      KillTimer(hwnd, DA_TIMER_SEARCH_DEBOUNCE);
      KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);
      free(app->masterIndices);
      app->masterIndices = NULL;
      free(app->filteredIndices);
      app->filteredIndices = NULL;
      scan_result_free(app->scan);
      app->scan = NULL;
      free(app->scan_root_utf8);
      app->scan_root_utf8 = NULL;
      free(app);
    }
    PostQuitMessage(0);
    return 0;
  }

  default:
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE prev, LPSTR cmd_line, int show) {
  (void)prev;
  (void)cmd_line;

  SetProcessDPIAware();

  if (diskatlas_init() != 0) {
    MessageBoxW(NULL, L"diskatlas_init failed", L"DiskAtlas", MB_ICONERROR);
    return 1;
  }

  int argc = 0;
  LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  const wchar_t *rootW =
      (argv && argc > 1 && argv[1] != NULL && argv[1][0] != L'\0') ? argv[1] : NULL;

  wchar_t profileFallback[MAX_PATH * 4];
  if (rootW == NULL || rootW[0] == L'\0') {
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", profileFallback, ARRAYSIZE(profileFallback));
    if (n > 0 && n < ARRAYSIZE(profileFallback)) {
      rootW = profileFallback;
    } else {
      rootW = L".";
    }
  }

  char *root_utf8 = DupWideToUtf8(rootW);
  LocalFree(argv);
  argv = NULL;

  if (!root_utf8) {
    MessageBoxW(NULL, L"Could not convert scan path to UTF-8.", L"DiskAtlas", MB_ICONERROR);
    return 1;
  }

  AppState *app = (AppState *)calloc(1, sizeof(AppState));
  if (!app) {
    free(root_utf8);
    return 1;
  }

  app->scan_root_utf8 = root_utf8;

  INITCOMMONCONTROLSEX iccx;
  iccx.dwSize = sizeof(iccx);
  iccx.dwICC =
      ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES | ICC_BAR_CLASSES;
  InitCommonControlsEx(&iccx);

  WNDCLASSEXW wc;
  ZeroMemory(&wc, sizeof(wc));
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = MainWndProc;
  wc.hInstance = hi;
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(ULONG_PTR)(COLOR_WINDOW + 1);
  wc.lpszClassName = L"DiskAtlasWndClass";

  ATOM atm = RegisterClassExW(&wc);
  if (atm == 0) {
    DWORD err = GetLastError();
    if (err != ERROR_CLASS_ALREADY_EXISTS) {
      free(app->scan_root_utf8);
      free(app);
      return 1;
    }
  }

  HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES | WS_EX_CLIENTEDGE, L"DiskAtlasWndClass", L"DiskAtlas",
                             WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 960,
                             620, NULL, NULL, hi, app);

  if (!hwnd) {
    free(app->scan_root_utf8);
    free(app);
    MessageBoxW(NULL, L"CreateWindow failed.", L"DiskAtlas", MB_ICONERROR);
    return 1;
  }

  ShowWindow(hwnd, show);
  UpdateWindow(hwnd);

  MSG mesg;
  while (GetMessageW(&mesg, NULL, 0, 0) > 0) {
    TranslateMessage(&mesg);
    DispatchMessageW(&mesg);
  }

  return (int)mesg.wParam;
}
