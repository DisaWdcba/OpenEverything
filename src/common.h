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
#define FSCTL_CREATE_USN_JOURNAL         CTL_CODE(FILE_DEVICE_FILE_SYSTEM, 57,  METHOD_NEITHER,   FILE_ANY_ACCESS)
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

/* Search types */
#define SEARCH_MAX_RESULTS               100000
#define SEARCH_CHAR_SLOT_COUNT           40

/* Entry string ownership flags */
#define ENTRY_STRING_NAME_POOLED         0x01
#define ENTRY_STRING_PATH_POOLED         0x02
#define ENTRY_STRING_EXTENSION_POOLED    0x04
#define ENTRY_STRING_FOLDED_NAME_POOLED  0x08

/* UI constants */
#define WC_EVERYTHING                    L"OPENEVERYTHING"
#define IDC_SEARCH_EDIT                  10007
#define IDC_LISTVIEW                     10020
#define IDC_STATUS_BAR                   10021

#define WM_SEARCH_UPDATE                 (WM_USER + 100)
#define WM_INDEX_PROGRESS                (WM_USER + 101)
#define WM_INDEX_DONE                    (WM_USER + 102)
#define WM_REFRESH                       (WM_USER + 103)
#define WM_SEARCH_DONE                   (WM_USER + 104)
#define WM_INDEX_SYNCED                  (WM_USER + 105)
#define WM_CACHE_LOADED                  (WM_USER + 106)

/* Column indices */
#define COL_NAME                         0
#define COL_PATH                         1
#define COL_SIZE                         2
#define COL_DATE_MODIFIED                3
#define COL_DATE_CREATED                 4
#define COL_ATTRIBUTES                   5

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
#define IDM_INDEX_UPDATE                 10050
#define IDM_INDEX_REBUILD                10051
#define IDM_HELP_ABOUT                   10060
#define ID_TOOLBAR_REFRESH               11001

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
    long long MaximumSize;
    long long AllocationDelta;
} CREATE_USN_JOURNAL_BUF;

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

/* Index entry structure */
typedef struct {
    wchar_t *name;
    long long usn;
    wchar_t *path;
    wchar_t *extension;
    wchar_t *folded_name;
    unsigned int string_flags;
    unsigned long long name_char_mask;
    unsigned long long path_char_mask;
    long long size;
    long long creation_time;
    long long modification_time;
    long long access_time;
    unsigned int attributes;
    long long file_ref;
    long long parent_ref;
    int is_directory;
    int volume_index;
    int metadata_loaded;
} INDEX_ENTRY;

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

/* Search query */
typedef struct {
    wchar_t text[512];
    wchar_t folded_text[512];
    int text_len;
    int folded_ready;
    unsigned long long char_mask;
    int match_case;
    int match_whole_word;
    int match_path;
    int use_regex;
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
    int *name_char_index_pool;
    int *name_char_indices[SEARCH_CHAR_SLOT_COUNT];
    int name_char_counts[SEARCH_CHAR_SLOT_COUNT];
    int name_char_index_ready;
    unsigned long long *ref_index_keys;
    int *ref_index_values;
    int ref_index_capacity;
    int ref_index_ready;
    volatile LONG index_revision;
    void *entry_string_pool;
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
} APP_STATE;

extern APP_STATE g_app;

#endif /* EVERYTHING_COMMON_H */
