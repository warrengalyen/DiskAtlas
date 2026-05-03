/*
 * DiskAtlas — Win32 GUI: column header + TreeView (duplicate groups expand in-place).
 */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <stdint.h>
#include <limits.h>

#include "diskatlas.h"

#ifndef ICC_HEADER_CLASS
#define ICC_HEADER_CLASS 0x00000020
#endif
#ifndef TVM_SETEXTENDEDSTYLE
#define TVM_SETEXTENDEDSTYLE (TV_FIRST + 44)
#endif
#ifndef TVS_EX_DOUBLEBUFFER
#define TVS_EX_DOUBLEBUFFER 0x0004
#endif

#define DA_IDC_FILETREE 1001
#define DA_IDC_STATUS 1002
#define DA_IDC_SEARCH 1003
#define DA_IDC_SCAN 1004
#define DA_IDC_CHKMATCHMTIME 1006
#define DA_IDC_COLHDR 1007
#define DA_IDC_DISPLAYMAX 1008
#define DA_IDC_LBLDISPLAYMAX 1009
/** Space reserved on the checkbox row for the “max list” label + combo (right-aligned). */
#define DA_DISPLAYMAX_ROW_W 218
#define DA_TIMER_SCAN 1
#define DA_TIMER_FILL 2
#define DA_TIMER_SEARCH_DEBOUNCE 3
#define DA_TIMER_FILTER_CHUNK 4
#define DA_TIMER_TREEINSERT 5
#define DA_TREEINSERT_BATCH 960
#define DA_TREEINSERT_MS 22
/** Rows per WM_TIMER tick (filter pass over master indices, keeps pump responsive). */
#define DA_FILTER_BATCH 4000
#define DA_SEARCH_DEBOUNCE_MS 200
#define DA_COL_COUNT 4
#define DA_COLHDR_H 22

#ifndef ARRAYSIZE
#define ARRAYSIZE(a) ((int)(sizeof(a) / sizeof((a)[0])))
#endif

typedef struct {
  HWND hwnd;
  HWND hwndScan;
  HWND hwndChkDupMtime;
  HWND hwndSearch;
  HWND hwndColHeader;
  HWND hwndFileTree;
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
  /** Batched root insertion into hwndFileTree (see DA_TIMER_TREEINSERT). */
  size_t treeInsertPos;
  uint8_t *dupGroupSeen;
  size_t dupGroupSeenCap;
  int colWidth[DA_COL_COUNT];
  HWND hwndLblDisplayMax;
  HWND hwndDisplayMaxCb;
  /** Top-N files shown at root when non-zero (matches combo); 0 = All. */
  size_t displayMaxEntries;
} AppState;

/** Keep displayMaxEntries in sync with hwndDisplayMaxCb (default 10000 = index 3). */
static void SyncDisplayMaxCapFromCombo(AppState *app) {
  if (app == NULL || app->hwndDisplayMaxCb == NULL) {
    return;
  }
  static const size_t caps[] = {0u, 100u, 1000u, 10000u, 100000u};
  const int ncaps = (int)(sizeof(caps) / sizeof(caps[0]));
  int ix = (int)SendMessageW(app->hwndDisplayMaxCb, CB_GETCURSEL, 0, 0);
  if (ix == CB_ERR || ix < 0 || ix >= ncaps) {
    ix = 3;
  }
  app->displayMaxEntries = caps[ix];
}

static void FileTree_Clear(AppState *app);
static void FileTree_BeginRootInsert(HWND hwnd, AppState *app);
static void FileTree_TreeInsertRedrawRelease(AppState *app);
static void FileTree_InsertRootsChunk(HWND hwnd, AppState *app);

static void EnableScanButton(AppState *app, BOOL enable) {
  if (app != NULL && app->hwndScan != NULL) {
    EnableWindow(app->hwndScan, enable);
  }
}

static void DiskAtlas_ApplyDpiAndShell(void) {
  typedef BOOL(WINAPI * SetProcessDpiAwarenessContextFn)(void *);
  HMODULE uh = GetModuleHandleW(L"user32.dll");
  void *perMonitorV2 = (void *)(intptr_t)-4; /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
  SetProcessDpiAwarenessContextFn pfn =
      uh ? (SetProcessDpiAwarenessContextFn)(void *)GetProcAddress(
               uh, "SetProcessDpiAwarenessContext")
         : NULL;
  if (pfn != NULL && pfn(perMonitorV2)) {
    return;
  }
  SetProcessDPIAware();
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

static void Utf8FullPathToWideTrunc(const char *path_utf8, WCHAR *out, int cchOut) {
  if (!out || cchOut <= 0) {
    return;
  }
  out[0] = L'\0';
  const char *p = (path_utf8 != NULL) ? path_utf8 : "";
  if (MultiByteToWideChar(CP_UTF8, 0, p, -1, out, cchOut) <= 0) {
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

static int FileTree_HeaderOffsetX(const AppState *app, int colIdx) {
  int x = 0;
  for (int i = 0; i < colIdx && i < DA_COL_COUNT; i++) {
    x += app->colWidth[i];
  }
  return x;
}

static void FileTree_HeaderInit(HWND hdr, AppState *app) {
  wchar_t *titles[] = {L"Name", L"Size", L"Type", L"Grp"};
  for (int i = 0; i < DA_COL_COUNT; i++) {
    HDITEMW hi;
    memset(&hi, 0, sizeof(hi));
    hi.mask = HDI_TEXT | HDI_WIDTH | HDI_FORMAT;
    hi.fmt = HDF_STRING | (i == 1 ? HDF_RIGHT : HDF_LEFT);
    hi.cxy = app->colWidth[i];
    hi.pszText = titles[i];
    hi.cchTextMax = (int)wcslen(titles[i]);
    Header_InsertItem(hdr, i, &hi);
  }
}

static void FileTree_LoadWidthsFromHeader(AppState *app) {
  HWND h = app->hwndColHeader;
  if (!h)
    return;
  for (int i = 0; i < DA_COL_COUNT; i++) {
    HDITEMW hi;
    memset(&hi, 0, sizeof(hi));
    hi.mask = HDI_WIDTH;
    if (Header_GetItem(h, i, &hi))
      app->colWidth[i] = hi.cxy;
  }
}

static void FileTree_PrepareDuringFilter(HWND hwnd, AppState *app) {
  KillTimer(hwnd, DA_TIMER_TREEINSERT);
  FileTree_TreeInsertRedrawRelease(app);
  FileTree_Clear(app);
  if (app->hwndFileTree != NULL) {
    InvalidateRect(app->hwndFileTree, NULL, TRUE);
  }
}

static void FileTree_RebuildRootsIfReady(HWND hwnd, AppState *app) {
  if (!app || !app->listPopulated || app->masterCount == 0 ||
      app->filterBuildRunning) {
    return;
  }
  FileTree_BeginRootInsert(hwnd, app);
}

static size_t FileTree_SourcePoolCount(const AppState *app) {
  if (app == NULL) {
    return 0;
  }
  return app->filterActive ? app->filteredCount : app->masterCount;
}

static size_t FileTree_SourceCount(const AppState *app) {
  if (app == NULL) {
    return 0;
  }
  size_t pool = FileTree_SourcePoolCount(app);
  size_t cap = app->displayMaxEntries;
  if (cap == 0) {
    return pool;
  }
  return pool < cap ? pool : cap;
}

static size_t FileTree_SourceAt(const AppState *app, size_t i) {
  if (app->filterActive) {
    return app->filteredIndices[i];
  }
  return app->masterIndices[i];
}

static void FileTree_FreeGroupSeen(AppState *app) {
  free(app->dupGroupSeen);
  app->dupGroupSeen = NULL;
  app->dupGroupSeenCap = 0;
}

static int FileTree_EnsureDupGroupSeen(AppState *app, uint32_t max_gid) {
  size_t need = (size_t)max_gid + 1u;
  if (need == 0) {
    need = 16;
  }
  if (app->dupGroupSeen != NULL && app->dupGroupSeenCap >= need) {
    memset(app->dupGroupSeen, 0, app->dupGroupSeenCap);
    return 0;
  }
  uint8_t *nb = (uint8_t *)realloc(app->dupGroupSeen, need);
  if (nb == NULL) {
    return -1;
  }
  app->dupGroupSeen = nb;
  app->dupGroupSeenCap = need;
  memset(app->dupGroupSeen, 0, need);
  return 0;
}

static void FileTree_Clear(AppState *app) {
  if (app == NULL) {
    return;
  }
  if (app->hwndFileTree != NULL) {
    TreeView_DeleteAllItems(app->hwndFileTree);
  }
  app->treeInsertPos = 0;
  FileTree_FreeGroupSeen(app);
}

static void FileTree_TreeInsertRedrawRelease(AppState *app) {
  if (app == NULL || app->hwndFileTree == NULL) {
    return;
  }
  SendMessageW(app->hwndFileTree, WM_SETREDRAW, TRUE, 0);
  RedrawWindow(app->hwndFileTree, NULL, NULL,
               RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

static void FileTree_InsertFileAt(HWND tv, HTREEITEM parent, size_t nodeIdx,
                                  const char *pathUtf8) {
  WCHAR name[512];
  Utf8BasenameToWideBuf(pathUtf8, name, ARRAYSIZE(name));
  TVINSERTSTRUCTW ti;
  memset(&ti, 0, sizeof(ti));
  ti.hParent = parent;
  ti.hInsertAfter = TVI_LAST;
  ti.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
  ti.item.cChildren = 0;
  ti.item.pszText = name;
  ti.item.lParam = (LONG_PTR)(nodeIdx + 1);
  TreeView_InsertItem(tv, &ti);
}

/** Full path label — used for duplicate members under an expanded group (same basename, different dirs). */
static void FileTree_InsertDupMemberAt(HWND tv, HTREEITEM parent, size_t nodeIdx,
                                       const char *pathUtf8) {
  WCHAR wpath[3072];
  Utf8FullPathToWideTrunc(pathUtf8, wpath, ARRAYSIZE(wpath));
  TVINSERTSTRUCTW ti;
  memset(&ti, 0, sizeof(ti));
  ti.hParent = parent;
  ti.hInsertAfter = TVI_LAST;
  ti.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
  ti.item.cChildren = 0;
  ti.item.pszText = wpath;
  ti.item.lParam = (LONG_PTR)(nodeIdx + 1);
  TreeView_InsertItem(tv, &ti);
}

static void FileTree_InsertDupGroupOriginalAtRoot(HWND tv, AppState *app, uint32_t gid) {
  size_t nmem = 0;
  const size_t *mp = diskatlas_dup_group_members(app->scan, gid, &nmem);
  scan_results_view_t v = scan_get_results(app->scan);
  if (tv == NULL || mp == NULL || nmem < 2 || v.nodes == NULL || mp[0] >= v.count) {
    return;
  }

  WCHAR name[512];
  Utf8BasenameToWideBuf(v.nodes[mp[0]].path, name, ARRAYSIZE(name));

  TVINSERTSTRUCTW ti;
  memset(&ti, 0, sizeof(ti));
  ti.hParent = TVI_ROOT;
  ti.hInsertAfter = TVI_LAST;
  ti.item.mask = TVIF_TEXT | TVIF_PARAM | TVIF_CHILDREN;
  ti.item.cChildren = 1;
  ti.item.pszText = name;
  ti.item.lParam = -(LONG_PTR)gid;
  TreeView_InsertItem(tv, &ti);
}

static void FileTree_ApplyHeaderItemWidths(AppState *app) {
  HWND h = app->hwndColHeader;
  if (h == NULL) {
    return;
  }
  for (int i = 0; i < DA_COL_COUNT; i++) {
    HDITEMW hi;
    memset(&hi, 0, sizeof(hi));
    hi.mask = HDI_WIDTH;
    hi.cxy = app->colWidth[i];
    Header_SetItem(h, i, &hi);
  }
}

static void FileTree_BeginRootInsert(HWND hwnd, AppState *app) {
  if (app == NULL || app->hwndFileTree == NULL || app->scan == NULL) {
    return;
  }
  KillTimer(hwnd, DA_TIMER_TREEINSERT);
  FileTree_TreeInsertRedrawRelease(app);
  FileTree_Clear(app);
  if (app->masterCount == 0) {
    InvalidateRect(app->hwndFileTree, NULL, TRUE);
    return;
  }

  if (!app->filterActive) {
    uint32_t mg = diskatlas_dup_max_group_id(app->scan);
    if (FileTree_EnsureDupGroupSeen(app, mg) != 0) {
      MessageBoxW(app->hwnd,
                  L"Could not allocate duplicate-group state for the tree.",
                  L"DiskAtlas", MB_ICONWARNING);
      RedrawWindow(app->hwndFileTree, NULL, NULL,
                   RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
      return;
    }
  }

  app->treeInsertPos = 0;
  if (!SetTimer(hwnd, DA_TIMER_TREEINSERT, DA_TREEINSERT_MS, NULL)) {
    SendMessageW(app->hwndFileTree, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(app->hwndFileTree, NULL, NULL,
                 RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    MessageBoxW(app->hwnd, L"Could not schedule tree build (SetTimer failed).", L"DiskAtlas",
                MB_ICONWARNING);
    return;
  }
  /** Run first chunk immediately so the list is not blank until the first WM_TIMER. */
  FileTree_InsertRootsChunk(hwnd, app);
}

static void FileTree_InsertRootsChunk(HWND hwnd, AppState *app) {
  HWND tv = app->hwndFileTree;
  scan_results_view_t v = scan_get_results(app->scan);
  if (tv == NULL || v.nodes == NULL) {
    KillTimer(hwnd, DA_TIMER_TREEINSERT);
    FileTree_TreeInsertRedrawRelease(app);
    return;
  }

  size_t total = FileTree_SourceCount(app);
  int batch = 0;

  while (app->treeInsertPos < total && batch < DA_TREEINSERT_BATCH) {
    size_t nid = FileTree_SourceAt(app, app->treeInsertPos++);
    if (nid >= v.count) {
      batch++;
      continue;
    }
    const file_node_t *n = &v.nodes[nid];
    batch++;

    if (app->filterActive || n->duplicate_group_id == DISKATLAS_DUPLICATE_GROUP_NONE) {
      FileTree_InsertFileAt(tv, TVI_ROOT, nid, n->path);
      continue;
    }

    uint32_t gid = n->duplicate_group_id;
    if (gid >= app->dupGroupSeenCap) {
      FileTree_InsertFileAt(tv, TVI_ROOT, nid, n->path);
      continue;
    }
    if (app->dupGroupSeen[gid]) {
      continue;
    }

    size_t mc = diskatlas_dup_group_member_count(app->scan, gid);
    if (mc < 2) {
      FileTree_InsertFileAt(tv, TVI_ROOT, nid, n->path);
      continue;
    }

    app->dupGroupSeen[gid] = 1;
    FileTree_InsertDupGroupOriginalAtRoot(tv, app, gid);
  }

  if (app->treeInsertPos >= total) {
    KillTimer(hwnd, DA_TIMER_TREEINSERT);
    FileTree_TreeInsertRedrawRelease(app);
  }
}

static LRESULT FileTree_OnItemExpanding(HWND hwnd, AppState *app, NMTREEVIEWW *nm) {
  (void)hwnd;
  if (nm == NULL || nm->action != TVE_EXPAND || nm->itemNew.hItem == NULL ||
      app->hwndFileTree == NULL || nm->hdr.hwndFrom != app->hwndFileTree ||
      app->scan == NULL) {
    return 0;
  }

  TVITEMW get;
  memset(&get, 0, sizeof(get));
  get.hItem = nm->itemNew.hItem;
  get.mask = TVIF_PARAM | TVIF_HANDLE;
  if (!TreeView_GetItem(app->hwndFileTree, &get)) {
    return 0;
  }
  LONG_PTR lp = get.lParam;
  if (lp >= 0) {
    return 0;
  }
  if (TreeView_GetChild(app->hwndFileTree, nm->itemNew.hItem) != NULL) {
    return 0;
  }

  uint32_t gid = (uint32_t)(-lp);
  size_t nmem = 0;
  const size_t *mp = diskatlas_dup_group_members(app->scan, gid, &nmem);
  scan_results_view_t v = scan_get_results(app->scan);
  if (mp == NULL || nmem == 0 || v.nodes == NULL) {
    return 0;
  }

  /** Children are duplicate paths only (index 0 is the root's canonical/original copy). */
  size_t dupChildCount = (nmem > 1) ? (nmem - 1) : 0;
  HWND wtv = app->hwndFileTree;
  BOOL holdPaint = dupChildCount >= 48;
  if (holdPaint && wtv != NULL) {
    SendMessageW(wtv, WM_SETREDRAW, FALSE, 0);
  }
  for (size_t k = 1; k < nmem; k++) {
    size_t ni = mp[k];
    if (ni >= v.count) {
      continue;
    }
    FileTree_InsertDupMemberAt(app->hwndFileTree, nm->itemNew.hItem, ni, v.nodes[ni].path);
  }
  if (holdPaint && wtv != NULL) {
    SendMessageW(wtv, WM_SETREDRAW, TRUE, 0);
    RedrawWindow(wtv, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
  }
  return 0;
}

static void FileTree_DrawRowColumns(AppState *app, HDC hdc, LONG_PTR lp, RECT *rcRow) {
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    return;
  }

  RECT rcTv;
  GetClientRect(app->hwndFileTree, &rcTv);
  int colsRight = rcTv.right > 12 ? rcTv.right - 4 : rcRow->right;

  SetBkMode(hdc, TRANSPARENT);

  int xSizeL = FileTree_HeaderOffsetX(app, 1);
  int xTypeL = FileTree_HeaderOffsetX(app, 2);
  int xGrpL = FileTree_HeaderOffsetX(app, 3);
  RECT rSz, rTy, rGr;

  if (lp < 0) {
    uint32_t gid = (uint32_t)(-lp);
    size_t nmem = 0;
    const size_t *mp = diskatlas_dup_group_members(app->scan, gid, &nmem);
    if (mp == NULL || nmem == 0 || mp[0] >= v.count) {
      return;
    }
    const file_node_t *n = &v.nodes[mp[0]];
    WCHAR sz[96], ty[64], gr[32];
    FormatSizeW(n->size_bytes, sz, ARRAYSIZE(sz));
    wcsncpy(ty, WideType(n->attributes), (size_t)ARRAYSIZE(ty) - 1u);
    ty[ARRAYSIZE(ty) - 1] = L'\0';
    swprintf(gr, ARRAYSIZE(gr), L"%u", (unsigned int)gid);

    int typR = xGrpL - 4;
    if (typR <= xTypeL)
      typR = colsRight;

    SetRect(&rSz, xSizeL, rcRow->top, xSizeL + app->colWidth[1], rcRow->bottom);
    SetRect(&rTy, xTypeL, rcRow->top, typR, rcRow->bottom);
    SetRect(&rGr, xGrpL, rcRow->top, colsRight, rcRow->bottom);
    DrawTextW(hdc, sz, -1, &rSz, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(hdc, ty, -1, &rTy, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(hdc, gr, -1, &rGr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    return;
  }

  if (lp <= 0) {
    return;
  }
  size_t ni = (size_t)(lp - 1);
  if (ni >= v.count) {
    return;
  }
  const file_node_t *n = &v.nodes[ni];
  WCHAR sz[96], ty[64], gr[32];
  FormatSizeW(n->size_bytes, sz, ARRAYSIZE(sz));
  wcsncpy(ty, WideType(n->attributes), (size_t)ARRAYSIZE(ty) - 1u);
  ty[ARRAYSIZE(ty) - 1] = L'\0';
  if (n->duplicate_group_id != 0u) {
    swprintf(gr, ARRAYSIZE(gr), L"%u", (unsigned int)n->duplicate_group_id);
  } else {
    gr[0] = L'\0';
  }

  int typR = xGrpL - 4;
  if (typR <= xTypeL)
    typR = colsRight;

  SetRect(&rSz, xSizeL, rcRow->top, xSizeL + app->colWidth[1], rcRow->bottom);
  SetRect(&rTy, xTypeL, rcRow->top, typR, rcRow->bottom);
  SetRect(&rGr, xGrpL, rcRow->top, colsRight, rcRow->bottom);
  DrawTextW(hdc, sz, -1, &rSz, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
  DrawTextW(hdc, ty, -1, &rTy, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
  DrawTextW(hdc, gr, -1, &rGr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

/**
 * Builds the sameWide label the tree displays (basename at root vs full path under a dup group)
 * without calling TreeView_GetItem TVIF_TEXT (expensive on huge result sets during paint).
 */
static BOOL FileTree_BuildLabelForEllipsis(HWND tv, AppState *app, HTREEITEM hItem,
                                           LONG_PTR lp, WCHAR *dst, int dstCch) {
  if (tv == NULL || app == NULL || dst == NULL || dstCch < 8) {
    return FALSE;
  }
  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL) {
    return FALSE;
  }
  dst[0] = L'\0';

  if (lp < 0) {
    uint32_t gid = (uint32_t)(-lp);
    size_t nmem = 0;
    const size_t *mp = diskatlas_dup_group_members(app->scan, gid, &nmem);
    if (mp == NULL || nmem == 0 || mp[0] >= v.count) {
      return FALSE;
    }
    Utf8BasenameToWideBuf(v.nodes[mp[0]].path, dst, dstCch);
    return dst[0] != L'\0';
  }

  if (lp <= 0) {
    return FALSE;
  }
  size_t ni = (size_t)(lp - 1);
  if (ni >= v.count) {
    return FALSE;
  }

  BOOL fullPathDupChild = FALSE;
  HTREEITEM parent = TreeView_GetNextItem(tv, hItem, TVGN_PARENT);
  if (parent != NULL) {
    TVITEMW tp;
    memset(&tp, 0, sizeof(tp));
    tp.hItem = parent;
    tp.mask = TVIF_PARAM;
    if (TreeView_GetItem(tv, &tp) && tp.lParam < 0) {
      fullPathDupChild = TRUE;
    }
  }
  if (fullPathDupChild) {
    Utf8FullPathToWideTrunc(v.nodes[ni].path, dst, dstCch);
  } else {
    Utf8BasenameToWideBuf(v.nodes[ni].path, dst, dstCch);
  }
  return dst[0] != L'\0';
}

/** After TreeView paints the label, repaint the name band with ellipsis so text stays within header column 0. */
static void FileTree_OverpaintEllipsizedName(AppState *app, NMTVCUSTOMDRAW *tvcd,
                                             HTREEITEM hItem, LONG_PTR lp,
                                             const RECT *rcRow) {
  HWND tv = app->hwndFileTree;
  HDC hdc = tvcd->nmcd.hdc;
  if (tv == NULL || hdc == NULL || rcRow == NULL) {
    return;
  }

  int xLim = FileTree_HeaderOffsetX(app, 1) - 4;
  RECT rcTv;
  GetClientRect(tv, &rcTv);
  if (xLim > rcTv.right) {
    xLim = rcTv.right;
  }
  RECT rcClipClient;
  SetRect(&rcClipClient, rcTv.left, rcRow->top - 2, rcTv.right, rcRow->bottom + 2);

  RECT rcText;
  if (!TreeView_GetItemRect(tv, hItem, &rcText, TRUE)) {
    return;
  }

  RECT rcErase;
  SetRect(&rcErase, rcText.left, rcText.top, xLim, rcText.bottom);
  if (!IntersectRect(&rcErase, &rcErase, &rcClipClient)) {
    return;
  }
  if (rcErase.right <= rcErase.left || rcErase.bottom <= rcErase.top ||
      rcErase.left >= xLim) {
    return;
  }

  WCHAR buf[4112];
  if (!FileTree_BuildLabelForEllipsis(tv, app, hItem, lp, buf, ARRAYSIZE(buf))) {
    return;
  }

  UINT st = tvcd->nmcd.uItemState;
  COLORREF bk = tvcd->clrTextBk;
  COLORREF tx = tvcd->clrText;

  if ((st & CDIS_SELECTED) != 0) {
    bk = GetSysColor(COLOR_HIGHLIGHT);
    tx = GetSysColor(COLOR_HIGHLIGHTTEXT);
  } else if ((st & CDIS_GRAYED) != 0) {
    if (bk == CLR_NONE || bk == (COLORREF)0xFFFFFFFF) {
      bk = GetSysColor(COLOR_BTNFACE);
    }
    if (tx == CLR_NONE || tx == (COLORREF)0xFFFFFFFF) {
      tx = GetSysColor(COLOR_GRAYTEXT);
    }
  } else {
    if (bk == CLR_NONE || bk == (COLORREF)0xFFFFFFFF) {
      bk = GetSysColor(COLOR_WINDOW);
    }
    if (tx == CLR_NONE || tx == (COLORREF)0xFFFFFFFF) {
      tx = GetSysColor(COLOR_WINDOWTEXT);
    }
  }

  HFONT hf = (HFONT)SendMessageW(tv, WM_GETFONT, 0, 0);
  HFONT hfOld = hf ? (HFONT)SelectObject(hdc, hf) : NULL;

  SetDCBrushColor(hdc, bk);
  FillRect(hdc, &rcErase, (HBRUSH)GetStockObject(DC_BRUSH));

  SetBkMode(hdc, TRANSPARENT);
  SetTextColor(hdc, tx);

  DrawTextW(hdc, buf, -1, &rcErase,
            DT_LEFT | DT_NOPREFIX | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

  if (hfOld) {
    SelectObject(hdc, hfOld);
  }
}

static LRESULT FileTree_OnCustomDraw(AppState *app, NMTVCUSTOMDRAW *tvcd) {
  if (app == NULL || app->hwndFileTree == NULL) {
    return CDRF_DODEFAULT;
  }

  switch (tvcd->nmcd.dwDrawStage) {
  case CDDS_PREPAINT:
    return CDRF_NOTIFYITEMDRAW;
  case CDDS_ITEMPREPAINT:
    return CDRF_NOTIFYPOSTPAINT;
  case CDDS_ITEMPOSTPAINT: {
    HTREEITEM hItem = (HTREEITEM)(UINT_PTR)tvcd->nmcd.dwItemSpec;
    RECT rcRow = tvcd->nmcd.rc;
    if ((rcRow.right - rcRow.left) < 2) {
      if (!TreeView_GetItemRect(app->hwndFileTree, hItem, &rcRow, TRUE)) {
        return CDRF_DODEFAULT;
      }
    }
    TVITEMW tig;
    memset(&tig, 0, sizeof(tig));
    tig.hItem = hItem;
    tig.mask = TVIF_PARAM;
    if (!TreeView_GetItem(app->hwndFileTree, &tig)) {
      return CDRF_DODEFAULT;
    }
    FileTree_OverpaintEllipsizedName(app, tvcd, hItem, tig.lParam, &rcRow);
    FileTree_DrawRowColumns(app, tvcd->nmcd.hdc, tig.lParam, &rcRow);
    return CDRF_DODEFAULT;
  }
  default:
    return CDRF_DODEFAULT;
  }
}

/** Clears list and starts DA_TIMER_FILL (sort phase, then virtual SetItemCount). */
static void SetScanDoneStatus(AppState *app) {
  if (!app->hwndStatus || !app->scan) {
    return;
  }
  scan_progress_t pr = scan_get_progress(app->scan);
  WCHAR line[1024];

  scan_results_view_t v = scan_get_results(app->scan);
  unsigned long long ntot = v.count <= (size_t)ULLONG_MAX ? (unsigned long long)v.count : 0;
  size_t pool = FileTree_SourcePoolCount(app);
  size_t visible = FileTree_SourceCount(app);
  unsigned long long nshow_tree = visible <= (size_t)ULLONG_MAX ? (unsigned long long)visible : 0;
  unsigned long long npool_ll = pool <= (size_t)ULLONG_MAX ? (unsigned long long)pool : 0;
  BOOL capped = pool > visible;

  uint32_t dgc = diskatlas_dup_max_group_id(app->scan);

  if (app->filterActive && app->filterBuildRunning) {
    swprintf(line, ARRAYSIZE(line),
             L"Filtering… showing %llu (scan %llu)%s — dup groups %u.",
             (unsigned long long)(app->filteredCount),
             (unsigned long long)(app->filterScanPos <= app->masterCount ? app->filterScanPos : app->masterCount),
             pr.is_cancel_observed ? L", cancelled scan" : L"",
             (unsigned int)dgc);
  } else if (app->filterActive) {
    if (capped) {
      swprintf(line, ARRAYSIZE(line),
               L"Showing %llu of %llu matches at root (sorted by size; list cap)%s — %llu bytes — dup "
               L"groups %u.",
               nshow_tree,
               npool_ll,
               pr.is_cancel_observed ? L", cancelled" : L"",
               (unsigned long long)pr.bytes_accounted,
               (unsigned int)dgc);
    } else {
      swprintf(line, ARRAYSIZE(line),
               L"Showing %llu of %llu (by size)%s — %llu bytes — dup groups %u.",
               nshow_tree,
               ntot,
               pr.is_cancel_observed ? L", cancelled" : L"",
               (unsigned long long)pr.bytes_accounted,
               (unsigned int)dgc);
    }
  } else {
    if (capped) {
      swprintf(line, ARRAYSIZE(line),
               L"Done — showing %llu of %llu entries at root (list cap)%s — %llu bytes accounted — dup "
               L"groups %u.",
               nshow_tree,
               npool_ll,
               pr.is_cancel_observed ? L", cancelled" : L"",
               (unsigned long long)pr.bytes_accounted,
               (unsigned int)dgc);
    } else {
      swprintf(line, ARRAYSIZE(line),
               L"Done — %llu entries (by size)%s — %llu bytes accounted — dup groups %u.",
               ntot,
               pr.is_cancel_observed ? L", cancelled" : L"",
               (unsigned long long)pr.bytes_accounted,
               (unsigned int)dgc);
    }
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
    FileTree_RebuildRootsIfReady(hwnd, app);
    SetScanDoneStatus(app);
    return;
  }

  if (!EnsureFilteredCapacity(app)) {
    MessageBoxW(app->hwnd, L"Could not allocate filter buffer.", L"DiskAtlas",
                MB_ICONWARNING);
    app->filterActive = FALSE;
    FileTree_RebuildRootsIfReady(hwnd, app);
    return;
  }

  app->filteredCount = 0;
  app->filterScanPos = 0;
  app->filterBuildRunning = TRUE;
  FileTree_PrepareDuringFilter(hwnd, app);
  SetScanDoneStatus(app);

  SetTimer(hwnd, DA_TIMER_FILTER_CHUNK, 12, NULL);
}

static void FilterChunkTimer(HWND hwnd, AppState *app) {
  if (!app->filterActive || !app->filterBuildRunning || !app->scan ||
      app->masterIndices == NULL || app->masterCount == 0 ||
      app->filteredIndices == NULL) {
    KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);
    app->filterBuildRunning = FALSE;
    FileTree_RebuildRootsIfReady(hwnd, app);
    SetScanDoneStatus(app);
    return;
  }

  scan_results_view_t v = scan_get_results(app->scan);
  if (v.nodes == NULL || v.count != app->masterCount) {
    KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);
    app->filterBuildRunning = FALSE;
    SetScanDoneStatus(app);
    FileTree_RebuildRootsIfReady(hwnd, app);
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
        FileTree_RebuildRootsIfReady(hwnd, app);
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

  SetScanDoneStatus(app);

  if (app->filterScanPos >= app->masterCount) {
    KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);
    app->filterBuildRunning = FALSE;
    SetScanDoneStatus(app);
    FileTree_BeginRootInsert(hwnd, app);
  }
}

/** Clears sort buffer and starts DA_TIMER_FILL (sort phase); then batched tree roots. */
static void BeginPopulateList(HWND hwnd, AppState *app) {
  scan_results_view_t v = scan_get_results(app->scan);

  KillTimer(hwnd, DA_TIMER_FILL);
  KillTimer(hwnd, DA_TIMER_FILTER_CHUNK);
  KillTimer(hwnd, DA_TIMER_TREEINSERT);
  FileTree_TreeInsertRedrawRelease(app);
  FileTree_Clear(app);

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

  if (app->hwndFileTree != NULL) {
    InvalidateRect(app->hwndFileTree, NULL, TRUE);
  }

  app->populateTotal = 0;

  if (v.nodes == NULL || v.count == 0) {
    app->listPopulated = TRUE;
    EnableScanButton(app, TRUE);
    return;
  }

  app->populateTotal = v.count;
  if (app->hwndFileTree != NULL) {
    SendMessage(app->hwndFileTree, WM_SETREDRAW, FALSE, 0);
  }
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
      if (app->hwndFileTree != NULL) {
        SendMessage(app->hwndFileTree, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(app->hwndFileTree, NULL, TRUE);
      }
      app->listPopulated = TRUE;
      EnableScanButton(app, TRUE);
      return;
    }
    SendMessageW(app->hwndStatus, WM_SETTEXT, 0,
                 (LPARAM)L"Sorting entries by size…");
    size_t *indices = (size_t *)calloc(v.count, sizeof(size_t));
    if (!indices) {
      KillTimer(hwnd, DA_TIMER_FILL);
      if (app->hwndFileTree != NULL) {
        SendMessage(app->hwndFileTree, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(app->hwndFileTree, NULL, TRUE);
      }
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
    if (app->hwndFileTree != NULL) {
      SendMessage(app->hwndFileTree, WM_SETREDRAW, TRUE, 0);
      InvalidateRect(app->hwndFileTree, NULL, TRUE);
    }
    SendMessageW(app->hwndStatus, WM_SETTEXT, 0,
                 (LPARAM)L"Scan results changed during list build; list partial or empty.");
    app->listPopulated = TRUE;
    EnableScanButton(app, TRUE);
    return;
  }

  if (app->hwndFileTree != NULL) {
    SendMessage(app->hwndFileTree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(app->hwndFileTree, NULL, TRUE);
  }
  KillTimer(hwnd, DA_TIMER_FILL);

  app->listPopulated = TRUE;
  SetScanDoneStatus(app);
  EnableScanButton(app, TRUE);

  FileTree_BeginRootInsert(hwnd, app);

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
  KillTimer(hwnd, DA_TIMER_TREEINSERT);

  FileTree_TreeInsertRedrawRelease(app);
  FileTree_Clear(app);
  if (app->hwndFileTree != NULL) {
    InvalidateRect(app->hwndFileTree, NULL, TRUE);
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

  scan_options_t opt;
  memset(&opt, 0, sizeof(opt));
  opt.struct_version = DISKATLAS_SCAN_OPTIONS_STRUCT_VERSION;
  opt.flags = 0;
  opt.max_depth = 0;
  opt.io_threads = 0;
  if (app->hwndChkDupMtime != NULL &&
      SendMessageW(app->hwndChkDupMtime, BM_GETCHECK, 0, 0) == BST_CHECKED) {
    opt.flags |= DISKATLAS_SCAN_OPTION_DUPLICATE_USE_MTIME;
  }

  app->scan = scan_start(app->scan_root_utf8, &opt);
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
  if (!app || !app->hwndScan || !app->hwndChkDupMtime || !app->hwndSearch ||
      !app->hwndColHeader || !app->hwndFileTree || !app->hwndStatus ||
      !app->hwndLblDisplayMax || !app->hwndDisplayMaxCb) {
    return;
  }

  RECT rc;
  GetClientRect(hwnd, &rc);

  const int margin = 4;
  const int searchH = 24;
  const int chkRowH = 22;
  const int gap = 6;
  const int btnW = 84;
  const int rowGap = 4;
  const int statusH = 22;
  const int hdrGap = 2;
  const int cw = rc.right - rc.left;
  const int totalH = rc.bottom - rc.top;
  const int innerW = cw > margin * 2 ? cw - margin * 2 : cw;

  const int btnX = margin;
  const int searchX = btnX + btnW + gap;
  int searchW = cw - margin - searchX - DA_DISPLAYMAX_ROW_W;
  if (searchW < 96)
    searchW = cw > searchX + margin ? cw - margin - searchX : 96;

  if (totalH < searchH + chkRowH + statusH + DA_COLHDR_H + 16) {
    MoveWindow(app->hwndScan, btnX, margin, btnW, searchH, TRUE);
    MoveWindow(app->hwndSearch, searchX, margin, searchW > 0 ? searchW : (cw - searchX),
               searchH, TRUE);
    int listTopBelowChk = margin + searchH + 2 + chkRowH + 2;
    int chkW = cw - 2 * margin - DA_DISPLAYMAX_ROW_W - 8;
    if (chkW < 80)
      chkW = 80;
    MoveWindow(app->hwndChkDupMtime, margin, margin + searchH + 2, chkW, chkRowH, TRUE);
    int lblW = 50;
    int panelLeft =
        cw > margin + lblW + 80 ? cw - margin - DA_DISPLAYMAX_ROW_W : margin + chkW + 6;
    if (panelLeft + DA_DISPLAYMAX_ROW_W > cw - margin)
      panelLeft = (cw > margin + DA_DISPLAYMAX_ROW_W + 8)
                      ? cw - margin - DA_DISPLAYMAX_ROW_W
                      : margin;
    MoveWindow(app->hwndLblDisplayMax, panelLeft, margin + searchH + 2, lblW, chkRowH, TRUE);
    MoveWindow(app->hwndDisplayMaxCb, panelLeft + lblW, margin + searchH + 2,
               cw > panelLeft + lblW + margin ? cw - margin - panelLeft - lblW
                                              : cw - panelLeft - margin,
               chkRowH + 140, TRUE);
    int listTop = listTopBelowChk;
    int bodyH = totalH - listTop - statusH;
    if (bodyH < 0)
      bodyH = 0;
    int hdrH = bodyH > DA_COLHDR_H ? DA_COLHDR_H : bodyH;
    int treeY = listTop + hdrH + (hdrH > 0 ? hdrGap : 0);
    int treeH = totalH - treeY - statusH;
    if (treeH < 0)
      treeH = 0;
    MoveWindow(app->hwndColHeader, margin, listTop, innerW, hdrH, TRUE);
    MoveWindow(app->hwndFileTree, margin, treeY, innerW, treeH, TRUE);
    MoveWindow(app->hwndStatus, 0, totalH - statusH, cw, statusH, TRUE);
    return;
  }

  int row1Y = margin + searchH + rowGap;
  MoveWindow(app->hwndScan, btnX, margin, btnW, searchH, TRUE);
  MoveWindow(app->hwndSearch, searchX, margin,
             searchW > 0 ? searchW : (cw > searchX + margin ? cw - searchX - margin : 48),
             searchH, TRUE);
  int chkWfull = cw - 2 * margin - DA_DISPLAYMAX_ROW_W - 8;
  if (chkWfull < 140)
    chkWfull = 140;
  MoveWindow(app->hwndChkDupMtime, margin, row1Y, chkWfull, chkRowH, TRUE);

  const int lblW = 52;
  const int cbWmin = DA_DISPLAYMAX_ROW_W - lblW - 4;
  int panelLeft =
      cw >= margin + lblW + cbWmin + DA_DISPLAYMAX_ROW_W / 4
          ? cw - margin - DA_DISPLAYMAX_ROW_W
          : margin + chkWfull + gap;
  if (panelLeft + lblW + 70 > cw - margin)
    panelLeft =
        cw > DA_DISPLAYMAX_ROW_W + margin * 2 ? cw - margin - DA_DISPLAYMAX_ROW_W : margin;
  MoveWindow(app->hwndLblDisplayMax, panelLeft, row1Y, lblW, chkRowH, TRUE);
  MoveWindow(app->hwndDisplayMaxCb, panelLeft + lblW, row1Y,
             cw >= panelLeft + lblW + cbWmin + margin ? cw - margin - panelLeft - lblW
                                                      : cw - panelLeft - margin - 8,
             chkRowH + 160, TRUE);

  int listTop = row1Y + chkRowH + gap;
  int bodyH = totalH - listTop - statusH;
  if (bodyH < 0)
    bodyH = 0;
  int treeH = bodyH - DA_COLHDR_H - hdrGap;
  if (treeH < 0)
    treeH = 0;
  MoveWindow(app->hwndColHeader, margin, listTop, innerW, DA_COLHDR_H, TRUE);
  MoveWindow(app->hwndFileTree, margin, listTop + DA_COLHDR_H + hdrGap, innerW, treeH,
             TRUE);
  MoveWindow(app->hwndStatus, 0, listTop + bodyH, cw, statusH, TRUE);
}

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  AppState *app = AppFromHwnd(hwnd);

  switch (msg) {
  case WM_CREATE: {
    CREATESTRUCTW *cs = (CREATESTRUCTW *)lParam;
    app = (AppState *)cs->lpCreateParams;
    app->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)(void *)app);

    RECT rc;
    GetClientRect(hwnd, &rc);

    const int btnWc = 84;
    const int margin0 = 4;
    int innerW = rc.right - rc.left - margin0 * 2;
    if (innerW < 80) {
      innerW = rc.right - rc.left;
    }
    const int listTop0 = 62;

    HWND hScan =
        CreateWindowExW(0, L"BUTTON", L"Scan",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 4, 4,
                        btnWc, 24, hwnd, (HMENU)(UINT_PTR)DA_IDC_SCAN,
                        GetModuleHandleW(NULL), NULL);

    HWND hSearch =
        CreateWindowExW(
            WS_EX_STATICEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 96, 4,
            rc.right - rc.left - 96 - 4, 24, hwnd, (HMENU)(UINT_PTR)DA_IDC_SEARCH,
            GetModuleHandleW(NULL), NULL);

    HWND hChk = CreateWindowExW(
        0, L"BUTTON", L"Distinguish duplicates by file mtime",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
        8, 34, rc.right - rc.left - 16 - DA_DISPLAYMAX_ROW_W, 22, hwnd,
        (HMENU)(UINT_PTR)DA_IDC_CHKMATCHMTIME, GetModuleHandleW(NULL), NULL);

    HWND hLblMax =
        CreateWindowExW(0, L"STATIC", L"Max list:",
                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX, rc.right - 8 - DA_DISPLAYMAX_ROW_W,
                        34, 52, 22, hwnd, (HMENU)(UINT_PTR)DA_IDC_LBLDISPLAYMAX,
                        GetModuleHandleW(NULL), NULL);

    HWND hMaxCb =
        CreateWindowExW(0, L"COMBOBOX", L"",
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS |
                            WS_VSCROLL,
                        rc.right - 8 - DA_DISPLAYMAX_ROW_W + 54, 33,
                        DA_DISPLAYMAX_ROW_W - 54 - 4, 200, hwnd,
                        (HMENU)(UINT_PTR)DA_IDC_DISPLAYMAX, GetModuleHandleW(NULL), NULL);

    HWND hHdr =
        CreateWindowExW(0, WC_HEADERW, L"",
                        WS_CHILD | WS_VISIBLE | HDS_HOTTRACK | HDS_BUTTONS, margin0,
                        listTop0, innerW, DA_COLHDR_H, hwnd,
                        (HMENU)(UINT_PTR)DA_IDC_COLHDR, GetModuleHandleW(NULL), NULL);

    HWND hTree = CreateWindowExW(
        WS_EX_STATICEDGE, WC_TREEVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_LINESATROOT | TVS_HASLINES |
            TVS_HASBUTTONS | TVS_SHOWSELALWAYS | TVS_INFOTIP | TVS_FULLROWSELECT |
            TVS_TRACKSELECT,
        margin0, listTop0 + DA_COLHDR_H + 2, innerW, 200, hwnd,
        (HMENU)(UINT_PTR)DA_IDC_FILETREE, GetModuleHandleW(NULL), NULL);

    HWND hStat =
        CreateWindowExW(0, L"STATIC", L"",
                        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_SIMPLE, 0, 0,
                        rc.right - rc.left, 22, hwnd, (HMENU)(UINT_PTR)DA_IDC_STATUS,
                        GetModuleHandleW(NULL), NULL);

    app->hwndScan = hScan;
    app->hwndChkDupMtime = hChk;
    app->hwndSearch = hSearch;
    app->hwndColHeader = hHdr;
    app->hwndFileTree = hTree;
    app->hwndStatus = hStat;
    app->hwndLblDisplayMax = hLblMax;
    app->hwndDisplayMaxCb = hMaxCb;

    SendMessageW(hMaxCb, CB_ADDSTRING, 0, (LPARAM)L"All");
    SendMessageW(hMaxCb, CB_ADDSTRING, 0, (LPARAM)L"100");
    SendMessageW(hMaxCb, CB_ADDSTRING, 0, (LPARAM)L"1000");
    SendMessageW(hMaxCb, CB_ADDSTRING, 0, (LPARAM)L"10000");
    SendMessageW(hMaxCb, CB_ADDSTRING, 0, (LPARAM)L"100000");
    SendMessageW(hMaxCb, CB_SETCURSEL, 3, 0);
    SyncDisplayMaxCapFromCombo(app);

    int cw0[] = {360, 118, 94, 72};
    for (int ci = 0; ci < DA_COL_COUNT; ++ci) {
      app->colWidth[ci] = cw0[ci];
    }
    FileTree_HeaderInit(hHdr, app);

    SendMessageW(hTree, TVM_SETEXTENDEDSTYLE, TVS_EX_DOUBLEBUFFER,
                 TVS_EX_DOUBLEBUFFER);

    HFONT guiFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(hScan, WM_SETFONT, (WPARAM)guiFont, TRUE);
    SendMessageW(hChk, WM_SETFONT, (WPARAM)guiFont, TRUE);
    SendMessageW(hSearch, WM_SETFONT, (WPARAM)guiFont, TRUE);
    SendMessageW(hHdr, WM_SETFONT, (WPARAM)guiFont, TRUE);
    SendMessageW(hTree, WM_SETFONT, (WPARAM)guiFont, TRUE);
    SendMessageW(hStat, WM_SETFONT, (WPARAM)guiFont, TRUE);
    SendMessageW(hLblMax, WM_SETFONT, (WPARAM)guiFont, TRUE);
    SendMessageW(hMaxCb, WM_SETFONT, (WPARAM)guiFont, TRUE);

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
    if (app != NULL && HIWORD(wParam) == CBN_SELCHANGE &&
        LOWORD(wParam) == DA_IDC_DISPLAYMAX) {
      SyncDisplayMaxCapFromCombo(app);
      FileTree_RebuildRootsIfReady(hwnd, app);
      SetScanDoneStatus(app);
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);

  case WM_NOTIFY:
    if (app != NULL && lParam != 0) {
      LPNMHDR nh = (LPNMHDR)lParam;
      if (nh->idFrom == DA_IDC_FILETREE && app->hwndFileTree != NULL &&
          nh->hwndFrom == app->hwndFileTree) {
        if (nh->code == NM_CUSTOMDRAW) {
          return FileTree_OnCustomDraw(app, (NMTVCUSTOMDRAW *)lParam);
        }
        if (nh->code == TVN_ITEMEXPANDING) {
          FileTree_OnItemExpanding(hwnd, app, (NMTREEVIEWW *)lParam);
          return 0;
        }
      }
      if (nh->idFrom == DA_IDC_COLHDR && app->hwndColHeader != NULL &&
          nh->hwndFrom == app->hwndColHeader) {
        if (nh->code == HDN_ITEMCHANGED) {
          FileTree_LoadWidthsFromHeader(app);
          if (app->hwndFileTree != NULL) {
            InvalidateRect(app->hwndFileTree, NULL, TRUE);
          }
          return 0;
        }
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

    if (wParam == (WPARAM)DA_TIMER_TREEINSERT) {
      FileTree_InsertRootsChunk(hwnd, app);
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
      KillTimer(hwnd, DA_TIMER_TREEINSERT);
      free(app->masterIndices);
      app->masterIndices = NULL;
      free(app->filteredIndices);
      app->filteredIndices = NULL;
      free(app->dupGroupSeen);
      app->dupGroupSeen = NULL;
      app->dupGroupSeenCap = 0;
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

  DiskAtlas_ApplyDpiAndShell();

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
      ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_TAB_CLASSES |
      ICC_BAR_CLASSES | ICC_TREEVIEW_CLASSES | ICC_HEADER_CLASS;
  InitCommonControlsEx(&iccx);

  WNDCLASSEXW wc;
  ZeroMemory(&wc, sizeof(wc));
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = MainWndProc;
  wc.hInstance = hi;
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
  wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
  wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);
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
