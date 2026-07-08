#ifndef NTFS_H
#define NTFS_H

#include "common.h"

/* NTFS engine API */
int ntfs_enumerate_volumes(VOLUME_INFO *volumes, int max_volumes);
HANDLE ntfs_open_volume(const wchar_t *volume_path);
void ntfs_close_volume(HANDLE h);
int ntfs_read_mft(HANDLE hVolume, INDEX_ENTRY **entries, int *count, int volume_index, HWND hwnd_progress);
int ntfs_read_usn_index(HANDLE hVolume, INDEX_ENTRY **entries, int *count, int volume_index, HWND hwnd_progress);
int ntfs_update_volume_usn_info(HANDLE hVolume, VOLUME_INFO *volume);
int ntfs_read_usn_changes(HANDLE hVolume, long long start_usn, long long journal_id,
                          long long stop_usn, int volume_index,
                          USN_CHANGE **changes, int *count, long long *next_usn,
                          int max_changes);
void ntfs_free_usn_changes(USN_CHANGE *changes, int count);
int ntfs_query_usn_journal(HANDLE hVolume, USN_JOURNAL_DATA_BUF *data);
int ntfs_create_usn_journal(HANDLE hVolume, long long max_size);
int ntfs_read_usn_records(HANDLE hVolume, long long start_usn, long long journal_id,
                           void (*callback)(USN_RECORD_BUF *record, wchar_t *name, void *ctx), void *ctx);
void ntfs_format_attributes(wchar_t *buf, size_t buf_size, unsigned int attrs);
void ntfs_format_size(wchar_t *buf, size_t buf_size, long long bytes);
long long ntfs_filetime_to_unix(long long ft);

#endif /* NTFS_H */
