#ifndef EVERYTHING_COMMON_H
#define EVERYTHING_COMMON_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <uxtheme.h>

/* =============================================================
 * NTFS IOCTL codes
 * ============================================================= */
#define FILE_DEVICE_FILE_SYSTEM          0x00000009
#define METHOD_BUFFERED                  0
#define METHOD_NEITHER                   3
#define FILE_ANY_ACCESS                  0
#define CTL_CODE(DeviceType, Function, Method, Access) \
    (((DeviceType) << 16) | ((Access) << 14) | ((Function) << 2) | (Method))

#define FSCTL_GET_NTFS_VOLUME_DATA       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 25,  METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_GET_NTFS_FILE_RECORD       CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 26,  METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_ENUM_USN_DATA              CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 44,  METHOD_NEITHER,   FILE_ANY_ACCESS)
#define FSCTL_QUERY_USN_JOURNAL          CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 61,  METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_READ_USN_JOURNAL           CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 46,  METHOD_NEITHER,   FILE_ANY_ACCESS)
#define FSCTL_READ_FILE_USN_DATA         CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 59,  METHOD_NEITHER,   FILE_ANY_ACCESS)
#define FSCTL_DELETE_USN_JOURNAL         CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 62,  METHOD_BUFFERED, FILE_ANY_ACCESS)
#define FSCTL_IS_VOLUME_MOUNTED          CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 10,  METHOD_BUFFERED, FILE_ANY_ACCESS)

/* USN Reason flags */
#define USN_REASON_DATA_OVERWRITE        0x00000001
#define USN_REASON_DATA_EXTEND           0x00000002
#define USN_REASON_DATA_TRUNCATION       0x00000004
#define USN_REASON_NAMED_DATA_OVERWRITE  0x00000010
#define USN_REASON_NAMED_DATA_EXTEND     0x00000020
#define USN_REASON_NAMED_DATA_TRUNCATION 0x00000040
#define USN_REASON_FILE_CREATE           0x00000100
#define USN_REASON_FILE_DELETE           0x00000200
#define USN_REASON_EA_CHANGE             0x00000400
#define USN_REASON_SECURITY_CHANGE       0x00000800
#define USN_REASON_RENAME_OLD_NAME       0x00001000
#define USN_REASON_RENAME_NEW_NAME       0x00002000
#define USN_REASON_INDEXABLE_CHANGE      0x00004000
#define USN_REASON_BASIC_INFO_CHANGE     0x00008000
#define USN_REASON_CLOSE                 0x80000000

/* NTFS attribute types */
#define ATTR_STANDARD_INFORMATION        0x10
#define ATTR_ATTRIBUTE_LIST              0x20
#define ATTR_FILE_NAME                   0x30
#define ATTR_OBJECT_ID                   0x40
#define ATTR_SECURITY_DESCRIPTOR         0x50
#define ATTR_VOLUME_NAME                 0x60
#define ATTR_VOLUME_INFORMATION          0x70
#define ATTR_DATA                        0x80
#define ATTR_INDEX_ROOT                  0x90
#define ATTR_INDEX_ALLOCATION            0xA0
#define ATTR_BITMAP                      0xB0
#define ATTR_REPARSE_POINT               0xC0
#define ATTR_EA_INFORMATION              0xD0
#define ATTR_EA                          0xE0

/* FILE record flags */
#define FR_IN_USE                        0x0001
#define FR_DIRECTORY                     0x0002

/* MFT record 5 is the NTFS volume root directory. */
#define NTFS_ROOT_FRN                    5

/* Search types */
#define SEARCH_MAX_RESULTS               100000
/* Slots 0..SEARCH_CHAR_SLOT_COUNT-1 are also backed by the inverted name
   index. The temporary build mask is 64 bits wide, so the remaining slots
   exist only as filter bits -- see index_char_mask_slot. */
#define SEARCH_CHAR_SLOT_COUNT           40
#define SEARCH_MASK_EXTRA_SLOTS          24
#define SEARCH_FOLDER_SCOPE_MAX          1024

#define INDEX_NAME_OFFSET_NONE           0xFFFFFFFFu

/* UI constants */
#define WC_EVERYTHING                    L"OPENEVERYTHING"
#define IDC_SEARCH_EDIT                  10007
#define IDC_LISTVIEW                     10020
#define IDC_STATUS_BAR                   10021
#define IDC_FOLDER_TREE                  10022
#define IDC_FILTER_LIST                  10023
#define IDC_SUBFOLDERS                   10024

#define WM_SEARCH_UPDATE                 (WM_USER + 100)
#define WM_INDEX_PROGRESS                (WM_USER + 101)
#define WM_INDEX_DONE                    (WM_USER + 102)
#define WM_REFRESH                       (WM_USER + 103)
#define WM_SEARCH_DONE                   (WM_USER + 104)
#define WM_INDEX_SYNCED                  (WM_USER + 105)
#define WM_CACHE_LOADED                  (WM_USER + 106)
#define WM_METADATA_READY                (WM_USER + 107)
#define WM_FOLDER_ENUM_READY             (WM_USER + 108)
#define WM_CACHE_UPGRADE_DONE            (WM_USER + 109)

/* WM_INDEX_PROGRESS values. Compatibility scan percentages are encoded above
   100 so the UI can distinguish the slow per-record fallback from bulk MFT
   progress without introducing another message type. */
#define INDEX_PROGRESS_FALLBACK_BASE     1000
#define INDEX_PROGRESS_SORTING           (-1)
#define INDEX_PROGRESS_BUILDING          (-2)
#define INDEX_PROGRESS_SAVING            (-3)

/* Column indices */
#define COL_NAME                         0
#define COL_PATH                         1
#define COL_SIZE                         2
#define COL_DATE_MODIFIED                3
#define COL_DATE_CREATED                 4
#define COL_ATTRIBUTES                   5
#define COL_EXTENSION                    6

/* Menu command IDs */
#define IDM_FILE_EXIT                    10001
#define IDM_EDIT_COPY                    10010
#define IDM_EDIT_COPY_PATH               10011
#define IDM_EDIT_SELECT_ALL              10012
#define IDM_VIEW_MATCH_CASE              10020
#define IDM_VIEW_MATCH_WHOLE_WORD        10021
#define IDM_VIEW_MATCH_PATH              10022
#define IDM_VIEW_USE_REGEX               10023
#define IDM_VIEW_REFRESH                 10030
#define IDM_VIEW_SORT_NAME               10040
#define IDM_VIEW_SORT_PATH               10041
#define IDM_VIEW_SORT_SIZE               10042
#define IDM_VIEW_SORT_DATE_MODIFIED      10043
#define IDM_VIEW_SORT_DATE_CREATED       10044
#define IDM_VIEW_SORT_ATTRIBUTES         10045
#define IDM_VIEW_FOLDERS                 10046
#define IDM_VIEW_FILTERS                 10047
#define IDM_VIEW_THEME_LIGHT             10090
#define IDM_VIEW_THEME_DARK              10091
#define IDM_VIEW_THEME_SYSTEM            10092
#define IDM_INDEX_UPDATE                 10050
#define IDM_INDEX_REBUILD                10051
#define IDM_HELP_ABOUT                   10060
#define ID_TOOLBAR_REFRESH               11001

typedef enum {
    FILTER_EVERYTHING = 0,
    FILTER_AUDIO,
    FILTER_COMPRESSED,
    FILTER_DOCUMENT,
    FILTER_EXECUTABLE,
    FILTER_FOLDER,
    FILTER_IMAGE,
    FILTER_VIDEO,
    FILTER_COUNT
} FILTER_TYPE;

typedef enum {
    THEME_SYSTEM = 0,
    THEME_LIGHT,
    THEME_DARK
} THEME_MODE;

typedef enum {
    PANEL_DOCK_LEFT = 0,
    PANEL_DOCK_RIGHT
} PANEL_DOCK_SIDE;

/* IPC constants */
#define IPC_PIPE_NAME                   L"\\\\.\\pipe\\OpenEverything"

/* Window dimensions */
#define DEFAULT_WINDOW_WIDTH             1000
#define DEFAULT_WINDOW_HEIGHT            700

/* =============================================================
 * NTFS raw structures (not in winioctl.h - parsed from MFT records)
 * ============================================================= */

#pragma pack(push, 1)

typedef struct {
    unsigned int Magic;
    unsigned short UpdateSequenceOffset;
    unsigned short UpdateSequenceSize;
    long long LogFileSequenceNumber;
    unsigned short SequenceNumber;
    unsigned short HardLinkCount;
    unsigned short FirstAttributeOffset;
    unsigned short Flags;
    unsigned int RealSize;
    unsigned int AllocatedSize;
    long long BaseRecordFileReference;
    unsigned short NextAttributeNumber;
} FILE_RECORD_HEADER;

typedef struct {
    unsigned int Type;
    unsigned int Length;
    unsigned char NonResident;
    unsigned char NameLength;
    unsigned short NameOffset;
    unsigned short Flags;
    unsigned short AttributeNumber;
} ATTR_HEADER;

typedef struct {
    unsigned int ValueLength;
    unsigned short ValueOffset;
    unsigned char IndexedFlag;
    unsigned char Reserved;       /* padding to 8 bytes */
} RESIDENT_ATTR;

typedef struct {
    long long StartingVCN;
    long long LastVCN;
    unsigned short DataRunOffset;
    unsigned short CompressionUnitSize;
    unsigned int padding;
    long long AllocatedLength;
    long long DataSize;
    long long InitializedSize;
} NON_RESIDENT_ATTR;

typedef struct {
    long long CreationTime;
    long long ModificationTime;
    long long MftChangeTime;
    long long AccessTime;
    unsigned int FileAttributes;
    unsigned int MaxVersions;
    unsigned int Version;
    unsigned int ClassId;
    unsigned int OwnerId;
    unsigned int SecurityId;
    long long Quota;
    long long Usn;
} STANDARD_INFORMATION;

/* FILE_NAME attribute structure - exactly 8-byte aligned like NTFS on disk */
/* MFT reference: 6 bytes (48-bit FRN + 16-bit sequence number) */
typedef struct {
    unsigned int ParentLow;       /* low 32 bits of parent FRN */
    unsigned short ParentHigh;    /* high 16 bits of parent FRN */
    unsigned short ParentSeq;     /* sequence number */
    long long CreationTime;
    long long ModificationTime;
    long long MftChangeTime;
    long long AccessTime;
    long long AllocatedSize;
    long long RealSize;
    unsigned int Flags;
    unsigned int EaSize;
    unsigned char NameLength;
    unsigned char NameType;
    /* wchar_t Name[NameLength] follows after this header */
} FILE_NAME_ATTR;

/* Map a character to a bit position in the temporary name-character mask.
 *
 * The indexer (building an entry's mask) and the search (building the query's
 * mask) MUST agree on this mapping exactly -- a mismatch makes the bloom test
 * `(entry_mask & query_mask) != query_mask` reject entries that actually match.
 * They used to keep private, subtly different copies, so it lives here as the
 * single definition.
 *
 * Non-ASCII folds into the high slots that the inverted index does not use.
 * Previously such characters returned -1, which left char_mask empty for CJK
 * queries and turned every one of them into an unfiltered full scan.
 */
static inline int index_char_mask_slot(wchar_t ch) {
    if (ch >= L'A' && ch <= L'Z')
        ch = (wchar_t)(ch + (L'a' - L'A'));

    if (ch >= L'a' && ch <= L'z') return (int)(ch - L'a');
    if (ch >= L'0' && ch <= L'9') return 26 + (int)(ch - L'0');
    if (ch == L'_') return 36;
    if (ch == L'-') return 37;
    if (ch == L'.') return 38;
    if (ch == L' ') return 39;
    if (ch >= 128) {
        wchar_t folded = (wchar_t)towlower(ch);
        return SEARCH_CHAR_SLOT_COUNT +
               (int)((unsigned int)folded % SEARCH_MASK_EXTRA_SLOTS);
    }
    return -1;
}

/* Convert 6-byte MFT reference to 64-bit FRN */
static inline long long mft_ref_to_frn(unsigned int low, unsigned short high) {
    return (long long)(((unsigned long long)high << 32) | low) & 0x0000FFFFFFFFFFFFLL;
}

/* Convert 6-byte MFT ref to full 64-bit (with sequence number) */  
static inline long long mft_ref_to_full(unsigned int low, unsigned short high, unsigned short seq) {
    return ((long long)seq << 48) | ((long long)high << 32) | low;
}

/* IOCTL input/output structures */
typedef struct {
    long long VolumeSerialNumber;
    long long NumberSectors;
    long long TotalClusters;
    long long FreeClusters;
    long long TotalReserved;
    unsigned int BytesPerSector;
    unsigned int BytesPerCluster;
    unsigned int BytesPerFileRecordSegment;
    unsigned int ClustersPerFileRecordSegment;
    long long MftValidDataLength;
    long long MftStartLcn;
    long long Mft2StartLcn;
    long long MftZoneStart;
    long long MftZoneEnd;
} NTFS_VOLUME_DATA_BUF;

typedef struct {
    long long UsnJournalId;
    long long FirstUsn;
    long long NextUsn;
    long long LowestValidUsn;
    long long MaxUsn;
    long long MaximumSize;
    long long AllocationDelta;
} USN_JOURNAL_DATA_BUF;

typedef struct {
    long long StartUsn;
    unsigned int ReasonMask;
    unsigned int ReturnOnlyOnClose;
    long long Timeout;
    long long BytesToWaitFor;
    long long UsnJournalId;
} READ_USN_JOURNAL_BUF;

typedef struct {
    long long StartFileReferenceNumber;
    long long LowUsn;
    long long HighUsn;
} MFT_ENUM_DATA_BUF;

typedef struct {
    unsigned int RecordLength;
    unsigned short MajorVersion;
    unsigned short MinorVersion;
    long long FileReferenceNumber;
    long long ParentFileReferenceNumber;
    long long Usn;
    long long TimeStamp;
    unsigned int Reason;
    unsigned int SourceInfo;
    unsigned int SecurityId;
    unsigned int FileAttributes;
    unsigned short FileNameLength;
    unsigned short FileNameOffset;
} USN_RECORD_BUF;

typedef struct {
    long long FileReferenceNumber;
    unsigned int FileRecordLength;
    unsigned char FileRecordBuffer[1];
} NTFS_FILE_RECORD_OUTPUT_BUF;

#pragma pack(pop)

typedef struct {
    /* Writable names live in data. A current cache can additionally provide a
     * read-only mapped prefix; offsets address the two regions as one logical
     * pool, so USN updates append without copying the mapped names. */
    char *data;
    size_t size;
    size_t capacity;
    const char *mapped_data;
    size_t mapped_size;
    void *mapped_view;
} INDEX_NAME_POOL;

/* Runtime entry. Names live in APP_STATE.name_pool as UTF-8 and are addressed
 * by a 32-bit offset. The layout remains 64 bytes on x64; the compact cache
 * representation saves memory by mapping names instead of copying them. */
typedef struct {
    long long size;
    long long creation_time;
    long long modification_time;
    long long file_ref;
    long long parent_ref;
    unsigned int name_offset;
    unsigned int attributes;
    int parent_index;
    unsigned short name_length;
    unsigned char filter_type;
    unsigned char is_directory;
    signed char volume_index;
    unsigned char metadata_loaded;
    unsigned char metadata_queued;
} INDEX_ENTRY;

typedef struct {
    INDEX_ENTRY *entries;
    int count;
    int capacity;
    INDEX_NAME_POOL names;
} INDEX_BUILD;

#define INDEX_PARENT_UNKNOWN (-1)

typedef struct {
    wchar_t *name;
    long long file_ref;
    long long parent_ref;
    long long usn;
    long long timestamp;
    unsigned int reason;
    unsigned int attributes;
    int is_directory;
    int volume_index;
} USN_CHANGE;

/* Volume info */
typedef struct {
    wchar_t drive_letter[4];    /* "C:\" */
    wchar_t volume_path[64];    /* "\\.\C:" */
    wchar_t label[64];
    int is_ntfs;
    int is_ready;
    long long total_size;
    long long free_size;
    long long usn_journal_id;
    long long usn_next_usn;
    long long usn_lowest_valid_usn;
} VOLUME_INFO;

/* Search query.
 *
 * The three `*_offset`/`has_*` fields below are derived from `text` once in
 * search_prepare_query. They used to be recomputed with wcsstr/wcschr inside
 * search_match_entry, i.e. four scans of the query string per indexed file.
 * Offsets rather than pointers so the struct stays safe to copy by value.
 */
typedef struct {
    wchar_t text[512];
    wchar_t folded_text[512];
    int text_len;
    int folded_ready;
    int ext_filter_offset;   /* index just past "ext:", or -1 */
    int has_folder_filter;
    int has_wildcard;
    unsigned long long char_mask;
    int match_case;
    int match_whole_word;
    int match_path;
    int use_regex;
    int filter_id;
    wchar_t folder_scope[SEARCH_FOLDER_SCOPE_MAX];
    int include_subfolders;
    int sort_column;
    int sort_ascending;
} SEARCH_QUERY;

/* Application state */
typedef struct {
    /* Window */
    HWND hwnd_main;
    HWND hwnd_search;
    HWND hwnd_list;
    HWND hwnd_status;
    HINSTANCE hinst;
    
    /* Index */
    INDEX_ENTRY *entries;
    int entry_count;
    int entry_capacity;
    int *filtered_indices;
    int filtered_count;
    int filtered_identity;
    int filtered_stale;
    int *name_char_index_pool;
    int *name_char_indices[SEARCH_CHAR_SLOT_COUNT];
    int name_char_counts[SEARCH_CHAR_SLOT_COUNT];
    int name_char_index_ready;
    int *filter_index_pool;
    int *filter_indices[FILTER_COUNT];
    int filter_counts[FILTER_COUNT];
    int filter_index_ready;
    int *ref_index_values;
    int ref_index_capacity;
    int ref_index_ready;
    volatile LONG index_revision;
    INDEX_NAME_POOL name_pool;
    size_t name_pool_live_size;
    int cache_loaded;
    CRITICAL_SECTION index_lock;
    
    /* Volumes */
    VOLUME_INFO volumes[26];
    int volume_count;
    int indexed_volume_count;
    int index_error_count;
    
    /* Search */
    SEARCH_QUERY query;
    int is_searching;
    volatile LONG search_generation;
    volatile LONG shutting_down;
    
    /* USN monitor */
    volatile LONG monitor_running;
    HANDLE monitor_thread;
    
    /* IPC */
    HANDLE ipc_pipe;
    
    /* Config */
    int match_case;
    int match_whole_word;
    int match_path;
    int use_regex;
    int close_to_tray;
    int minimize_to_tray;
    int column_width_name;
    int column_width_path;
    int column_width_size;
    int column_width_modified;
    int column_width_attributes;
    int column_width_extension;
    int show_folders;
    int show_filters;
    int theme_mode;
    int sidebar_width;
    int sidebar_split_percent;
    int folder_panel_width;
    int filter_panel_width;
    int folder_panel_side;
    int filter_panel_side;
    int selected_filter;
    int include_subfolders;
    wchar_t folder_scope[SEARCH_FOLDER_SCOPE_MAX];

    /* Window placement. Position is in physical screen pixels (it addresses a
       specific monitor); size is in 96-DPI logical units so it stays sensible
       across displays. window_width == 0 means "nothing saved yet". */
    int window_x;
    int window_y;
    int window_width;
    int window_height;
    int window_maximized;
    int sort_column;
    int sort_ascending;
} APP_STATE;

extern APP_STATE g_app;

#endif /* EVERYTHING_COMMON_H */
