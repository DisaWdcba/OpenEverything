#include "ntfs.h"
#include "common.h"

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

static int ntfs_grow_entries(INDEX_ENTRY **entries, int *capacity, int needed)
{
    if (needed <= *capacity)
        return 1;
    if (needed > (int)(INT_MAX / sizeof(INDEX_ENTRY)))
        return 0;

    /* A zero capacity would spin forever on the doubling loop below. */
    int new_cap = *capacity > 0 ? *capacity : 65536;
    while (new_cap < needed) {
        if (new_cap > INT_MAX / 2)
            return 0;
        new_cap *= 2;
    }

    INDEX_ENTRY *new_entries = (INDEX_ENTRY *)realloc(*entries, (size_t)new_cap * sizeof(INDEX_ENTRY));
    if (!new_entries)
        return 0;

    memset(new_entries + *capacity, 0, (size_t)(new_cap - *capacity) * sizeof(INDEX_ENTRY));
    *entries = new_entries;
    *capacity = new_cap;
    return 1;
}

static int ntfs_fill_entry_from_name(INDEX_ENTRY *e, const wchar_t *name, int name_len)
{
    if (name_len < 0)
        name_len = (int)wcslen(name);

    e->name = (wchar_t *)malloc(((size_t)name_len + 1) * sizeof(wchar_t));
    if (!e->name)
        return 0;

    if (name_len > 0)
        memcpy(e->name, name, (size_t)name_len * sizeof(wchar_t));
    e->name[name_len] = L'\0';
    return 1;
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

int ntfs_read_usn_index(HANDLE hVolume, INDEX_ENTRY **out_entries, int *out_count,
                        int volume_index, HWND hwnd_progress)
{
    USN_JOURNAL_DATA_BUF journal = {0};
    MFT_ENUM_DATA_BUF enum_data = {0};
    DWORD bytes = 0;
    char *buffer = NULL;
    DWORD buffer_size = 4 * 1024 * 1024;
    INDEX_ENTRY *entries = NULL;
    int entry_count = 0;
    int entry_cap = 65536;
    int last_progress = 0;
    int ok = 0;

    *out_entries = NULL;
    *out_count = 0;
    
    if (!ntfs_query_usn_journal(hVolume, &journal))
        return 0;
    
    buffer = (char *)malloc(buffer_size);
    entries = (INDEX_ENTRY *)calloc(entry_cap, sizeof(INDEX_ENTRY));
    if (!buffer || !entries) {
        free(buffer);
        free(entries);
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
                ntfs_grow_entries(&entries, &entry_cap, entry_count + 1)) {
                INDEX_ENTRY *e = &entries[entry_count];

                /* Publish only once the name is owned. A NULL name here would
                   travel into the index and make every later cache save fail. */
                if (!ntfs_fill_entry_from_name(e, name, name_chars))
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
                entry_count++;
            }

            offset += rec->RecordLength;
        }

        /* Report on growth rather than on an exact multiple: batch sizes vary,
           so `entry_count % 65536 == 0` almost never hit and the UI sat silent
           for the whole enumeration. */
        if (hwnd_progress && entry_count - last_progress >= 65536) {
            last_progress = entry_count;
            PostMessageW(hwnd_progress, WM_INDEX_PROGRESS, (WPARAM)0, (LPARAM)volume_index);
        }
    }

enum_done:
    free(buffer);
    
    if (!ok && entry_count == 0) {
        free(entries);
        return 0;
    }
    
    *out_entries = entries;
    *out_count = entry_count;
    return entry_count;
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

int ntfs_read_mft(HANDLE hVolume, INDEX_ENTRY **out_entries, int *out_count,
                  int volume_index, HWND hwnd_progress)
{
    NTFS_VOLUME_DATA_BUF vol_data = {0};
    DWORD bytes;
    void *buffer = NULL;
    INDEX_ENTRY *entries = NULL;
    int entry_count = 0;
    int entry_cap = 65536;
    
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
    
    entries = (INDEX_ENTRY *)calloc(entry_cap, sizeof(INDEX_ENTRY));
    if (!entries) { free(buffer); return 0; }
    
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
            if (!ntfs_grow_entries(&entries, &entry_cap, entry_count + 1))
                break;

            INDEX_ENTRY *e = &entries[entry_count];

            /* Skip the record rather than admit a NULL name to the index. */
            if (!ntfs_fill_entry_from_name(e, name_buf, -1))
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

            entry_count++;
        }
        
        if (hwnd_progress && mft_max_frn > 0 && (frn % 5000) == 0) {
            PostMessageW(hwnd_progress, WM_INDEX_PROGRESS, 
                         (WPARAM)(int)(frn * 100 / mft_max_frn), (LPARAM)volume_index);
        }
    }
    
    free(buffer);
    *out_entries = entries;
    *out_count = entry_count;
    return entry_count;
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
