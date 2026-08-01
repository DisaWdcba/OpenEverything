#include "ntfs.h"
#include "common.h"
#include "index.h"
#include <limits.h>
#include <stdint.h>

int ntfs_enumerate_volumes(VOLUME_INFO *volumes, int max_volumes)
{
    wchar_t drives[256];
    int count = 0;
    
    if (!GetLogicalDriveStringsW(255, drives))
        return 0;
    
    wchar_t *p = drives;
    while (*p && count < max_volumes) {
        UINT type = GetDriveTypeW(p);
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) {
            VOLUME_INFO *vol = &volumes[count];
            wcscpy_s(vol->drive_letter, 4, p);
            swprintf_s(vol->volume_path, 64, L"\\\\.\\%c:", p[0]);
            
            wchar_t fs[32];
            vol->is_ntfs = 0;
            vol->is_ready = 0;
            
            if (GetVolumeInformationW(p, vol->label, 64, NULL, NULL, NULL, fs, 32)) {
                vol->is_ready = 1;
                if (wcscmp(fs, L"NTFS") == 0)
                    vol->is_ntfs = 1;
            }
            
            ULARGE_INTEGER freeBytes, totalBytes;
            if (GetDiskFreeSpaceExW(p, &freeBytes, &totalBytes, NULL)) {
                vol->total_size = (long long)totalBytes.QuadPart;
                vol->free_size = (long long)freeBytes.QuadPart;
            }
            
            if (vol->is_ntfs && vol->is_ready)
                count++;
        }
        p += wcslen(p) + 1;
    }
    
    return count;
}

HANDLE ntfs_open_volume(const wchar_t *volume_path)
{
    HANDLE h = CreateFileW(
        volume_path,
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        0,
        NULL);
    
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    
    /* Verify volume is mounted */
    DWORD bytes;
    if (!DeviceIoControl(h, FSCTL_IS_VOLUME_MOUNTED, NULL, 0, NULL, 0, &bytes, NULL)) {
        if (GetLastError() != ERROR_INVALID_FUNCTION) {
            CloseHandle(h);
            return NULL;
        }
    }
    
    return h;
}

void ntfs_close_volume(HANDLE h)
{
    if (h && h != INVALID_HANDLE_VALUE)
        CloseHandle(h);
}

static long long ntfs_ref_to_frn(long long ref)
{
    return ref & 0x0000FFFFFFFFFFFFLL;
}

/* A single NTFS name component tops out at 255 characters. Enforcing it here
   keeps every downstream fixed buffer (notably the 512-char ListView cell)
   within range no matter what the journal hands us. */
#define NTFS_MAX_NAME_CHARS 255

/* Resolve a USN record's name and prove it lies inside the record. The kernel
   is the producer, but an out-of-range FileNameOffset would otherwise read
   past the IOCTL buffer, so bound it explicitly. */
static int ntfs_usn_record_name(const USN_RECORD_BUF *rec, unsigned int record_bytes,
                                const wchar_t **out_name, int *out_chars)
{
    unsigned int name_offset;
    unsigned int name_bytes;
    unsigned int chars;

    if (!rec || rec->MajorVersion != 2)
        return 0;

    name_offset = rec->FileNameOffset;
    name_bytes = rec->FileNameLength;

    if (name_bytes == 0 || (name_bytes % sizeof(wchar_t)) != 0)
        return 0;
    if (name_offset < sizeof(USN_RECORD_BUF) || name_offset > record_bytes)
        return 0;
    if (name_bytes > record_bytes - name_offset)
        return 0;

    chars = name_bytes / sizeof(wchar_t);
    if (chars == 0 || chars > NTFS_MAX_NAME_CHARS)
        return 0;

    *out_name = (const wchar_t *)((const char *)rec + name_offset);
    *out_chars = (int)chars;
    return 1;
}

static int ntfs_grow_entries(INDEX_BUILD *build, int needed)
{
    INDEX_ENTRY *new_entries;
    int new_cap;

    if (!build || needed < 0)
        return 0;
    if (needed <= build->capacity)
        return 1;
    if (needed > (int)(INT_MAX / sizeof(INDEX_ENTRY)))
        return 0;

    new_cap = build->capacity > 0 ? build->capacity : 65536;
    while (new_cap < needed) {
        if (new_cap > INT_MAX / 2)
            return 0;
        new_cap *= 2;
    }

    new_entries = (INDEX_ENTRY *)realloc(
        build->entries, (size_t)new_cap * sizeof(INDEX_ENTRY));
    if (!new_entries)
        return 0;

    memset(new_entries + build->capacity, 0,
           (size_t)(new_cap - build->capacity) * sizeof(INDEX_ENTRY));
    build->entries = new_entries;
    build->capacity = new_cap;
    return 1;
}

static int ntfs_fill_entry_from_name(INDEX_BUILD *build, INDEX_ENTRY *entry,
                                     const wchar_t *name, int name_len)
{
    if (!build || !entry || !name)
        return 0;
    if (name_len < 0)
        name_len = (int)wcslen(name);
    entry->name_offset = INDEX_NAME_OFFSET_NONE;
    entry->name_length = 0;
    return index_name_pool_append_wide(&build->names, name, (size_t)name_len,
                                       &entry->name_offset,
                                       &entry->name_length);
}

int ntfs_update_volume_usn_info(HANDLE hVolume, VOLUME_INFO *volume)
{
    USN_JOURNAL_DATA_BUF journal;
    
    if (!volume || !ntfs_query_usn_journal(hVolume, &journal))
        return 0;
    
    volume->usn_journal_id = journal.UsnJournalId;
    volume->usn_next_usn = journal.NextUsn;
    volume->usn_lowest_valid_usn = journal.LowestValidUsn;
    return 1;
}

int ntfs_read_usn_index(HANDLE hVolume, INDEX_BUILD *build,
                        int volume_index, HWND hwnd_progress)
{
    USN_JOURNAL_DATA_BUF journal = {0};
    MFT_ENUM_DATA_BUF enum_data = {0};
    DWORD bytes = 0;
    char *buffer = NULL;
    DWORD buffer_size = 4 * 1024 * 1024;
    int last_progress = 0;
    int ok = 0;

    if (!build)
        return 0;
    index_build_init(build);
    
    if (!ntfs_query_usn_journal(hVolume, &journal))
        return 0;
    
    buffer = (char *)malloc(buffer_size);
    if (!buffer || !ntfs_grow_entries(build, 65536)) {
        free(buffer);
        index_build_free(build);
        return 0;
    }
    
    enum_data.StartFileReferenceNumber = 0;
    enum_data.LowUsn = 0;
    enum_data.HighUsn = journal.NextUsn;
    
    for (;;) {
        BOOL ioctl_ok = DeviceIoControl(
            hVolume,
            FSCTL_ENUM_USN_DATA,
            &enum_data,
            sizeof(enum_data),
            buffer,
            buffer_size,
            &bytes,
            NULL);
        
        if (!ioctl_ok) {
            if (GetLastError() == ERROR_HANDLE_EOF)
                ok = 1;
            break;
        }
        
        if (bytes <= sizeof(long long))
            break;
        
        enum_data.StartFileReferenceNumber = *(long long *)buffer;
        
        DWORD offset = sizeof(long long);
        while (offset + sizeof(USN_RECORD_BUF) <= bytes) {
            USN_RECORD_BUF *rec = (USN_RECORD_BUF *)(buffer + offset);
            const wchar_t *name;
            int name_chars;

            if (rec->RecordLength < sizeof(USN_RECORD_BUF) ||
                offset + rec->RecordLength > bytes)
                break;

            if (ntfs_usn_record_name(rec, rec->RecordLength, &name, &name_chars) &&
                ntfs_grow_entries(build, build->count + 1)) {
                INDEX_ENTRY *e = &build->entries[build->count];

                /* Publish only once the name is owned. A NULL name here would
                   travel into the index and make every later cache save fail. */
                if (!ntfs_fill_entry_from_name(build, e, name, name_chars))
                    goto enum_done;

                e->size = 0;
                e->creation_time = 0;
                e->modification_time = 0;
                e->attributes = rec->FileAttributes;
                e->file_ref = ntfs_ref_to_frn(rec->FileReferenceNumber);
                e->parent_ref = ntfs_ref_to_frn(rec->ParentFileReferenceNumber);
                e->is_directory = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
                e->volume_index = (signed char)volume_index;
                e->parent_index = INDEX_PARENT_UNKNOWN;
                build->count++;
            }

            offset += rec->RecordLength;
        }

        /* Report on growth rather than on an exact multiple: batch sizes vary,
           so `entry_count % 65536 == 0` almost never hit and the UI sat silent
           for the whole enumeration. */
        if (hwnd_progress && build->count - last_progress >= 65536) {
            last_progress = build->count;
            PostMessageW(hwnd_progress, WM_INDEX_PROGRESS, (WPARAM)0, (LPARAM)volume_index);
        }
    }

enum_done:
    free(buffer);
    
    if (!ok && build->count == 0) {
        index_build_free(build);
        return 0;
    }
    return build->count;
}

static int ntfs_grow_changes(USN_CHANGE **changes, int *capacity, int needed)
{
    if (needed <= *capacity)
        return 1;
    
    if (needed > (int)(INT_MAX / sizeof(USN_CHANGE)))
        return 0;

    int new_cap = *capacity > 0 ? *capacity : 4096;
    while (new_cap < needed) {
        if (new_cap > INT_MAX / 2)
            return 0;
        new_cap *= 2;
    }

    USN_CHANGE *new_changes = (USN_CHANGE *)realloc(*changes, (size_t)new_cap * sizeof(USN_CHANGE));
    if (!new_changes)
        return 0;

    memset(new_changes + *capacity, 0, (size_t)(new_cap - *capacity) * sizeof(USN_CHANGE));
    *changes = new_changes;
    *capacity = new_cap;
    return 1;
}

static int ntfs_add_usn_change(USN_CHANGE **changes, int *count, int *capacity,
                               USN_RECORD_BUF *rec, int volume_index)
{
    const wchar_t *name_ptr;
    int name_chars;

    /* A record we cannot bound is skipped, not treated as a read failure. */
    if (!ntfs_usn_record_name(rec, rec->RecordLength, &name_ptr, &name_chars))
        return 1;

    if (!ntfs_grow_changes(changes, capacity, *count + 1))
        return 0;

    USN_CHANGE *change = &(*changes)[*count];
    memset(change, 0, sizeof(*change));

    change->name = (wchar_t *)calloc((size_t)name_chars + 1, sizeof(wchar_t));
    if (!change->name)
        return 0;

    memcpy(change->name, name_ptr, (size_t)name_chars * sizeof(wchar_t));
    change->name[name_chars] = L'\0';
    change->file_ref = ntfs_ref_to_frn(rec->FileReferenceNumber);
    change->parent_ref = ntfs_ref_to_frn(rec->ParentFileReferenceNumber);
    change->usn = rec->Usn;
    change->timestamp = rec->TimeStamp;
    change->reason = rec->Reason;
    change->attributes = rec->FileAttributes;
    change->is_directory = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 0;
    change->volume_index = volume_index;
    
    (*count)++;
    return 1;
}

int ntfs_read_usn_changes(HANDLE hVolume, long long start_usn, long long journal_id,
                          long long stop_usn, int volume_index,
                          USN_CHANGE **out_changes, int *out_count, long long *out_next_usn,
                          int max_changes)
{
    READ_USN_JOURNAL_BUF read;
    char *buffer = NULL;
    DWORD buffer_size = 1024 * 1024;
    USN_CHANGE *changes = NULL;
    int change_count = 0;
    int change_cap = 0;
    long long next_usn = start_usn;
    int ok = 0;
    
    if (!out_changes || !out_count || !out_next_usn)
        return 0;
    
    *out_changes = NULL;
    *out_count = 0;
    *out_next_usn = start_usn;
    
    buffer = (char *)malloc(buffer_size);
    if (!buffer)
        return 0;
    
    for (;;) {
        DWORD bytes = 0;
        memset(&read, 0, sizeof(read));
        read.StartUsn = next_usn;
        read.ReasonMask = USN_REASON_DATA_OVERWRITE |
                          USN_REASON_DATA_EXTEND |
                          USN_REASON_DATA_TRUNCATION |
                          USN_REASON_NAMED_DATA_OVERWRITE |
                          USN_REASON_NAMED_DATA_EXTEND |
                          USN_REASON_NAMED_DATA_TRUNCATION |
                          USN_REASON_FILE_CREATE |
                          USN_REASON_FILE_DELETE |
                          USN_REASON_RENAME_OLD_NAME |
                          USN_REASON_RENAME_NEW_NAME |
                          USN_REASON_SECURITY_CHANGE |
                          USN_REASON_INDEXABLE_CHANGE |
                          USN_REASON_BASIC_INFO_CHANGE |
                          USN_REASON_CLOSE;
        read.ReturnOnlyOnClose = 1;
        read.Timeout = 0;
        read.BytesToWaitFor = 0;
        read.UsnJournalId = journal_id;
        
        if (!DeviceIoControl(hVolume, FSCTL_READ_USN_JOURNAL,
                             &read, sizeof(read),
                             buffer, buffer_size, &bytes, NULL)) {
            if (GetLastError() == ERROR_HANDLE_EOF) {
                ok = 1;
                break;
            }
            ok = 0;
            break;
        }
        
        if (bytes < sizeof(long long)) {
            ok = 1;
            break;
        }
        
        long long returned_next_usn = *(long long *)buffer;
        unsigned int offset = sizeof(long long);
        
        while (offset + sizeof(USN_RECORD_BUF) <= bytes) {
            USN_RECORD_BUF *rec = (USN_RECORD_BUF *)(buffer + offset);
            if (rec->RecordLength < sizeof(USN_RECORD_BUF) ||
                offset + rec->RecordLength > bytes)
                break;

            if (!ntfs_add_usn_change(&changes, &change_count, &change_cap, rec, volume_index)) {
                ok = 0;
                goto done;
            }

            offset += rec->RecordLength;
        }
        
        if (returned_next_usn <= next_usn) {
            ok = 1;
            break;
        }
        
        next_usn = returned_next_usn;
        if (max_changes > 0 && change_count >= max_changes) {
            ok = 1;
            break;
        }
        if (stop_usn > 0 && next_usn >= stop_usn) {
            ok = 1;
            break;
        }
    }
    
done:
    free(buffer);
    
    if (!ok) {
        ntfs_free_usn_changes(changes, change_count);
        return 0;
    }
    
    *out_changes = changes;
    *out_count = change_count;
    *out_next_usn = next_usn;
    return 1;
}

void ntfs_free_usn_changes(USN_CHANGE *changes, int count)
{
    if (!changes)
        return;
    
    for (int i = 0; i < count; i++)
        free(changes[i].name);
    
    free(changes);
}

typedef struct {
    uint64_t lcn;
    uint64_t cluster_count;
} NTFS_MFT_RUN;

static int ntfs_read_exact_at(HANDLE hVolume, uint64_t offset,
                              void *buffer, DWORD size)
{
    LARGE_INTEGER position;
    DWORD total = 0;

    if (offset > INT64_MAX)
        return 0;
    position.QuadPart = (LONGLONG)offset;
    if (!SetFilePointerEx(hVolume, position, NULL, FILE_BEGIN))
        return 0;

    while (total < size) {
        DWORD got = 0;
        if (!ReadFile(hVolume, (unsigned char *)buffer + total,
                      size - total, &got, NULL) || got == 0) {
            return 0;
        }
        total += got;
    }
    return 1;
}

static int ntfs_fixup_raw_record(unsigned char *record, DWORD record_size,
                                 DWORD bytes_per_sector)
{
    FILE_RECORD_HEADER *header;
    unsigned short *usa;
    unsigned short signature;
    unsigned int usa_bytes;

    if (!record || record_size < sizeof(FILE_RECORD_HEADER) ||
        bytes_per_sector < 512 || bytes_per_sector > record_size ||
        record_size % bytes_per_sector != 0) {
        return 0;
    }

    header = (FILE_RECORD_HEADER *)record;
    if (header->Magic != 0x454C4946 || header->UpdateSequenceSize < 2)
        return 0;

    usa_bytes = (unsigned int)header->UpdateSequenceSize * sizeof(unsigned short);
    if (header->UpdateSequenceOffset > record_size ||
        usa_bytes > record_size - header->UpdateSequenceOffset ||
        header->UpdateSequenceSize != record_size / bytes_per_sector + 1) {
        return 0;
    }

    usa = (unsigned short *)(record + header->UpdateSequenceOffset);
    signature = usa[0];
    for (unsigned int i = 1; i < header->UpdateSequenceSize; i++) {
        unsigned int trailer_offset = i * bytes_per_sector - sizeof(unsigned short);
        unsigned short *trailer;

        if (trailer_offset > record_size - sizeof(unsigned short))
            return 0;
        trailer = (unsigned short *)(record + trailer_offset);
        if (*trailer != signature)
            return 0;
        *trailer = usa[i];
    }
    return 1;
}

static int ntfs_append_mft_run(NTFS_MFT_RUN **runs, int *count, int *capacity,
                               uint64_t lcn, uint64_t cluster_count)
{
    NTFS_MFT_RUN *grown;
    int new_capacity;

    if (!runs || !count || !capacity || cluster_count == 0)
        return 0;
    if (*count < *capacity) {
        (*runs)[*count].lcn = lcn;
        (*runs)[*count].cluster_count = cluster_count;
        (*count)++;
        return 1;
    }

    new_capacity = *capacity > 0 ? *capacity * 2 : 16;
    if (new_capacity < *capacity ||
        (size_t)new_capacity > SIZE_MAX / sizeof(NTFS_MFT_RUN)) {
        return 0;
    }
    grown = (NTFS_MFT_RUN *)realloc(
        *runs, (size_t)new_capacity * sizeof(NTFS_MFT_RUN));
    if (!grown)
        return 0;
    *runs = grown;
    *capacity = new_capacity;
    (*runs)[*count].lcn = lcn;
    (*runs)[*count].cluster_count = cluster_count;
    (*count)++;
    return 1;
}

static int ntfs_decode_mft_runlist(const unsigned char *record, DWORD record_size,
                                   NTFS_MFT_RUN **out_runs, int *out_count,
                                   uint64_t *out_clusters)
{
    const FILE_RECORD_HEADER *header;
    unsigned int offset;

    *out_runs = NULL;
    *out_count = 0;
    *out_clusters = 0;
    if (!record || record_size < sizeof(FILE_RECORD_HEADER))
        return 0;

    header = (const FILE_RECORD_HEADER *)record;
    offset = header->FirstAttributeOffset;
    while (offset <= record_size - sizeof(ATTR_HEADER)) {
        const ATTR_HEADER *attr = (const ATTR_HEADER *)(record + offset);

        if (attr->Type == 0xFFFFFFFF)
            break;
        if (attr->Length < sizeof(ATTR_HEADER) ||
            attr->Length > record_size - offset)
            break;

        if (attr->Type == ATTR_DATA && attr->NonResident &&
            attr->NameLength == 0 &&
            attr->Length >= sizeof(ATTR_HEADER) + sizeof(NON_RESIDENT_ATTR)) {
            const NON_RESIDENT_ATTR *nonresident =
                (const NON_RESIDENT_ATTR *)((const unsigned char *)attr +
                                            sizeof(ATTR_HEADER));
            const unsigned char *cursor;
            const unsigned char *end = (const unsigned char *)attr + attr->Length;
            int64_t current_lcn = 0;
            uint64_t total_clusters = 0;
            NTFS_MFT_RUN *runs = NULL;
            int run_count = 0;
            int run_capacity = 0;

            if (nonresident->StartingVCN != 0 ||
                nonresident->DataRunOffset <
                    sizeof(ATTR_HEADER) + sizeof(NON_RESIDENT_ATTR) ||
                nonresident->DataRunOffset >= attr->Length) {
                return 0;
            }

            cursor = (const unsigned char *)attr + nonresident->DataRunOffset;
            while (cursor < end && *cursor != 0) {
                unsigned char descriptor = *cursor++;
                int length_bytes = descriptor & 0x0F;
                int offset_bytes = descriptor >> 4;
                uint64_t cluster_count = 0;
                uint64_t raw_delta = 0;
                int64_t lcn_delta;

                if (length_bytes == 0 || length_bytes > 8 ||
                    offset_bytes == 0 || offset_bytes > 8 ||
                    (size_t)(end - cursor) <
                        (size_t)length_bytes + (size_t)offset_bytes) {
                    free(runs);
                    return 0;
                }
                for (int i = 0; i < length_bytes; i++)
                    cluster_count |= (uint64_t)cursor[i] << (i * 8);
                cursor += length_bytes;
                for (int i = 0; i < offset_bytes; i++)
                    raw_delta |= (uint64_t)cursor[i] << (i * 8);
                if (offset_bytes < 8 &&
                    (raw_delta & (1ULL << (offset_bytes * 8 - 1)))) {
                    raw_delta |= UINT64_MAX << (offset_bytes * 8);
                }
                lcn_delta = (int64_t)raw_delta;
                cursor += offset_bytes;

                if (cluster_count == 0 ||
                    (lcn_delta < 0 &&
                     (lcn_delta == INT64_MIN || current_lcn < -lcn_delta)) ||
                    (lcn_delta > 0 && current_lcn > INT64_MAX - lcn_delta)) {
                    free(runs);
                    return 0;
                }
                current_lcn += lcn_delta;
                if (current_lcn < 0 ||
                    total_clusters > UINT64_MAX - cluster_count ||
                    !ntfs_append_mft_run(&runs, &run_count, &run_capacity,
                                         (uint64_t)current_lcn, cluster_count)) {
                    free(runs);
                    return 0;
                }
                total_clusters += cluster_count;
            }

            if (cursor >= end || *cursor != 0 || run_count == 0) {
                free(runs);
                return 0;
            }
            *out_runs = runs;
            *out_count = run_count;
            *out_clusters = total_clusters;
            return 1;
        }
        offset += attr->Length;
    }
    return 0;
}

static int ntfs_parse_raw_mft_entry(const unsigned char *record, DWORD record_size,
                                    long long file_ref, int volume_index,
                                    INDEX_BUILD *build, INDEX_ENTRY *entry)
{
    const FILE_RECORD_HEADER *header;
    const wchar_t *best_name = NULL;
    int best_name_length = 0;
    int best_name_score = -1;
    long long parent_ref = 0;
    long long creation_time = 0;
    long long modification_time = 0;
    long long file_size = 0;
    unsigned int attributes = 0;
    unsigned int offset;

    if (!record || !build || !entry ||
        record_size < sizeof(FILE_RECORD_HEADER))
        return 0;
    header = (const FILE_RECORD_HEADER *)record;
    if (header->Magic != 0x454C4946 || !(header->Flags & FR_IN_USE) ||
        header->BaseRecordFileReference != 0) {
        return 0;
    }

    offset = header->FirstAttributeOffset;
    while (offset <= record_size - sizeof(ATTR_HEADER)) {
        const ATTR_HEADER *attr = (const ATTR_HEADER *)(record + offset);

        if (attr->Type == 0xFFFFFFFF)
            break;
        if (attr->Length < sizeof(ATTR_HEADER) ||
            attr->Length > record_size - offset)
            return 0;

        if (!attr->NonResident &&
            attr->Length >= sizeof(ATTR_HEADER) + sizeof(RESIDENT_ATTR)) {
            const RESIDENT_ATTR *resident =
                (const RESIDENT_ATTR *)((const unsigned char *)attr +
                                        sizeof(ATTR_HEADER));
            if (resident->ValueOffset <= attr->Length &&
                resident->ValueLength <= attr->Length - resident->ValueOffset) {
                const unsigned char *value =
                    (const unsigned char *)attr + resident->ValueOffset;

                if (attr->Type == ATTR_STANDARD_INFORMATION &&
                    resident->ValueLength >= sizeof(STANDARD_INFORMATION)) {
                    const STANDARD_INFORMATION *si =
                        (const STANDARD_INFORMATION *)value;
                    creation_time = si->CreationTime;
                    modification_time = si->ModificationTime;
                    attributes = si->FileAttributes;
                } else if (attr->Type == ATTR_FILE_NAME &&
                           resident->ValueLength >= sizeof(FILE_NAME_ATTR)) {
                    const FILE_NAME_ATTR *name_attr =
                        (const FILE_NAME_ATTR *)value;
                    unsigned int name_bytes =
                        (unsigned int)name_attr->NameLength * sizeof(wchar_t);
                    if (name_attr->NameLength > 0 &&
                        name_attr->NameLength <= NTFS_MAX_NAME_CHARS &&
                        resident->ValueLength >= sizeof(FILE_NAME_ATTR) + name_bytes) {
                        int score = name_attr->NameType == 1 || name_attr->NameType == 3
                            ? 3 : name_attr->NameType == 0 ? 2 : 1;
                        if (score > best_name_score) {
                            best_name = (const wchar_t *)(value + sizeof(FILE_NAME_ATTR));
                            best_name_length = name_attr->NameLength;
                            best_name_score = score;
                            parent_ref = mft_ref_to_frn(
                                name_attr->ParentLow, name_attr->ParentHigh);
                            if (!creation_time)
                                creation_time = name_attr->CreationTime;
                            if (!modification_time)
                                modification_time = name_attr->ModificationTime;
                            if (!file_size)
                                file_size = name_attr->RealSize;
                            if (!attributes)
                                attributes = name_attr->Flags;
                        }
                    }
                } else if (attr->Type == ATTR_DATA && attr->NameLength == 0 &&
                           !(header->Flags & FR_DIRECTORY)) {
                    file_size = resident->ValueLength;
                }
            }
        } else if (attr->NonResident && attr->Type == ATTR_DATA &&
                   attr->NameLength == 0 && !(header->Flags & FR_DIRECTORY) &&
                   attr->Length >= sizeof(ATTR_HEADER) + sizeof(NON_RESIDENT_ATTR)) {
            const NON_RESIDENT_ATTR *nonresident =
                (const NON_RESIDENT_ATTR *)((const unsigned char *)attr +
                                            sizeof(ATTR_HEADER));
            if (nonresident->DataSize >= 0)
                file_size = nonresident->DataSize;
        }
        offset += attr->Length;
    }

    if (!best_name && file_ref != NTFS_ROOT_FRN)
        return 0;

    memset(entry, 0, sizeof(*entry));
    if (!ntfs_fill_entry_from_name(build, entry, best_name ? best_name : L"",
                                   best_name ? best_name_length : 0)) {
        return 0;
    }
    entry->size = (header->Flags & FR_DIRECTORY) ? 0 : file_size;
    entry->creation_time = creation_time;
    entry->modification_time = modification_time;
    entry->attributes = attributes;
    entry->file_ref = file_ref;
    entry->parent_ref = file_ref == NTFS_ROOT_FRN ? NTFS_ROOT_FRN : parent_ref;
    entry->is_directory = (header->Flags & FR_DIRECTORY) ? 1 : 0;
    if (entry->is_directory)
        entry->attributes |= FILE_ATTRIBUTE_DIRECTORY;
    entry->volume_index = (signed char)volume_index;
    entry->parent_index = INDEX_PARENT_UNKNOWN;
    entry->metadata_loaded = 1;
    return 1;
}

static int ntfs_read_mft_bulk(HANDLE hVolume, INDEX_BUILD *build,
                              int volume_index,
                              HWND hwnd_progress)
{
    NTFS_VOLUME_DATA_BUF volume_data;
    NTFS_MFT_RUN *runs = NULL;
    int run_count = 0;
    uint64_t run_clusters = 0;
    uint64_t valid_bytes;
    uint64_t consumed_bytes = 0;
    uint64_t record_number = 0;
    DWORD record_size;
    DWORD cluster_size;
    DWORD sector_size;
    unsigned char *record_zero = NULL;
    unsigned char *buffer = NULL;
    size_t carry = 0;
    int ok = 0;
    int last_progress = -1;
    const DWORD chunk_size = 8 * 1024 * 1024;

    memset(&volume_data, 0, sizeof(volume_data));
    {
        DWORD bytes = 0;
        if (!DeviceIoControl(hVolume, FSCTL_GET_NTFS_VOLUME_DATA,
                             NULL, 0, &volume_data, sizeof(volume_data),
                             &bytes, NULL)) {
            return 0;
        }
    }

    record_size = volume_data.BytesPerFileRecordSegment;
    cluster_size = volume_data.BytesPerCluster;
    sector_size = volume_data.BytesPerSector;
    if (volume_data.MftValidDataLength <= 0)
        return 0;
    valid_bytes = (uint64_t)volume_data.MftValidDataLength;
    if (record_size < 512 || record_size > 65536 ||
        cluster_size < sector_size || cluster_size > 2 * 1024 * 1024 ||
        sector_size < 512 || sector_size > 65536 ||
        valid_bytes < record_size) {
        return 0;
    }

    record_zero = (unsigned char *)malloc(record_size);
    buffer = (unsigned char *)malloc((size_t)chunk_size + record_size);
    if (!record_zero || !buffer || !ntfs_grow_entries(build, 65536))
        goto done;

    if (volume_data.MftStartLcn < 0 ||
        (uint64_t)volume_data.MftStartLcn > UINT64_MAX / cluster_size ||
        !ntfs_read_exact_at(
            hVolume,
            (uint64_t)volume_data.MftStartLcn * cluster_size,
            record_zero, record_size) ||
        !ntfs_fixup_raw_record(record_zero, record_size, sector_size) ||
        !ntfs_decode_mft_runlist(record_zero, record_size,
                                 &runs, &run_count, &run_clusters) ||
        run_clusters < (valid_bytes + cluster_size - 1) / cluster_size) {
        goto done;
    }

    for (int run_index = 0;
         run_index < run_count && consumed_bytes < valid_bytes;
         run_index++) {
        uint64_t physical;
        uint64_t run_bytes;
        uint64_t run_position = 0;

        if (runs[run_index].lcn > UINT64_MAX / cluster_size ||
            runs[run_index].cluster_count > UINT64_MAX / cluster_size) {
            goto done;
        }
        physical = runs[run_index].lcn * (uint64_t)cluster_size;
        run_bytes = runs[run_index].cluster_count * (uint64_t)cluster_size;

        if (run_bytes > valid_bytes - consumed_bytes)
            run_bytes = valid_bytes - consumed_bytes;
        while (run_position < run_bytes) {
            DWORD to_read = (DWORD)((run_bytes - run_position) > chunk_size
                ? chunk_size : (run_bytes - run_position));
            size_t available;
            size_t complete_records;

            if (physical > UINT64_MAX - run_position ||
                !ntfs_read_exact_at(hVolume, physical + run_position,
                                    buffer + carry, to_read)) {
                goto done;
            }
            available = carry + to_read;
            complete_records = available / record_size;
            for (size_t i = 0; i < complete_records; i++, record_number++) {
                unsigned char *record = buffer + i * record_size;
                INDEX_ENTRY parsed;

                if (record_number > 0x0000FFFFFFFFFFFFULL)
                    goto done;
                memset(&parsed, 0, sizeof(parsed));
                if (!ntfs_fixup_raw_record(record, record_size, sector_size) ||
                    !ntfs_parse_raw_mft_entry(record, record_size,
                                              (long long)record_number,
                                              volume_index, build, &parsed)) {
                    continue;
                }
                if (!ntfs_grow_entries(build, build->count + 1)) {
                    goto done;
                }
                build->entries[build->count++] = parsed;
            }

            carry = available - complete_records * record_size;
            if (carry > 0)
                memmove(buffer, buffer + complete_records * record_size, carry);
            run_position += to_read;
            consumed_bytes += to_read;

            if (hwnd_progress) {
                int progress = (int)(consumed_bytes * 100 / valid_bytes);
                if (progress != last_progress) {
                    last_progress = progress;
                    PostMessageW(hwnd_progress, WM_INDEX_PROGRESS,
                        (WPARAM)progress, (LPARAM)volume_index);
                }
            }
        }
    }

    if (consumed_bytes != valid_bytes || carry != valid_bytes % record_size ||
        build->count == 0) {
        goto done;
    }
    ok = 1;

done:
    free(record_zero);
    free(buffer);
    free(runs);
    if (!ok) {
        index_build_free(build);
        return 0;
    }
    if (hwnd_progress && last_progress != 100) {
        PostMessageW(hwnd_progress, WM_INDEX_PROGRESS,
                     (WPARAM)100, (LPARAM)volume_index);
    }
    return build->count;
}

static int ntfs_read_mft_records(HANDLE hVolume, INDEX_BUILD *build,
                                 int volume_index,
                                 HWND hwnd_progress)
{
    NTFS_VOLUME_DATA_BUF vol_data = {0};
    DWORD bytes;
    void *buffer = NULL;
    int last_progress = -1;
    
    /* Get volume geometry */
    if (!DeviceIoControl(hVolume, FSCTL_GET_NTFS_VOLUME_DATA,
                         NULL, 0, &vol_data, sizeof(vol_data), &bytes, NULL)) {
        return 0;
    }
    
    DWORD record_size = vol_data.BytesPerFileRecordSegment;
    if (record_size == 0 || record_size > 4096)
        record_size = 1024;
    
    DWORD output_header_size = (DWORD)offsetof(NTFS_FILE_RECORD_OUTPUT_BUF, FileRecordBuffer);
    DWORD output_buffer_size = output_header_size + record_size;
    
    buffer = malloc(output_buffer_size);
    if (!buffer) return 0;
    
    if (!ntfs_grow_entries(build, 65536)) {
        free(buffer);
        return 0;
    }
    
    long long mft_max_frn = vol_data.MftValidDataLength / record_size;
    long long frn;
    
    for (frn = 0; frn < mft_max_frn && frn < 10000000; frn++) {
        long long frn_query = frn;
        memset(buffer, 0, output_buffer_size);
        
        if (!DeviceIoControl(hVolume, FSCTL_GET_NTFS_FILE_RECORD,
                             &frn_query, sizeof(frn_query),
                             buffer, output_buffer_size, &bytes, NULL)) {
            continue;
        }
        
        if (bytes <= output_header_size)
            continue;
        
        NTFS_FILE_RECORD_OUTPUT_BUF *out = (NTFS_FILE_RECORD_OUTPUT_BUF *)buffer;
        unsigned int available = bytes - output_header_size;
        unsigned int record_len = out->FileRecordLength;
        if (record_len == 0 || record_len > available)
            record_len = available;
        if (record_len < sizeof(FILE_RECORD_HEADER))
            continue;
        
        unsigned char *record = out->FileRecordBuffer;
        FILE_RECORD_HEADER *hdr = (FILE_RECORD_HEADER *)record;
        
        if (hdr->Magic != 0x454C4946)
            continue;
        
        if (!(hdr->Flags & FR_IN_USE))
            continue;
        
        unsigned int offset = hdr->FirstAttributeOffset;
        wchar_t name_buf[512] = {0};
        long long parent_frn = 0;
        long long ctime = 0, mtime = 0;
        long long real_size = 0;
        unsigned int attrs = 0;
        int best_name_score = -1;
        
        while (offset + sizeof(ATTR_HEADER) <= record_len) {
            ATTR_HEADER *attr = (ATTR_HEADER *)(record + offset);
            
            if (attr->Type == 0xFFFFFFFF || attr->Length == 0)
                break;
            
            if (attr->Length < sizeof(ATTR_HEADER) || offset + attr->Length > record_len)
                break;
            
            if (!attr->NonResident && attr->Length >= sizeof(ATTR_HEADER) + sizeof(RESIDENT_ATTR)) {
                RESIDENT_ATTR *res = (RESIDENT_ATTR *)((char*)attr + sizeof(ATTR_HEADER));
                if (res->ValueOffset <= attr->Length &&
                    res->ValueLength <= attr->Length - res->ValueOffset) {
                    void *value = (char*)attr + res->ValueOffset;
                    
                    if (attr->Type == ATTR_STANDARD_INFORMATION &&
                        res->ValueLength >= sizeof(STANDARD_INFORMATION)) {
                        STANDARD_INFORMATION *si = (STANDARD_INFORMATION *)value;
                        ctime = si->CreationTime;
                        mtime = si->ModificationTime;
                        attrs = si->FileAttributes;
                    }
                    else if (attr->Type == ATTR_FILE_NAME &&
                             res->ValueLength >= sizeof(FILE_NAME_ATTR)) {
                        FILE_NAME_ATTR *fn = (FILE_NAME_ATTR *)value;
                        unsigned int name_bytes = fn->NameLength * sizeof(wchar_t);
                        if (fn->NameLength > 0 &&
                            fn->NameLength < 255 &&
                            res->ValueLength >= sizeof(FILE_NAME_ATTR) + name_bytes) {
                            int score;
                            if (fn->NameType == 1 || fn->NameType == 3)
                                score = 3;          /* Win32 names */
                            else if (fn->NameType == 0)
                                score = 2;          /* POSIX names */
                            else
                                score = 1;          /* DOS 8.3 names */
                            
                            if (score >= best_name_score) {
                                wchar_t *fn_name = (wchar_t *)((char*)fn + sizeof(FILE_NAME_ATTR));
                                wcsncpy_s(name_buf, 512, fn_name, fn->NameLength);
                                parent_frn = mft_ref_to_frn(fn->ParentLow, fn->ParentHigh);
                                real_size = fn->RealSize;
                                if (!ctime) ctime = fn->CreationTime;
                                if (!mtime) mtime = fn->ModificationTime;
                                if (!attrs) attrs = fn->Flags;
                                best_name_score = score;
                            }
                        }
                    }
                }
            }
            
            offset += attr->Length;
        }
        
        if (name_buf[0]) {
            if (!ntfs_grow_entries(build, build->count + 1))
                break;

            INDEX_ENTRY *e = &build->entries[build->count];

            /* Skip the record rather than admit a NULL name to the index. */
            if (!ntfs_fill_entry_from_name(build, e, name_buf, -1))
                break;

            e->size = real_size;
            e->creation_time = ctime;
            e->modification_time = mtime;
            e->attributes = attrs;
            e->file_ref = frn;
            e->parent_ref = parent_frn;
            e->is_directory = (hdr->Flags & FR_DIRECTORY) ? 1 : 0;
            e->volume_index = (signed char)volume_index;
            e->parent_index = INDEX_PARENT_UNKNOWN;
            e->metadata_loaded = 1;

            build->count++;
        }
        
        if (hwnd_progress && mft_max_frn > 0) {
            int progress = (int)(frn * 100 / mft_max_frn);
            if (progress != last_progress) {
                last_progress = progress;
                PostMessageW(hwnd_progress, WM_INDEX_PROGRESS,
                    (WPARAM)(INDEX_PROGRESS_FALLBACK_BASE + progress),
                    (LPARAM)volume_index);
            }
        }
    }
    
    free(buffer);
    if (hwnd_progress && build->count > 0 && last_progress != 100) {
        PostMessageW(hwnd_progress, WM_INDEX_PROGRESS,
            (WPARAM)(INDEX_PROGRESS_FALLBACK_BASE + 100),
            (LPARAM)volume_index);
    }
    return build->count;
}

int ntfs_read_mft(HANDLE hVolume, INDEX_BUILD *build,
                  int volume_index, HWND hwnd_progress)
{
    int count;

    if (!build)
        return 0;
    index_build_init(build);
    count = ntfs_read_mft_bulk(hVolume, build, volume_index, hwnd_progress);
    if (count > 0)
        return count;
    if (hwnd_progress) {
        PostMessageW(hwnd_progress, WM_INDEX_PROGRESS,
            (WPARAM)INDEX_PROGRESS_FALLBACK_BASE, (LPARAM)volume_index);
    }
    index_build_init(build);
    count = ntfs_read_mft_records(hVolume, build, volume_index, hwnd_progress);
    if (count <= 0)
        index_build_free(build);
    return count;
}

int ntfs_query_usn_journal(HANDLE hVolume, USN_JOURNAL_DATA_BUF *data)
{
    DWORD bytes;
    return DeviceIoControl(hVolume, FSCTL_QUERY_USN_JOURNAL,
                           NULL, 0, data, sizeof(USN_JOURNAL_DATA_BUF), &bytes, NULL);
}

void ntfs_format_attributes(wchar_t *buf, size_t buf_size, unsigned int attrs)
{
    /* Flag order matches the column's historical layout. */
    static const struct { unsigned int bit; wchar_t letter; } k_flags[] = {
        { 0x0001, L'R' }, { 0x0002, L'H' }, { 0x0004, L'S' }, { 0x0020, L'A' },
        { 0x0010, L'D' }, { 0x0100, L'T' }, { 0x0200, L'C' }, { 0x1000, L'P' },
        { 0x2000, L'N' }, { 0x4000, L'E' }, { 0x0800, L'O' },
    };
    size_t pos = 0;
    size_t i;

    if (!buf || buf_size == 0)
        return;

    /* Append directly instead of chaining swprintf_s: `buf_size - pos` used to
       wrap around once pos passed buf_size, and a -1 return drove pos negative
       so the next write landed before the buffer. */
    for (i = 0; i < sizeof(k_flags) / sizeof(k_flags[0]); i++) {
        if ((attrs & k_flags[i].bit) && pos + 1 < buf_size)
            buf[pos++] = k_flags[i].letter;
    }
    buf[pos] = L'\0';
}

void ntfs_format_size(wchar_t *buf, size_t buf_size, long long bytes)
{
    if (bytes < 1024LL)
        swprintf_s(buf, buf_size, L"%lld B", bytes);
    else if (bytes < 1024LL * 1024LL)
        swprintf_s(buf, buf_size, L"%.1f KB", bytes / 1024.0);
    else if (bytes < 1024LL * 1024LL * 1024LL)
        swprintf_s(buf, buf_size, L"%.1f MB", bytes / (1024.0 * 1024.0));
    else
        swprintf_s(buf, buf_size, L"%.2f GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

long long ntfs_filetime_to_unix(long long ft)
{
    return (ft - 116444736000000000LL) / 10000000LL;
}
