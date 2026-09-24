//  MIT License - Modified for Mandatory Attribution
//  Copyright(c) 2025 Sergio Paludo - github.com/sergiocupa
//
//  Implementacao de gw_path para Windows (Win32) e Linux (POSIX).

#include "gw_path.h"
#include "gw_base.h"     // GW_ALLOC/GW_FREE (memory_pool da xplatbase)
#include <string.h>
#include <stdio.h>

#ifdef _WIN32
  #include <windows.h>
#else
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <dirent.h>
  #include <unistd.h>
#endif

char* gw_path_join(char* out, size_t size, const char* base, const char* leaf)
{
    if (!out || size == 0) return out;
    if (!base || !*base) { snprintf(out, size, "%s", leaf ? leaf : ""); return out; }
    if (!leaf || !*leaf) { snprintf(out, size, "%s", base); return out; }

    size_t n = strlen(base);
    int    has_sep = (base[n - 1] == '/' || base[n - 1] == '\\');
    while (*leaf == '/' || *leaf == '\\') leaf++;   // evita "a//b"
    snprintf(out, size, has_sep ? "%s%s" : "%s/%s", base, leaf);
    return out;
}

void gw_path_normalize(char* path)
{
    if (!path) return;
    for (char* p = path; *p; p++) if (*p == '\\') *p = '/';
}

int gw_file_exists(const char* path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES) && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

int gw_dir_exists(const char* path)
{
#ifdef _WIN32
    DWORD a = GetFileAttributesA(path);
    return (a != INVALID_FILE_ATTRIBUTES) && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int gw_mkdir_one(const char* path)
{
#ifdef _WIN32
    if (CreateDirectoryA(path, 0)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS;
#else
    if (mkdir(path, 0775) == 0) return 1;
    return gw_dir_exists(path);
#endif
}

int gw_mkdir_p(const char* path)
{
    if (!path || !*path) return 0;
    if (gw_dir_exists(path)) return 1;

    char buf[1024];
    snprintf(buf, sizeof(buf), "%s", path);
    gw_path_normalize(buf);

    // Pula a raiz ("/" ou "C:/") para nao tentar cria-la.
    char* p = buf;
    if (p[0] == '/') p++;
    else if (p[0] && p[1] == ':') p += (p[2] == '/') ? 3 : 2;

    for (; *p; p++)
    {
        if (*p != '/') continue;
        *p = '\0';
        if (buf[0] && !gw_mkdir_one(buf)) { *p = '/'; return 0; }
        *p = '/';
    }
    return gw_mkdir_one(buf);
}

int gw_rmtree(const char* path)
{
    if (!path || !*path) return 0;

#ifdef _WIN32
    DWORD attr = GetFileAttributesA(path);
    if (attr == INVALID_FILE_ATTRIBUTES) return 1;   // ja nao existe

    if (!(attr & FILE_ATTRIBUTE_DIRECTORY))
    {
        // Somente-leitura impede o DeleteFile; o mp4 recem-gravado tambem pode estar
        // com lock transitorio (antivirus), por isso o atributo e limpo antes.
        SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
        return DeleteFileA(path) != 0;
    }

    char pat[1024];
    snprintf(pat, sizeof(pat), "%s/*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int ok = 1;
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
            char child[1024];
            gw_path_join(child, sizeof(child), path, fd.cFileName);
            if (!gw_rmtree(child)) ok = 0;
        }
        while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    if (!RemoveDirectoryA(path)) ok = 0;
    return ok;
#else
    struct stat st;
    if (lstat(path, &st) != 0) return 1;             // ja nao existe
    if (!S_ISDIR(st.st_mode)) return unlink(path) == 0;

    DIR* d = opendir(path);
    int ok = 1;
    if (d)
    {
        struct dirent* e;
        while ((e = readdir(d)) != 0)
        {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char child[1024];
            gw_path_join(child, sizeof(child), path, e->d_name);
            if (!gw_rmtree(child)) ok = 0;
        }
        closedir(d);
    }
    if (rmdir(path) != 0) ok = 0;
    return ok;
#endif
}

int gw_dir_list(const char* dir, GwDirEntryFn cb, void* user)
{
    if (!dir || !*dir) return -1;
    int count = 0;

#ifdef _WIN32
    char pat[1024];
    snprintf(pat, sizeof(pat), "%s/*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do
    {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) continue;
        count++;
        if (cb) cb(user, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    }
    while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR* d = opendir(dir);
    if (!d) return -1;
    struct dirent* e;
    while ((e = readdir(d)) != 0)
    {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        count++;
        if (cb)
        {
            char child[1024];
            gw_path_join(child, sizeof(child), dir, e->d_name);
            cb(user, e->d_name, gw_dir_exists(child));
        }
    }
    closedir(d);
#endif
    return count;
}

// ---- arquivo grande --------------------------------------------------------

FILE* gw_fopen_rb(const char* path)
{
#ifdef _WIN32
    FILE* f = 0;
    if (fopen_s(&f, path, "rb") != 0) return 0;
    return f;
#else
    return fopen(path, "rb");
#endif
}

int gw_fseek64(FILE* f, int64_t off, int origin)
{
#ifdef _WIN32
    return _fseeki64(f, off, origin);
#else
    return fseeko(f, (off_t)off, origin);
#endif
}

int64_t gw_ftell64(FILE* f)
{
#ifdef _WIN32
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}
