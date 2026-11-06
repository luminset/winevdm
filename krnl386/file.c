/*
 * File handling functions
 *
 * Copyright 1993 John Burton
 * Copyright 1996 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * TODO:
 *    Fix the CopyFileEx methods to implement the "extended" functionality.
 *    Right now, they simply call the CopyFile method.
 */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#include <stdio.h>
#include <assert.h>

#include "winerror.h"
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "wine/winbase16.h"
#include "kernel16_private.h"
#include "wine/unicode.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(file);

#include <shlwapi.h>
#include <imagehlp.h>
#include <aclapi.h>
#pragma comment(lib, "imagehlp.lib")
#pragma comment(lib, "shlwapi.lib")
#ifdef ENABLEREDIRECTSYSTEMDIR
BOOL EnableRedirectSystemDir = TRUE;
#else
BOOL EnableRedirectSystemDir = FALSE;
#endif
const char *GetRedirectWindowsDir();
static void RedirectPrivateProfileStringWindowsDir(LPCSTR filename, LPCSTR output);

NTSYSAPI
NTSTATUS
NTAPI
NtAccessCheck(IN PSECURITY_DESCRIPTOR SecurityDescriptor,
    IN HANDLE TokenHandle,
    IN ACCESS_MASK DesiredAccess,
    IN PGENERIC_MAPPING GenericMapping,
    OUT PPRIVILEGE_SET PrivilegeSet OPTIONAL,
    IN OUT PULONG PrivilegeSetLength,
    OUT PACCESS_MASK GrantedAccess,
    OUT PNTSTATUS AccessStatus);
BOOL can_write_directory(LPCSTR path)
{
    HANDLE hToken = INVALID_HANDLE_VALUE, hTokenImpersonation = INVALID_HANDLE_VALUE;
    PSECURITY_DESCRIPTOR pSecurityDescriptor = NULL;
    GENERIC_MAPPING genericMapping = { 0 };
    PRIVILEGE_SET privilegeSet = { 0 };
    NTSTATUS accessStatus = 0;
    DWORD dwDesiredAccess = FILE_GENERIC_WRITE;
    DWORD dwSize = sizeof(PRIVILEGE_SET);
    DWORD dwGrantedAccess = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &hToken))
        goto fail;
    if (!DuplicateTokenEx(hToken, GENERIC_ALL, NULL, SecurityImpersonation, TokenImpersonation, &hTokenImpersonation))
        goto fail;
    if (GetNamedSecurityInfoA(path, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, NULL, NULL, NULL, NULL, &pSecurityDescriptor) != ERROR_SUCCESS)
        goto fail;
    genericMapping.GenericRead = FILE_GENERIC_READ;
    genericMapping.GenericWrite = FILE_GENERIC_WRITE;
    genericMapping.GenericExecute = FILE_GENERIC_EXECUTE;
    genericMapping.GenericAll = FILE_ALL_ACCESS;
    MapGenericMask(&dwDesiredAccess, &genericMapping);
    /* Because app compatibility layer hooks AccessCheck and returns incorrect result, call NtAccessCheck. (NS_WRPMitigation::APIHook_AccessCheck) */
    if (!NT_SUCCESS(NtAccessCheck(pSecurityDescriptor, hTokenImpersonation, dwDesiredAccess, &genericMapping, &privilegeSet, &dwSize, &dwGrantedAccess, &accessStatus)))
        goto fail;
fail:
    CloseHandle(hToken);
    CloseHandle(hTokenImpersonation);
    LocalFree(pSecurityDescriptor);
    return NT_SUCCESS(accessStatus);
}
static char virtual_drive = 0;
static BOOL virtual_drive_loaded = FALSE;
static BOOL virtual_drive_explicit = FALSE;

static void load_virtual_drive(void)
{
    char cfg[16];
    DWORD n;
    if (virtual_drive_loaded) return;
    virtual_drive_loaded = TRUE;
    /* Opt-in: the virtual drive stays disabled unless [otvdm] VirtualDrive is
     * present and non-empty. A missing key must never silently move the C:\
     * paths of already installed programs into <otvdm>\C; an existing
     * deployment keeps exactly the host paths it had before this feature. */
    n = krnl386_get_config_string("otvdm", "VirtualDrive", "", cfg, sizeof(cfg));
    if (!n || !cfg[0])
    {
        virtual_drive = 0;
        return;
    }
    if (cfg[0] >= 'a' && cfg[0] <= 'z') virtual_drive = cfg[0] - 'a' + 'A';
    else if (cfg[0] >= 'A' && cfg[0] <= 'Z') virtual_drive = cfg[0];
    else virtual_drive = 0;
    virtual_drive_explicit = virtual_drive != 0;
}

/* Directory holding otvdm.exe, in short form: <otvdm>. The virtual drive lives
 * in <otvdm>\<letter>. This is deliberately computed from the executable and
 * not from GetRedirectWindowsDir(), so a custom [otvdm] WINDIR (absolute or
 * rooted) cannot break the mapping. */
static const char *GetVirtualDriveBase(void)
{
    static char base[MAX_PATH];
    if (base[0]) return base;
    GetModuleFileNameA(NULL, base, sizeof(base));
    PathRemoveFileSpecA(base);
    GetShortPathNameA(base, base, sizeof(base));
    PathRemoveBackslashA(base);
    return base;
}

/* Path that 16-bit programs should see for the Windows directory.
 * With a virtual drive this is C:\WINDOWS even though the real directory
 * lives next to otvdm.exe. */
const char *GetVirtualWindowsDir(void)
{
    static char vpath[MAX_PATH];
    char ini[MAX_PATH];
    load_virtual_drive();
    if (!virtual_drive) return GetRedirectWindowsDir();
    if (vpath[0]) return vpath;
    if (!krnl386_get_config_string("otvdm", "WINDIR", "WINDOWS", ini, sizeof(ini)))
        strcpy(ini, "WINDOWS");
    if (ini[0] && ini[1] == ':') return GetRedirectWindowsDir();
    if (ini[0] == '\\' || ini[0] == '/') return GetRedirectWindowsDir();
    if (ini[0] && ini[1] == '\\')
        snprintf(vpath, sizeof(vpath), "%c:%s", virtual_drive, ini);
    else
        snprintf(vpath, sizeof(vpath), "%c:\\%s", virtual_drive, ini);
    vpath[sizeof(vpath) - 1] = '\0';
    return vpath;
}

/* TRUE when 16-bit paths on the configured drive are redirected under the
 * otvdm directory. Used by the 32-bit child-process warning. */
BOOL IsVirtualDriveActive(void)
{
    load_virtual_drive();
    return virtual_drive != 0;
}

/* TRUE only when [otvdm] VirtualDrive is present and non-empty, i.e. when the
 * user opted in. Warnings keyed on this only fire for such installations. */
BOOL IsVirtualDriveExplicit(void)
{
    load_virtual_drive();
    return virtual_drive_explicit && virtual_drive != 0;
}

static BOOL contains_ci(LPCSTR haystack, LPCSTR needle)
{
    size_t len = strlen(needle);
    if (!haystack || !len) return FALSE;
    for (; *haystack; haystack++)
        if (!_strnicmp(haystack, needle, len)) return TRUE;
    return FALSE;
}

static BOOL is_installer_like(LPCSTR s)
{
    return contains_ci(s, "setup") || contains_ci(s, "install") ||
           contains_ci(s, "unwise") || contains_ci(s, "~gl") ||
           contains_ci(s, "glbs") || contains_ci(s, "wise");
}

/* 16-bit installers often unpack a 32-bit setup engine (Wise/InstallShield
 * ~GL_*.EXE). CreateProcessA starts that engine natively, where the 16-bit
 * virtual-drive redirect does not apply. Tell the user instead of silently
 * writing into the real C: drive. */
__declspec(dllexport) void Warn32BitChild(LPCSTR filename, LPCSTR cmdline)
{
    DWORD type;
    static BOOL warned;
    char text[1024];
    typedef int (WINAPI *MESSAGEBOXA)(HWND, LPCSTR, LPCSTR, UINT);
    HMODULE user32;
    MESSAGEBOXA mb;
    if (warned || !IsVirtualDriveExplicit()) return;
    if (!GetBinaryTypeA(filename, &type)) return;
    /* GetBinaryTypeA returns SCS_32BIT_BINARY (0) for PE32. */
    if (type != 0 && type != 6) return;

    if (!is_installer_like(filename) && !is_installer_like(cmdline)) return;
    ERR("32-bit child process %s bypasses the virtual drive; C:\\ paths go to the real C: drive.\n", filename);

    warned = TRUE;
    sprintf(text,
            "This program starts a 32-bit child process:\n%s\n\n"
            "otvdm can redirect C:\\ paths only for 16-bit code. The 32-bit child runs natively and writes to the real C: drive, not to <otvdm>\\C.\n\n"
            "For an installer, use a sandbox/VM, or choose an install directory under <otvdm>\\C.",
            filename);
    user32 = GetModuleHandleA("user32.dll");
    mb = user32 ? (MESSAGEBOXA)GetProcAddress(user32, "MessageBoxA") : NULL;
    if (mb) mb(NULL, text, "otvdm - 32-bit installer", 0x00000030L);
}

static BOOL buffer_contains(LPCSTR buf, size_t len, LPCSTR needle)
{
    size_t n = strlen(needle), i;
    if (!buf || !n) return FALSE;
    for (i = 0; i + n <= len; i++)
        if (!_strnicmp(buf + i, needle, n)) return TRUE;
    return FALSE;
}

/* Some legacy installers are 16-bit stubs that unpack a 32-bit engine or use
 * a private setup runtime. Warn once at launch, before any file is written. */
LPCSTR RedirectDriveRoot(LPCSTR path, LPSTR to, size_t max_len, BOOL silence);
LPCSTR RedirectSystemDir(LPCSTR path, LPSTR to, size_t max_len);

void WarnLegacyInstaller(LPCSTR filename)
{
    static const char *markers[] = {
        "GLBSInstall", "GLBSSTUB", "WiseMain", "InstallShield",
        "UNWISE", "STUB.exe", "_INS"
    };
    static BOOL warned;
    HANDLE h;
    DWORD size, read;
    char *buf;
    BOOL found = FALSE;
    size_t i;
    char text[1024];
    char b1[MAX_PATH], b2[MAX_PATH];
    LPCSTR real;
    typedef int (WINAPI *MESSAGEBOXA)(HWND, LPCSTR, LPCSTR, UINT);
    HMODULE user32;
    MESSAGEBOXA mb;

    if (warned || !filename || !IsVirtualDriveExplicit()) return;
    real = RedirectSystemDir(filename, b1, MAX_PATH);
    real = RedirectDriveRoot(real, b2, MAX_PATH, TRUE);
    h = CreateFileA(real, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    size = GetFileSize(h, NULL);
    if (size == 0 || size > 8 * 1024 * 1024) { CloseHandle(h); return; }
    buf = HeapAlloc(GetProcessHeap(), 0, size);
    if (!buf) { CloseHandle(h); return; }
    if (!ReadFile(h, buf, size, &read, NULL) || read != size) { HeapFree(GetProcessHeap(), 0, buf); CloseHandle(h); return; }
    CloseHandle(h);
    for (i = 0; i < ARRAY_SIZE(markers); i++)
        if (buffer_contains(buf, size, markers[i])) { found = TRUE; break; }
    HeapFree(GetProcessHeap(), 0, buf);
    if (!found) return;

    warned = TRUE;
    sprintf(text,
            "This looks like a legacy 16-bit installer that may unpack a 32-bit setup engine:\n%s\n\n"
            "otvdm can redirect C:\\ paths only for 16-bit code. The installer may write to the real C: drive, not to <otvdm>\\C.\n\n"
            "Use a sandbox/VM, or choose an install directory under <otvdm>\\C.",
            filename);
    user32 = GetModuleHandleA("user32.dll");
    mb = user32 ? (MESSAGEBOXA)GetProcAddress(user32, "MessageBoxA") : NULL;
    if (mb) mb(NULL, text, "otvdm - legacy installer", 0x00000030L);
}

BOOL is_readonly_directory(LPCSTR path)
{
    DWORD attr = GetFileAttributesA(path);
    if (attr == -1)
        return FALSE;
    if (GetDriveTypeA(path) == DRIVE_CDROM)
        return TRUE;
    return (attr & FILE_ATTRIBUTE_READONLY) == FILE_ATTRIBUTE_READONLY;
}
__declspec(dllexport) LPCSTR RedirectDriveRoot(LPCSTR path, LPSTR to, size_t max_len, BOOL silence)
{
    LPCSTR path_old = path;
    LPCSTR fail_ret;
    char shortpath[MAX_PATH];
    char drive;
    const char *rest;
    const char *base;
    char full[MAX_PATH];
    char canonical[MAX_PATH];
    char leaf[2];

    if (!path) return NULL;
    /* Fast path: only the configured drive letter can be virtual. Relative
     * names, other drives and UNC paths are returned untouched, so the host
     * file API keeps its normal meaning and no syscall is spent on them. */
    if (!(path[0] && path[1] == ':' && (path[2] == 0 || path[2] == '\\' || path[2] == '/')))
        return path;
    drive = path[0];
    if (drive >= 'a' && drive <= 'z') drive -= 'a' - 'A';
    if (drive < 'A' || drive > 'Z') return path;
    load_virtual_drive();
    if (!virtual_drive || drive != virtual_drive) return path;

    fail_ret = (path_old == to) ? to : path_old;
    /* Canonicalize the 16-bit path first: ".." components then collapse inside
     * the virtual root instead of rejecting the whole path, which used to leak
     * it through to the real C: drive. */
    if (!PathCanonicalizeA(canonical, path)) return fail_ret;
    if (canonical[1] != ':') return fail_ret;
    base = GetVirtualDriveBase();
    if (!base[0]) return fail_ret;
    /* Paths that already point inside the otvdm directory are real paths, not
     * virtual-drive paths. This keeps the redirect idempotent when the
     * deployment itself lives on the virtual drive letter. The short-name form
     * is checked too, because a long input path such as C:\winevdm\test-deploy
     * would not match C:\winevdm\TEST-D~1. */
    {
        size_t base_len = strlen(base);
        if (strlen(path) >= base_len && _strnicmp(path, base, base_len) == 0 &&
            (path[base_len] == '\\' || path[base_len] == '\0'))
            return fail_ret;
        if (GetShortPathNameA(path, shortpath, sizeof(shortpath)) &&
            strlen(shortpath) >= base_len && _strnicmp(shortpath, base, base_len) == 0 &&
            (shortpath[base_len] == '\\' || shortpath[base_len] == '\0'))
            return fail_ret;
    }

    rest = canonical + 2;
    if (*rest == '\\' || *rest == '/') rest++;
    leaf[0] = drive;
    leaf[1] = 0;
    if (!PathCombineA(full, base, leaf)) return fail_ret;
    CreateDirectoryA(full, NULL);   /* <otvdm>\C is the virtual drive root */
    if (*rest && !PathCombineA(full, full, rest)) return fail_ret;
    if (!PathCanonicalizeA(canonical, full)) return fail_ret;
    if (strlen(canonical) >= max_len) return fail_ret;
    strcpy(to, canonical);
    if (!silence) ERR("%s => %s\n", path_old, canonical);
    return to;
}
/* buf: <otvdm>\C\ZZZ => C:\ZZZ */
void UnredirectDriveRoot(LPSTR buf, SIZE_T buf_len)
{
    const char *base;
    const char *src;
    const char *drive_dir;
    char shortbuf[MAX_PATH];
    char tmp[MAX_PATH];
    size_t base_len;

    if (!buf || !buf_len) return;
    load_virtual_drive();
    if (!virtual_drive) return;     /* nothing was mapped, nothing to undo */
    base = GetVirtualDriveBase();
    if (!base[0]) return;
    base_len = strlen(base);
    src = buf;
    /* Accept both the long and the short form of the deployment path; the
     * redirect produces the short one, but GetModuleFileNameA/cwd callers may
     * hand us the long one. Comparison is case insensitive, like the redirect. */
    if (!(strlen(buf) > base_len && _strnicmp(buf, base, base_len) == 0 && buf[base_len] == '\\'))
    {
        if (!GetShortPathNameA(buf, shortbuf, sizeof(shortbuf))) return;
        if (!(strlen(shortbuf) > base_len && _strnicmp(shortbuf, base, base_len) == 0 &&
              shortbuf[base_len] == '\\'))
            return;
        src = shortbuf;
    }

    drive_dir = src + base_len + 1;     /* <otvdm>\C\... -> "C\..." */
    if (!((drive_dir[0] >= 'A' && drive_dir[0] <= 'Z') ||
          (drive_dir[0] >= 'a' && drive_dir[0] <= 'z')))
        return;
    if (drive_dir[1] != '\\' && drive_dir[1] != '\0') return;

    tmp[0] = drive_dir[0] >= 'a' ? drive_dir[0] - 'a' + 'A' : drive_dir[0];
    tmp[1] = ':';
    tmp[2] = '\\';
    if (drive_dir[1] == '\0') tmp[3] = '\0';
    else strcpy(tmp + 3, drive_dir + 2);
    strncpy(buf, tmp, buf_len - 1);
    buf[buf_len - 1] = '\0';
}

/* buf: <otvdm>\WINDOWS\SYSTEM => C:\WINDOWS\SYSTEM */
void UnredirectSystemDir(LPSTR buf, SIZE_T buf_len)
{
    const char *real = GetRedirectWindowsDir();
    const char *virt = GetVirtualWindowsDir();
    const char *src = buf;
    char shortbuf[MAX_PATH];
    size_t len;
    char tmp[MAX_PATH];

    if (!buf || !buf_len) return;
    len = strlen(real);
    if (!(strlen(buf) >= len && _strnicmp(buf, real, len) == 0 &&
          (buf[len] == '\\' || buf[len] == '\0')))
    {
        /* the caller may hold the long form of the same directory */
        if (!GetShortPathNameA(buf, shortbuf, sizeof(shortbuf))) return;
        if (!(strlen(shortbuf) >= len && _strnicmp(shortbuf, real, len) == 0 &&
              (shortbuf[len] == '\\' || shortbuf[len] == '\0')))
            return;
        src = shortbuf;
    }
    if (strlen(virt) + strlen(src + len) >= sizeof(tmp)) return;
    strcpy(tmp, virt);
    strcat(tmp, src + len);
    strncpy(buf, tmp, buf_len - 1);
    buf[buf_len - 1] = '\0';
}

/* TRUE when path is exactly `prefix` or starts with `prefix` followed by a
 * separator. PathCommonPrefixA alone would also accept "C:\WINDOWSX" as being
 * inside "C:\WINDOWS", which mapped a sibling directory into the Windows dir. */
static BOOL path_has_dir_prefix(LPCSTR path, LPCSTR prefix)
{
    size_t n = strlen(prefix);
    if (strlen(path) < n || _strnicmp(path, prefix, n)) return FALSE;
    return path[n] == '\\' || path[n] == '/' || path[n] == '\0';
}
/*
 * C:\WINDOWS-> redirected windows dir
 * C:\WINDOWS\SYSTEM32 -> redirected windows dir\SYSTEM
 */
__declspec(dllexport) LPCSTR RedirectSystemDir(LPCSTR path, LPSTR to, size_t max_len)
{
	char dirbuf[MAX_PATH];
	if (!EnableRedirectSystemDir) return path;
	char buf[MAX_PATH];
	if (!PathCanonicalizeA(buf, path))
	{
		return path;
	}
	char bufwdir[MAX_PATH];
    strcpy(bufwdir, GetVirtualWindowsDir());
    size_t windir_len = strlen(bufwdir);
	if (!path_has_dir_prefix(buf, bufwdir))
	{
		/* Without the virtual drive the 16-bit view of the Windows directory is
		 * the real <otvdm>\WINDOWS, but code that hard-codes C:\WINDOWS was
		 * redirected before this feature existed. Keep honouring it, otherwise
		 * switching the virtual drive off would not be a full rollback. */
		if (virtual_drive) return path;
		if (!GetWindowsDirectoryA(bufwdir, sizeof(bufwdir))) return path;
		windir_len = strlen(bufwdir);
		if (!path_has_dir_prefix(buf, bufwdir)) return path;
	}
	if (!PathAppendA(bufwdir, "SYSTEM32"))
	{
		return path;
	}
	//system32->system
	if (path_has_dir_prefix(buf, bufwdir))
	{
		memcpy(buf + strlen(bufwdir) - 2, buf + strlen(bufwdir), strlen(buf) - strlen(bufwdir) + 1);
	}
	//.\windir\ 
	dirbuf[0] = '\0';
	char *dir = &dirbuf[0];
	GetModuleFileNameA(GetModuleHandleA("otvdm.exe"), dir, sizeof(dirbuf));
	char *file = PathFindFileNameA(dir);
	if (file != dir) *file = '\0';
	if (!PathAppendA(dir, GetRedirectWindowsDir()))
		return path;
	if (!PathAppendA(dir, buf + windir_len))
		return path;
	if (strlen(dir) >= max_len)
		return path;
	file = PathFindFileNameA(dir);
	char f = *file;
	*file = '\0';
	GetShortPathNameA(dir, to, max_len);
	*file = f;
	PathCombineA(to, to, file);
	return to;
}

/* Map a 16-bit path through both the Windows-directory redirect and the
 * virtual drive. Two buffers are required: RedirectSystemDir may return b1,
 * and RedirectDriveRoot must not write into the same buffer it is reading. */
static LPCSTR RedirectPath16(LPCSTR path, LPSTR b1, LPSTR b2, size_t len)
{
    LPCSTR p = RedirectSystemDir(path, b1, len);
    return RedirectDriveRoot(p, b2, len, TRUE);
}

/* Same mapping, exported for the other DLLs (mmsystem, storage, shell, ...)
 * which have to map the file names they hand to the host as well. `to` must
 * hold at least max_len bytes. */
__declspec(dllexport) LPCSTR RedirectVirtualDrivePath(LPCSTR path, LPSTR to, size_t max_len)
{
    char b1[MAX_PATH];
    if (!path) return path;
    if (max_len > MAX_PATH) max_len = MAX_PATH;
    return RedirectPath16(path, b1, to, max_len);
}

NTSYSAPI NTSTATUS NTAPI RtlUpcaseUnicodeStringToOemString(
    POEM_STRING      DestinationString,
    PCUNICODE_STRING SourceString,
    BOOLEAN          AllocateDestinationString
);
static LPWSTR strdupAtoW(LPCSTR str)
{
    LPWSTR ret;
    INT len;

    if (!str) return NULL;
    len = MultiByteToWideChar(CP_ACP, 0, str, -1, NULL, 0);
    ret = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
    if (ret) MultiByteToWideChar(CP_ACP, 0, str, -1, ret, len);
    return ret;
}
static LPWSTR strdupOEMtoW(LPCSTR str)
{
    LPWSTR ret;
    INT len;

    if (!str) return NULL;
    len = MultiByteToWideChar(CP_OEMCP, 0, str, -1, NULL, 0);
    ret = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));
    if (ret) MultiByteToWideChar(CP_OEMCP, 0, str, -1, ret, len);
    return ret;
}
static LPCSTR strdupWtoOEM(LPCWSTR str)
{
    LPSTR ret;
    INT len;

    if (!str) return NULL;
    len = WideCharToMultiByte(CP_OEMCP, 0, str, -1, NULL, 0, NULL, NULL);
    ret = HeapAlloc(GetProcessHeap(), 0, len * sizeof(CHAR));
    if (ret) WideCharToMultiByte(CP_OEMCP, 0, str, -1, ret, len, NULL, NULL);
    return ret;
}

#define DOS_TABLE_SIZE 256

static HANDLE *FILE_GetTable()
{
    static DWORD dos_handles_index = NULL;
    if (!dos_handles_index) dos_handles_index = TlsAlloc();
    HANDLE *dos_handles = TlsGetValue(dos_handles_index);
    if (!dos_handles)
    {
        dos_handles = (HANDLE *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(HANDLE)*256);
        TlsSetValue(dos_handles_index, dos_handles);
    }
    return dos_handles;
}

// only call at task exit
void FILE_CloseAll()
{
    HANDLE *dos_handles = FILE_GetTable();
    for (int i = 0; i < 256; i++)
    {
        if (dos_handles[i])
            CloseHandle(dos_handles[i]);
    }
    HeapFree(GetProcessHeap(), 0, dos_handles);
}

BOOL is_cdrom_file(LPCSTR name)
{
    char path[MAX_PATH];
    if(!GetFullPathNameA(name, MAX_PATH, &path, NULL))
        return FALSE;
    path[3] = 0;
    return GetDriveTypeA(path) == DRIVE_CDROM;
}

/***********************************************************************
 *           FILE_InitProcessDosHandles
 *
 * Allocates the default DOS handles for a process. Called either by
 * Win32HandleToDosFileHandle below or by the DOSVM stuff.
 */
static void FILE_InitProcessDosHandles( void )
{
    HANDLE hStdInput, hStdOutput, hStdError, hNull;
    static BOOL init_done /* = FALSE */;
    HANDLE cp = GetCurrentProcess();

    if (init_done) return;
    HANDLE *dos_handles = FILE_GetTable();
    init_done = TRUE;
    hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    hStdError = GetStdHandle(STD_ERROR_HANDLE);
    hNull = CreateFileA("NUL", GENERIC_READ|GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, 0);
    /* Invalid console handles need to translate to real DOS handles in a new process */
    if (!hStdInput) hStdInput = hNull;
    if (!hStdOutput) hStdOutput = hNull;
    if (!hStdError) hStdError = hNull;
    DuplicateHandle(cp, hStdInput, cp, &dos_handles[0], 0, TRUE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(cp, hStdOutput, cp, &dos_handles[1], 0, TRUE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(cp, hStdError, cp, &dos_handles[2], 0, TRUE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(cp, hStdError, cp, &dos_handles[3], 0, TRUE, DUPLICATE_SAME_ACCESS);
    DuplicateHandle(cp, hStdError, cp, &dos_handles[4], 0, TRUE, DUPLICATE_SAME_ACCESS);
    CloseHandle(hNull);
}

/***********************************************************************
 *           DosFileHandleToWin32Handle   (KERNEL32.20)
 *
 * Return the Win32 handle for a DOS handle.
 *
 * Note: this is not exactly right, since on Win95 the Win32 handles
 *       are on top of DOS handles and we do it the other way
 *       around. Should be good enough though.
 */
HANDLE WINAPI DosFileHandleToWin32Handle( HFILE handle )
{
    HFILE16 hfile = (HFILE16)handle;
    if (hfile < 5) FILE_InitProcessDosHandles();
    HANDLE *dos_handles = FILE_GetTable();
    if ((hfile >= DOS_TABLE_SIZE) || !dos_handles[hfile])
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return INVALID_HANDLE_VALUE;
    }
    return dos_handles[hfile];
}

/***********************************************************************
 *           Win32HandleToDosFileHandle   (KERNEL32.21)
 *
 * Allocate a DOS handle for a Win32 handle. The Win32 handle is no
 * longer valid after this function (even on failure).
 *
 * Note: this is not exactly right, since on Win95 the Win32 handles
 *       are on top of DOS handles and we do it the other way
 *       around. Should be good enough though.
 */
HFILE WINAPI Win32HandleToDosFileHandle( HANDLE handle )
{
    int i;

    if (!handle || (handle == INVALID_HANDLE_VALUE))
        return HFILE_ERROR;
    HANDLE *dos_handles = FILE_GetTable();
    for (i = 5; i < DOS_TABLE_SIZE; i++)
        if (dos_handles[i] == handle) return (HFILE)i;
    for (i = 5; i < DOS_TABLE_SIZE; i++)
        if (!dos_handles[i])
        {
            dos_handles[i] = handle;
            TRACE("Got %d for h32 %p\n", i, handle );
            return (HFILE)i;
        }
    CloseHandle( handle );
    SetLastError( ERROR_TOO_MANY_OPEN_FILES );
    return HFILE_ERROR;
}

/***********************************************************************
 *           DisposeLZ32Handle   (KERNEL32.22)
 *
 * Note: this is not entirely correct, we should only close the
 *       32-bit handle and not the 16-bit one, but we cannot do
 *       this because of the way our DOS handles are implemented.
 *       It shouldn't break anything though.
 */
BOOL16 WINAPI DeleteDosFileHandle( HANDLE handle )
{
    int i;

    if (!handle || (handle == INVALID_HANDLE_VALUE)) return;

    HANDLE *dos_handles = FILE_GetTable();
    for (i = 5; i < DOS_TABLE_SIZE; i++)
        if (dos_handles[i] == handle)
        {
            dos_handles[i] = 0;
            return TRUE;
        }
    return FALSE;
}

/***********************************************************************
 *           GetProfileInt   (KERNEL.57)
 */
UINT16 WINAPI GetProfileInt16( LPCSTR section, LPCSTR entry, INT16 def_val )
{
    return GetPrivateProfileInt16( section, entry, def_val, "win.ini" );
}

int CALLBACK enum_ini_font(const LOGFONT *lf, const TEXTMETRIC *tm, DWORD type, LPARAM lParam)
{
    char *name = (char *)lParam;
    return strncmp(name, lf->lfFaceName, strlen(lf->lfFaceName));
}

/***********************************************************************
 *           GetProfileString   (KERNEL.58)
 */
INT16 WINAPI GetProfileString16( LPCSTR section, LPCSTR entry, LPCSTR def_val,
                                 LPSTR buffer, UINT16 len )
{
    char tmp[256];
    int tmplen = 256;
    if (section && entry && !stricmp(section, "devices") && !stricmp(entry, "DefaultPrinter"))
    {
        if (GetDefaultPrinterA(tmp, &tmplen))
            entry = tmp;
    }
    INT16 ret = GetPrivateProfileString16( section, entry, def_val,
                                      buffer, len, "win.ini" );
    if (ret && section && entry && !stricmp(section, "windows") && !stricmp(entry, "device"))
    {
        char *comma = strchr(buffer, ',');
        if (!comma && ((len - 1) == ret))
        {
            ret = GetPrivateProfileString16( section, entry, def_val,
                                      tmp, tmplen, "win.ini" );
            comma = strchr(tmp, ',');
            if (comma && (len > (strlen(comma) + 14)))
            {
                strcpy(buffer, "DefaultPrinter");
                strcpy(buffer + 14, comma);
            }
        }
        else if (comma && ((comma - buffer) > 32))
        {
            strcpy(buffer, "DefaultPrinter");
            strcpy(buffer + 14, comma);
        }
        ret = strlen(buffer);
    }
    else if (!ret && section && !stricmp(section, "fonts"))
    {
        LOGFONTA lf;
        HDC hdc = GetDC(0);
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfPitchAndFamily = 0;
        lf.lfFaceName[0] = 0;
        ret = EnumFontFamiliesExA(hdc, &lf, enum_ini_font, entry, 0);
        ReleaseDC(0, hdc);
        if (!ret)
            strncpy(buffer, "vgaoem.fon", len); // actual filename is unavailable
        return !ret ? strnlen(buffer, len) : 0;
    }
    else if (ret && !PathIsRelativeA(buffer) && PathFileExistsA(buffer))
    {
        int cnt = GetShortPathNameA(buffer, tmp, len <= 256 ? len : 256);
        if (cnt)
            strcpy(buffer, tmp);
    }
    return ret;
}

/***********************************************************************
 *           WriteProfileString   (KERNEL.59)
 */
BOOL16 WINAPI WriteProfileString16( LPCSTR section, LPCSTR entry,
                                    LPCSTR string )
{
    return WritePrivateProfileString16( section, entry, string, "win.ini" );
}


/* get the search path for the current module; helper for OpenFile16 */
/*static */char *get_search_path(void)
{
    UINT len, i;
    char *ret, *p, module[OFS_MAXPATHNAME];

    module[0] = 0;
    if (GetCurrentTask() && GetModuleFileName16( GetCurrentTask(), module, sizeof(module) ))
    {
        if (!(p = strrchr( module, '\\' ))) p = module;
        *p = 0;
    }
    char windir[MAX_PATH];
    char windir2[MAX_PATH];
    char realwinsys[MAX_PATH];
    char *windir3;
    GetWindowsDirectoryA(windir, MAX_PATH);
    windir3 = RedirectSystemDir(windir, windir2, MAX_PATH);
    char vdmpath[MAX_PATH];
    GetModuleFileNameA(GetModuleHandleA(NULL), vdmpath, MAX_PATH);
    PathRemoveFileSpecA(vdmpath);
    GetShortPathNameA(vdmpath, vdmpath, MAX_PATH);
    strcpy(realwinsys, windir);
    strcat(realwinsys, "\\SYSTEM");
    char realsys[MAX_PATH];
    PathCombineA(realsys, GetRedirectWindowsDir(), "SYSTEM");

    len = (strlen(vdmpath) + 1 + 2 +                                              /* search order: first current dir */
           strlen(realsys) + 2 +                            /* then the redirected system dir */
           strlen(windir3) + 1 +            /* then windows dir */
           strlen( module ) + 1 +                           /* then module path */
           GetEnvironmentVariableA( "PATH16", NULL, 0 ) + 1 + /* then look in PATH */
           strlen(realwinsys) + 1); /* then the real windows system dir */
    if (!(ret = HeapAlloc( GetProcessHeap(), 0, len ))) return NULL;
    strcpy(ret, ".;");
    p = ret + 2;
    strcpy(p, vdmpath);
    p += strlen(vdmpath);
    *p++ = ';';

    strcpy( p, realsys );
    p += strlen( p );
    *p++ = ';';
    strcpy(p, windir3);//GetWindowsDirectoryA( p, ret + len - p );
    p += strlen( p );
    *p++ = ';';
    if (module[0])
    {
        strcpy( p, module );
        p += strlen( p );
        *p++ = ';';
    }
    i = GetEnvironmentVariableA("PATH16", p, ret + len - p);
    if (i)
    {
        p += i;
        *p++ = ';';
    }
    strcpy(p, realwinsys);
    return ret;
}
char *krnl386_get_search_path(void)
{
    return get_search_path();
}
#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>
LPCSTR krnl386_search_executable_file(LPCSTR lpFile, LPSTR buf, SIZE_T size, BOOL search_builtin)
{
    char *path = krnl386_get_search_path();
    LPCSTR result = lpFile;
    if (SearchPathA(path, lpFile, NULL, size, buf, NULL))
    {
        result = buf;
    }
    else
    {
        char dlldir[MAX_PATH];
        GetDllDirectoryA(MAX_PATH, dlldir);
        if (SearchPathA(dlldir, lpFile, NULL, size, buf, NULL))
        {
            result = buf;
        }
    }
    if (result == lpFile && search_builtin)
    {
        char filewin32[MAX_PATH];
        LPSTR ext = PathFindExtensionA(lpFile);
        if (ext && !stricmp(ext, ".dll") || !stricmp(ext, ".exe") || !stricmp(ext, ".drv"))
        {
            filewin32[0] = '\0';
            if (StringCbCatA(filewin32, ARRAYSIZE(filewin32), lpFile) == S_OK &&
                StringCbCatA(filewin32, ARRAYSIZE(filewin32), "16") == S_OK)
            {
                if (SearchPathA(path, filewin32, NULL, size, buf, NULL))
                {
                    result = buf;
                }
                else
                {
                    char dlldir[MAX_PATH];
                    GetDllDirectoryA(MAX_PATH, dlldir);
                    if (SearchPathA(dlldir, filewin32, NULL, size, buf, NULL))
                    {
                        result = buf;
                    }
                }
            }
        }
    }
    HeapFree(GetProcessHeap(), 0, path);
    return result;
}

/***********************************************************************
 *              create_file_OF
 *
 * Wrapper for CreateFile that takes OF_* mode flags.
 */
static HANDLE create_file_OF( LPCWSTR path, INT mode )
{
    DWORD access, sharing, creation;

    if (mode & OF_CREATE)
    {
        creation = CREATE_ALWAYS;
        access = GENERIC_READ | GENERIC_WRITE;
    }
    else
    {
        creation = OPEN_EXISTING;
        switch(mode & 0x03)
        {
        case OF_READ:      access = GENERIC_READ; break;
        case OF_WRITE:     access = GENERIC_WRITE; break;
        case OF_READWRITE: access = GENERIC_READ | GENERIC_WRITE; break;
        default:           access = 0; break;
        }
    }

    switch(mode & 0x70)
    {
    case OF_SHARE_EXCLUSIVE:  sharing = 0; break;
    case OF_SHARE_DENY_WRITE: sharing = FILE_SHARE_READ; break;
    case OF_SHARE_DENY_READ:  sharing = FILE_SHARE_WRITE; break;
    case OF_SHARE_DENY_NONE:
    case OF_SHARE_COMPAT:
    default:                  sharing = FILE_SHARE_READ | FILE_SHARE_WRITE; break;
    }
    return CreateFileW( path, access, sharing | FILE_SHARE_DELETE, NULL, creation, FILE_ATTRIBUTE_NORMAL, 0 );
}
/***********************************************************************
 *           OpenFile   (KERNEL.74)
 *           OpenFileEx (KERNEL.360)
 */
HFILE16 WINAPI WIN16_OpenFile16( LPCSTR name, SEGPTR ofs, UINT16 mode )
{
    /* Win 3.1 validates the file name pointer and fails instead of faulting */
    if (name && !(mode & OF_REOPEN) && IsBadStringPtrA(name, OFS_MAXPATHNAME)) return HFILE_ERROR16;
    CURRENT_STACK16->es = SELECTOROF(ofs);
    return OpenFile16(name, MapSL(ofs), mode);
}

HFILE16 WINAPI OpenFile16( LPCSTR name, OFSTRUCT *ofs, UINT16 mode )
{
    HFILE hFileRet = 0;
    HANDLE handle = INVALID_HANDLE_VALUE;
    FILETIME filetime;
    WORD filedatetime[2];
    OEM_STRING oem;
    UNICODE_STRING uni;
    FILETIME lfiletime;
    WCHAR pathname[sizeof(ofs->szPathName)];
    LPWSTR namew;
    const WCHAR *p, *filename;
    CHAR buf[OFS_MAXPATHNAME];
    CHAR redir[MAX_PATH];
    BOOL result;
    oem.Buffer = ofs->szPathName;
    oem.Length = 0;
    oem.MaximumLength = sizeof(ofs->szPathName);

    if (!ofs) return HFILE_ERROR;

    TRACE("%s %s %s %s%s%s%s%s%s%s%s%s\n",debugstr_a(name),
          ((mode & 0x3 )==OF_READ)?"OF_READ":
          ((mode & 0x3 )==OF_WRITE)?"OF_WRITE":
          ((mode & 0x3 )==OF_READWRITE)?"OF_READWRITE":"unknown",
          ((mode & 0x70 )==OF_SHARE_COMPAT)?"OF_SHARE_COMPAT":
          ((mode & 0x70 )==OF_SHARE_DENY_NONE)?"OF_SHARE_DENY_NONE":
          ((mode & 0x70 )==OF_SHARE_DENY_READ)?"OF_SHARE_DENY_READ":
          ((mode & 0x70 )==OF_SHARE_DENY_WRITE)?"OF_SHARE_DENY_WRITE":
          ((mode & 0x70 )==OF_SHARE_EXCLUSIVE)?"OF_SHARE_EXCLUSIVE":"unknown",
          ((mode & OF_PARSE )==OF_PARSE)?"OF_PARSE ":"",
          ((mode & OF_DELETE )==OF_DELETE)?"OF_DELETE ":"",
          ((mode & OF_VERIFY )==OF_VERIFY)?"OF_VERIFY ":"",
          ((mode & OF_SEARCH )==OF_SEARCH)?"OF_SEARCH ":"",
          ((mode & OF_CANCEL )==OF_CANCEL)?"OF_CANCEL ":"",
          ((mode & OF_CREATE )==OF_CREATE)?"OF_CREATE ":"",
          ((mode & OF_PROMPT )==OF_PROMPT)?"OF_PROMPT ":"",
          ((mode & OF_EXIST )==OF_EXIST)?"OF_EXIST ":"",
          ((mode & OF_REOPEN )==OF_REOPEN)?"OF_REOPEN ":""
        );
    name = RedirectSystemDir(name, redir, MAX_PATH);

    if ((mode & (OF_CREATE | OF_SHARE_EXCLUSIVE)) == (OF_CREATE | OF_SHARE_EXCLUSIVE))
    {
        mode &= ~OF_SHARE_EXCLUSIVE;
    }

    {
        ofs->cBytes = sizeof(OFSTRUCT);
        ofs->nErrCode = 0;
        if (mode & OF_REOPEN) name = ofs->szPathName;

        if (!name)
        {
            return HFILE_ERROR;
        }
        name = RedirectDriveRoot(name, buf, ARRAY_SIZE(buf), TRUE);
        if (mode & OF_REOPEN)
            namew = strdupOEMtoW(name);
        else
            namew = strdupAtoW(name);

        /* the watcom 10.6 IDE relies on a valid path returned in ofs->szPathName
           Are there any cases where getting the path here is wrong?
           Uwe Bonnes 1997 Apr 2 */
        if (!*namew)
        {
            /* empty path => cwd */
            result = GetCurrentDirectoryW(sizeof(ofs->szPathName), pathname);
            if (result)
            {
                size_t len = wcslen(pathname);
                if (len < sizeof(ofs->szPathName) - 1 && pathname[len - 1] != '\\' && pathname[len - 1] != '/')
                {
                    pathname[len] = '\\';
                    pathname[len + 1] = 0;
                }
            }
            if (result && !(mode & OF_PARSE))
            {
                SetLastError(ERROR_FILE_NOT_FOUND);
                RtlInitUnicodeString(&uni, pathname);
                if (!NT_SUCCESS(RtlUpcaseUnicodeStringToOemString(&oem, &uni, FALSE)))
                {
                    ERR("RtlUpcaseUnicodeStringToOemString failed\n");
                    ofs->szPathName[0] = '\0';
                }
                goto error;
            }
        }
        else
        {
            result = GetFullPathNameW(namew, sizeof(ofs->szPathName), pathname, NULL);
        }
        if (!result)
        {
            RtlInitUnicodeString(&uni, namew);
            if (!NT_SUCCESS(RtlUpcaseUnicodeStringToOemString(&oem, &uni, FALSE)))
            {
                ERR("RtlUpcaseUnicodeStringToOemString failed\n");
                ofs->szPathName[0] = '\0';
            }
            goto error;
        }

        /* If OF_SEARCH is set, ignore the given path */

        filename = namew;
        if ((mode & OF_SEARCH) && !(mode & OF_REOPEN))
        {
            /* First try the file name as is */
            if (GetFileAttributesW( filename ) != INVALID_FILE_ATTRIBUTES) filename = NULL;
            else
            {
                /* Now remove the path */
                if (filename[0] && (filename[1] == ':')) filename += 2;
                if ((p = wcsrchr( filename, '\\' ))) filename = p + 1;
                if ((p = wcsrchr( filename, '/' ))) filename = p + 1;
                if (!filename[0])
                {
                    RtlInitUnicodeString(&uni, filename);
                    if (!NT_SUCCESS(RtlUpcaseUnicodeStringToOemString(&oem, &uni, FALSE)))
                    {
                        ERR("RtlUpcaseUnicodeStringToOemString failed\n");
                        ofs->szPathName[0] = '\0';
                    }
                    SetLastError( ERROR_FILE_NOT_FOUND );
                    goto error;
                }
            }
        }

        /* Now look for the file */

        if (!(mode & OF_PARSE) && !(mode & OF_CREATE) && filename)
        {
            BOOL found;
            char *path = get_search_path();
            WCHAR *pathw;

            if (!path)
            {
                RtlInitUnicodeString(&uni, filename);
                if (!NT_SUCCESS(RtlUpcaseUnicodeStringToOemString(&oem, &uni, FALSE)))
                {
                    ERR("RtlUpcaseUnicodeStringToOemString failed\n");
                    ofs->szPathName[0] = '\0';
                }
                goto error;
            }
            if (!wcsicmp(pathname, L"\\\\.\\nul"))
                found = TRUE;
            else
            {
                pathw = strdupAtoW(path);
                found = SearchPathW( pathw, filename, NULL, sizeof(ofs->szPathName),
                                     pathname, NULL );
                HeapFree( GetProcessHeap(), 0, path );
                HeapFree( GetProcessHeap(), 0, pathw );
            }
            if (!found)
            {
                RtlInitUnicodeString(&uni, pathname);
                if (!NT_SUCCESS(RtlUpcaseUnicodeStringToOemString(&oem, &uni, FALSE)))
                {
                    ERR("RtlUpcaseUnicodeStringToOemString failed\n");
                    ofs->szPathName[0] = '\0';
                }
                goto error;
            }
        }

        TRACE("found %s\n", debugstr_w(pathname) );

        if (mode & OF_DELETE)
        {
            RtlInitUnicodeString(&uni, pathname);
            if (!NT_SUCCESS(RtlUpcaseUnicodeStringToOemString(&oem, &uni, FALSE)))
            {
                ERR("RtlUpcaseUnicodeStringToOemString failed\n");
                ofs->szPathName[0] = '\0';
            }
            if (!DeleteFileW(pathname)) goto error;
            TRACE("(%s): OF_DELETE return = OK\n", name);
            HeapFree(GetProcessHeap(), 0, namew);
            return 1;
        }

        GetShortPathNameW(pathname, pathname, ARRAY_SIZE(pathname));
        if (pathname[0] && pathname[wcslen(pathname) - 1] == '\\')
        {
            SetLastError(ERROR_FILE_NOT_FOUND);
            handle = INVALID_HANDLE_VALUE;
            hFileRet = HFILE_ERROR16;
        }
        else if (!(mode & OF_PARSE))
        {
            handle = create_file_OF(pathname, mode);
        }
        RtlInitUnicodeString(&uni, pathname);
        if (!NT_SUCCESS(RtlUpcaseUnicodeStringToOemString(&oem, &uni, FALSE)))
        {
            ERR("RtlUpcaseUnicodeStringToOemString failed\n");
            ofs->szPathName[0] = '\0';
        }
        UnredirectSystemDir(ofs->szPathName, ARRAY_SIZE(ofs->szPathName));
        UnredirectDriveRoot(ofs->szPathName, ARRAY_SIZE(ofs->szPathName));
        if (!(mode & OF_PARSE))
        {
            if (handle == INVALID_HANDLE_VALUE) goto error;
        }

        if (mode & OF_PARSE)
        {
            SYSTEMTIME systime;
            GetSystemTime(&systime);
            SystemTimeToFileTime(&systime, &filetime);
        }
        else
        {
            GetFileTime( handle, NULL, NULL, &filetime );
        }
        FileTimeToLocalFileTime(&filetime, &lfiletime);
        FileTimeToDosDateTime( &lfiletime, &filedatetime[0], &filedatetime[1] );
        if ((mode & OF_VERIFY) && (mode & OF_REOPEN))
        {
            if (ofs->Reserved1 != filedatetime[0] || ofs->Reserved2 != filedatetime[1] )
            {
                CloseHandle( handle );
                WARN("(%s): OF_VERIFY failed\n", name );
                /* FIXME: what error here? */
                SetLastError( ERROR_FILE_NOT_FOUND );
                goto error;
            }
        }
        ofs->Reserved1 = filedatetime[0];
        ofs->Reserved2 = filedatetime[1];
    }

    if (!(mode & OF_PARSE))
    {
        hFileRet = Win32HandleToDosFileHandle(handle);
        TRACE("(%s): OK, return = %p\n", name, handle);
    }
    if (hFileRet == HFILE_ERROR16) goto error;
    HeapFree(GetProcessHeap(), 0, namew);
    if (!(mode & OF_PARSE) && mode & OF_EXIST) _lclose16( hFileRet ); /* Return the handle, but close it first */
    return hFileRet;

error:  /* We get here if there was an error opening the file */
    ofs->nErrCode = GetLastError();
    WARN("(%s): return = HFILE_ERROR error= %d\n", name,ofs->nErrCode );
    HeapFree(GetProcessHeap(), 0, namew);
    return HFILE_ERROR16;
}


/***********************************************************************
 *           _lclose   (KERNEL.81)
 */
HFILE16 WINAPI _lclose16( HFILE16 hFile )
{
    HANDLE *dos_handles = FILE_GetTable();
    if ((hFile >= DOS_TABLE_SIZE) || !dos_handles[hFile])
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return HFILE_ERROR16;
    }
    TRACE("%d (handle32=%p)\n", hFile, dos_handles[hFile] );
    CloseHandle( dos_handles[hFile] );
    dos_handles[hFile] = 0;
    return 0;
}

/***********************************************************************
 *           _lcreat   (KERNEL.83)
 */
HFILE16 WINAPI _lcreat16( LPCSTR path, INT16 attr )
{
    char b1[MAX_PATH], b2[MAX_PATH];
    LPCSTR p = RedirectPath16(path, b1, b2, MAX_PATH);
    return Win32HandleToDosFileHandle( (HANDLE)_lcreat( p, attr ) );
}

/***********************************************************************
 *           _llseek   (KERNEL.84)
 */
LONG WINAPI _llseek16( HFILE16 hFile, LONG lOffset, INT16 nOrigin )
{
    HANDLE fd = DosFileHandleToWin32Handle(hFile);
    LONG high = 0;
    DWORD offset = (DWORD)lOffset;
    switch (nOrigin)
    {
        case FILE_BEGIN:
            break;
        case FILE_END:
            offset += GetFileSize(fd, NULL);
            break;
        case FILE_CURRENT:
            offset += SetFilePointer(fd, 0, NULL, FILE_CURRENT);
            break;
        default:
            SetLastError(ERROR_INVALID_PARAMETER);
            return -1;
    }
    return SetFilePointer(fd, offset, &high, FILE_BEGIN);
}


/***********************************************************************
 *           _lopen   (KERNEL.85)
 */
HFILE16 WINAPI _lopen16( LPCSTR path, INT16 mode )
{
    HFILE fh;
    char b1[MAX_PATH], b2[MAX_PATH];
    LPCSTR p = RedirectPath16(path, b1, b2, MAX_PATH);
    fh =_lopen(p, mode);
    if((fh == INVALID_HANDLE_VALUE) && (GetLastError() == ERROR_ACCESS_DENIED) && is_cdrom_file(p))
        fh = _lopen(p, OF_READ);
    return Win32HandleToDosFileHandle((HANDLE)fh);
}


/***********************************************************************
 *           _lread16 (internal)
 */
UINT16 WINAPI _lread16( HFILE16 hFile, LPVOID buffer, UINT16 count )
{
    return (UINT16)_lread((HFILE)DosFileHandleToWin32Handle(hFile), buffer, (LONG)count );
}


/***********************************************************************
 *           _lwrite   (KERNEL.86)
 */
UINT16 WINAPI _lwrite16( HFILE16 hFile, LPCSTR buffer, UINT16 count )
{
    return (UINT16)_hwrite( (HFILE)DosFileHandleToWin32Handle(hFile), buffer, (LONG)count );
}

/***********************************************************************
 *           _hread (KERNEL.349)
 */
LONG WINAPI WIN16_hread( HFILE16 hFile, SEGPTR buffer, LONG count )
{
    LONG maxlen;

    TRACE("%d %08x %d\n", hFile, (DWORD)buffer, count );

    /* Some programs pass a count larger than the allocated buffer */
    maxlen = GetSelectorLimit16( SELECTOROF(buffer) ) - OFFSETOF(buffer) + 1;
    if (count > maxlen)
    {
        LPVOID temp_buffer = HeapAlloc(GetProcessHeap(), 0, count);
        HFILE result = _lread((HFILE)DosFileHandleToWin32Handle(hFile), temp_buffer, count );
        if (result != HFILE_ERROR)
        {
            memcpy(MapSL(buffer), temp_buffer, (size_t)result);
        }
        HeapFree(GetProcessHeap(), 0, temp_buffer);
        return result;
    }

    return _lread((HFILE)DosFileHandleToWin32Handle(hFile), MapSL(buffer), count );
}


/***********************************************************************
 *           _lread (KERNEL.82)
 */
UINT16 WINAPI WIN16_lread( HFILE16 hFile, SEGPTR buffer, UINT16 count )
{
    return (UINT16)WIN16_hread( hFile, buffer, (LONG)count );
}


/***********************************************************************
 *           _hwrite   (KERNEL.350)
 */
LONG WINAPI _hwrite16( HFILE16 hFile, LPCSTR buffer, LONG count )
{
    return _hwrite( (HFILE)DosFileHandleToWin32Handle(hFile), buffer, count );
}


/***********************************************************************
 *           GetTempDrive   (KERNEL.92)
 * A closer look at krnl386.exe shows what the SDK doesn't mention:
 *
 * returns:
 *   AL: driveletter
 *   AH: ':'		- yes, some kernel code even does stosw with
 *                            the returned AX.
 *   DX: 1 for success
 */
UINT WINAPI GetTempDrive( BYTE ignored )
{
    WCHAR buffer[MAX_PATH];
    BYTE ret;

    if (GetTempPathW( MAX_PATH, buffer )) ret = (BYTE)toupperW(buffer[0]);
    else ret = 'C';
    return MAKELONG( ret | (':' << 8), 1 );
}


#define MAX_TEMP_PATH 144
/***********************************************************************
 *           GetTempFileName   (KERNEL.97)
 */
UINT16 WINAPI GetTempFileName16( BYTE drive, LPCSTR prefix, UINT16 unique,
                                 LPSTR buffer )
{
    char temppath[MAX_TEMP_PATH];
    char *unique_buf;

    if (!(drive & ~TF_FORCEDRIVE)) /* drive 0 means current default drive */
    {
        WCHAR curdir[MAX_PATH];
        DWORD r = GetCurrentDirectoryW(MAX_PATH, curdir);
        if (r >= MAX_PATH || r == 0 || curdir[1] != ':')
        {
            ERR("could not get current drive\n");
            return 0;
        }
        drive |= curdir[0];
    }

    if (drive & TF_FORCEDRIVE)
    {
        /* C:~preuuuu.TMP */
        char root[4] = "C:\\";
        char mapped[MAX_TEMP_PATH];
        root[0] = drive & ~TF_FORCEDRIVE;
        /* A forced drive keeps the temp file in that drive's root; with the
         * virtual drive that root is <otvdm>\<letter>, not the real one. */
        if (RedirectDriveRoot(root, mapped, sizeof(mapped), TRUE) != root)
        {
            snprintf(temppath, sizeof(temppath), "%s", mapped);
            PathAddBackslashA(temppath);
        }
        else
        {
            snprintf(temppath, sizeof(temppath), "%c:", drive & ~TF_FORCEDRIVE);
        }
    }
    else
    {
        DWORD r = GetTempPathA(MAX_TEMP_PATH, temppath);
        if (r >= MAX_TEMP_PATH)
        {
            ERR("temp path is too long\n");
            return 0;
        }
        if (r < 3)
        {
            return 0;
        }
    }
    GetShortPathNameA(temppath, temppath, MAX_TEMP_PATH);
    if (strlen(temppath) + 12 /* ~preuuuu.TMP */ >= MAX_TEMP_PATH - 1)
    {
        ERR("temp path is too long\n");
        return 0;
    }
    unique_buf = temppath + strlen(temppath);
    *unique_buf++ = '~';
    if (prefix[0])
    {
        *unique_buf++ = prefix[0];
        if (prefix[1])
        {
            *unique_buf++ = prefix[1];
            if (prefix[2])
            {
                *unique_buf++ = prefix[2];
            }
        }
    }

    if (!unique)
    {
        int num = GetTickCount() & 0xffff;
        if (num == 0)
        {
            num = 1;
        }
        unique = num;
        do
        {
            HANDLE handle;
            sprintf(unique_buf, "%04X.TMP", unique);

            handle = CreateFileA(temppath, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, 0);
            if (handle != INVALID_HANDLE_VALUE)
            {
                CloseHandle(handle);
                break;
            }
            if (GetLastError() == ERROR_ACCESS_DENIED)
            {
                ERR("%s ERROR_ACCESS_DENIED\n", temppath);
            }
            if (GetLastError() != ERROR_FILE_EXISTS &&
                GetLastError() != ERROR_SHARING_VIOLATION)
                break;
            if (!(++unique & 0xffff)) unique = 1;
        } while (unique != num);
    }
    else
    {
        sprintf(unique_buf, "%04X.TMP", unique);
    }
    if (strlen(temppath) + 1 > MAX_TEMP_PATH)
    {
        ERR("temp path is too long\n");
        return 0;
    }
    else
    {
        strcpy(buffer, temppath);
    }

    return unique;
}


typedef const char* (*ini_redirect_str_func)();
typedef int(*ini_redirect_int_func)();
struct ini_redirect_data
{
    const char *file;
    const char *section;
    const char *entry;
    const char *value;
    ini_redirect_str_func get_str;
    ini_redirect_int_func get_int;
    BOOL overwrite_section;
};
int system_ini_keyboard_type()
{
    return GetKeyboardType(0);
}
int system_ini_keyboard_subtype()
{
    return GetKeyboardType(1);
}
static const char *system_ini_boot_language_dll()
{
    switch (GetUserDefaultUILanguage())
    {
        case 1030:
        case 1035:
        case 1039:
        case 1044:
        case 1053:
            return "langsca.dll";
        case 1043:
            return "langdut.dll";
        case 2057:
        case 3084:
        case 1040:
        case 2070:
        case 3082:
            return "langeng.dll";
        case 1036:
            return "langfrn.dll";
        case 1031:
            return "langger.dll";
        case 1034:
            return "langspa.dll";
        case 1033:
        default:
            return "";
    }
}
const char *system_init_boot_description_language_dll()
{
    LANGID lang = GetUserDefaultUILanguage();
    static char buf[256];
    if (GetLocaleInfoA(LOCALE_USER_DEFAULT, LOCALE_SNATIVELANGNAME, buf, ARRAY_SIZE(buf)))
    {
        return buf;
    }
    return "English (American)";
}
const char *system_init_boot_description_keyboard_typ()
{
    char buf[KL_NAMELENGTH];
    if (GetKeyboardLayoutNameA(buf))
    {
        HKEY hkey;
        HKEY hkey2;
        static char buf2[256];
        DWORD cb = ARRAY_SIZE(buf2);
        if (RegOpenKeyA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts\\", &hkey) == ERROR_SUCCESS)
        {
            if (RegOpenKeyA(hkey, buf, &hkey2) == ERROR_SUCCESS)
            {
                LRESULT result = RegQueryValueExA(hkey2, "Layout Text", NULL, NULL, buf2, &cb);
                RegCloseKey(hkey2);
                RegCloseKey(hkey);
                if (result == ERROR_SUCCESS)
                {
                    return buf2;
                }
            }
            else
            {
                RegCloseKey(hkey);
            }
        }
    }
    return "Enhanced 101 or 102 key US and Non US keyboards";
}
int system_init_boot_description_codepage()
{
    return GetACP();
}
/* HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\WOW */
/* HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\IniFileMapping */
/* HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\IniFileMapping */
struct ini_redirect_data ini_redirect_list[] =
{
    {"system.ini", "boot", "sound.drv", "sound.drv"},
    {"system.ini", "boot", "comm.drv", "comm.drv"},
    {"system.ini", "boot", "keyboard.drv", "keyboard.drv"},
    {"system.ini", "boot", "system.drv", "system.drv"},
    {"system.ini", "boot", "display.drv", "display.drv"},
    {"system.ini", "boot", "shell", "progman.exe"},
    {"system.ini", "boot", "mouse.drv", "mouse.drv"},
    {"system.ini", "boot", "network.drv", "network.drv"},
    {"system.ini", "boot", "language.dll", NULL, system_ini_boot_language_dll},
    {"system.ini", "boot", "drivers", "mmsystem.dll"},
    /* {"system.ini", "boot", "oemfonts.fon", "", NULL}, */
    /* {"system.ini", "boot", "fixedfon.fon", "", NULL}, */
    /* {"system.ini", "boot", "fonts.fon", "", NULL}, */


    {"system.ini", "keyboard", "type", NULL, NULL, system_ini_keyboard_type},
    {"system.ini", "keyboard", "subtype", NULL, NULL, system_ini_keyboard_subtype},
    {"system.ini", "keyboard", "keyboard.dll", ""},

    {"system.ini", "boot.description", "system.drv", "MS-DOS or PC-DOS System"},
    /* {"system.ini", "boot.description", "aspect", "100,96,96"}, */
    {"system.ini", "boot.description", "display.drv", "VGA"},
    {"system.ini", "boot.description", "keyboard.typ", NULL, system_init_boot_description_keyboard_typ},
    {"system.ini", "boot.description", "mouse.drv", "Microsoft, or IBM PS/2"},
    {"system.ini", "boot.description", "network.drv", "LAN Support"},
    {"system.ini", "boot.description", "language.dll", NULL, system_init_boot_description_language_dll},
    {"system.ini", "boot.description", "codepage", NULL, NULL, system_init_boot_description_codepage},
    /* {"system.ini", "boot.description", "woafont.fon", ""}, */

    /* HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\MCI (Only available in 32-bit windows) */
    {"system.ini", "mci", "AVIVideo", "mciavi.drv"},

    {"system.ini", "mci", "Sequencer", "mciseq.drv"},
    {"system.ini", "mci", "CDAudio", "mcicda.drv"},
    {"system.ini", "mci", "WaveAudio", "mciwave.drv"},

    /* MPEGVideo! */
    {"system.ini", "mci", "MPEGVideo", "mciqtz.drv"},

    /*
    HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\MCI Extensions
    HKEY_LOCAL_MACHINE\SOFTWARE\WOW6432Node\Microsoft\Windows NT\CurrentVersion\MCI Extensions
    */
#if 0
    {"win.ini", "mci extensions", "aiff", "MPEGVideo"},
    {"win.ini", "mci extensions", "dat", "MPEGVideo"},
    {"win.ini", "mci extensions", "m2t", "MPEGVideo"},
    {"win.ini", "mci extensions", "mpa", "MPEGVideo"},
    {"win.ini", "mci extensions", "wmx", "MPEGVideo"},
    {"win.ini", "mci extensions", "Mid", "MPEGVideo"},
    {"win.ini", "mci extensions", "wmv", "MPEGVideo"},
    {"win.ini", "mci extensions", "m3u", "MPEGVideo"},
    {"win.ini", "mci extensions", "avi", "MPEGVideo"},
    {"win.ini", "mci extensions", "ivf", "MPEGVideo"},
    {"win.ini", "mci extensions", "wvx", "MPEGVideo"},
    {"win.ini", "mci extensions", "m4v", "MPEGVideo"},
    {"win.ini", "mci extensions", "mts", "MPEGVideo"},
    {"win.ini", "mci extensions", "mp4v", "MPEGVideo"},
    {"win.ini", "mci extensions", "mp2v", "MPEGVideo"},
    {"win.ini", "mci extensions", "adts", "MPEGVideo"},
    {"win.ini", "mci extensions", "wma", "MPEGVideo"},
    {"win.ini", "mci extensions", "mpeg", "MPEGVideo"},
    {"win.ini", "mci extensions", "tts", "MPEGVideo"},
    {"win.ini", "mci extensions", "mpv2", "MPEGVideo"},
    {"win.ini", "mci extensions", "au", "MPEGVideo"},
    {"win.ini", "mci extensions", "3gpp", "MPEGVideo"},
    {"win.ini", "mci extensions", "m4a", "MPEGVideo"},
    {"win.ini", "mci extensions", "wax", "MPEGVideo"},
    {"win.ini", "mci extensions", "aif", "MPEGVideo"},
    {"win.ini", "mci extensions", "asx", "MPEGVideo"},
    {"win.ini", "mci extensions", "Wav", "MPEGVideo"},
    {"win.ini", "mci extensions", "m2ts", "MPEGVideo"},
    {"win.ini", "mci extensions", "mov", "MPEGVideo"},
    {"win.ini", "mci extensions", "wpl", "MPEGVideo"},
    {"win.ini", "mci extensions", "aac", "MPEGVideo"},
    {"win.ini", "mci extensions", "3gp2", "MPEGVideo"},
    {"win.ini", "mci extensions", "mp4", "MPEGVideo"},
    {"win.ini", "mci extensions", "mp3", "MPEGVideo"},
    {"win.ini", "mci extensions", "mp2", "MPEGVideo"},
    {"win.ini", "mci extensions", "wm", "MPEGVideo"},
    {"win.ini", "mci extensions", "adt", "MPEGVideo"},
    {"win.ini", "mci extensions", "cda", "MPEGVideo"},
    {"win.ini", "mci extensions", "3g2", "MPEGVideo"},
    {"win.ini", "mci extensions", "asf", "MPEGVideo"},
    {"win.ini", "mci extensions", "mod", "MPEGVideo"},
    {"win.ini", "mci extensions", "m1v", "MPEGVideo"},
    {"win.ini", "mci extensions", "ts", "MPEGVideo"},
    {"win.ini", "mci extensions", "rmi", "MPEGVideo"},
    {"win.ini", "mci extensions", "mpg", "MPEGVideo"},
    {"win.ini", "mci extensions", "3gp", "MPEGVideo"},
    {"win.ini", "mci extensions", "aifc", "MPEGVideo"},
    {"win.ini", "mci extensions", "mpe", "MPEGVideo"},
    {"win.ini", "mci extensions", "m2v", "MPEGVideo"},
    {"win.ini", "mci extensions", "snd", "MPEGVideo"},
#else
    {"win.ini", "mci extensions", "Mid", "Sequencer"},
    {"win.ini", "mci extensions", "Wav", "WaveAudio"},
    {"win.ini", "mci extensions", "avi", "avivideo"},
    {"win.ini", "mci extensions", "cda", "CDAudio"},

    {"win.ini", "mci extensions", "rmi", "Sequencer"},
    {"win.ini", "mci extensions", "wma", "MPEGVideo"},
    {"win.ini", "mci extensions", "m4v", "MPEGVideo"},
    {"win.ini", "mci extensions", "m4a", "MPEGVideo"},
    {"win.ini", "mci extensions", "aac", "MPEGVideo"},
    {"win.ini", "mci extensions", "mp4", "MPEGVideo"},
    {"win.ini", "mci extensions", "mov", "MPEGVideo"},
    {"win.ini", "mci extensions", "mp3", "MPEGVideo"},
    {"win.ini", "mci extensions", "mpg", "MPEGVideo"},
    {"win.ini", "mci extensions", "mp2", "MPEGVideo"},
    {"win.ini", "mci extensions", "dat", "MPEGVideo"},
    {"win.ini", "mci extensions", "m2t", "MPEGVideo"},
    {"win.ini", "mci extensions", "mpa", "MPEGVideo"},
    {"win.ini", "mci extensions", "wmx", "MPEGVideo"},
    {"win.ini", "mci extensions", "wmv", "MPEGVideo"},
    {"win.ini", "mci extensions", "m3u", "MPEGVideo"},
    {"win.ini", "mci extensions", "ivf", "MPEGVideo"},
    {"win.ini", "mci extensions", "mts", "MPEGVideo"},
    {"win.ini", "mci extensions", "au", "MPEGVideo"},
    {"win.ini", "mci extensions", "mpe", "MPEGVideo"},
    {"win.ini", "mci extensions", "mod", "MPEGVideo"},
    {"win.ini", "mci extensions", "aif", "MPEGVideo"},
    /* When there are many extensions, MPLAYER causes buffer overflow. */
    /*
    {"win.ini", "mci extensions", "wax", "MPEGVideo"},
    {"win.ini", "mci extensions", "asx", "MPEGVideo"},
    {"win.ini", "mci extensions", "wpl", "MPEGVideo"},
    {"win.ini", "mci extensions", "wm", "MPEGVideo"},
    {"win.ini", "mci extensions", "adt", "MPEGVideo"},
    {"win.ini", "mci extensions", "3g2", "MPEGVideo"},
    {"win.ini", "mci extensions", "asf", "MPEGVideo"},
    {"win.ini", "mci extensions", "m1v", "MPEGVideo"},
    {"win.ini", "mci extensions", "tts", "MPEGVideo"},
    {"win.ini", "mci extensions", "ts", "MPEGVideo"},
    {"win.ini", "mci extensions", "wvx", "MPEGVideo"},
    {"win.ini", "mci extensions", "3gp", "MPEGVideo"},
    {"win.ini", "mci extensions", "m2v", "MPEGVideo"},
    {"win.ini", "mci extensions", "snd", "MPEGVideo"},

    {"win.ini", "mci extensions", "mpeg", "MPEGVideo"},
    {"win.ini", "mci extensions", "mp4v", "MPEGVideo"},
    {"win.ini", "mci extensions", "mp2v", "MPEGVideo"},
    {"win.ini", "mci extensions", "adts", "MPEGVideo"},
    {"win.ini", "mci extensions", "aifc", "MPEGVideo"},
    {"win.ini", "mci extensions", "3gp2", "MPEGVideo"},
    {"win.ini", "mci extensions", "m2ts", "MPEGVideo"},
    {"win.ini", "mci extensions", "aiff", "MPEGVideo"},
    {"win.ini", "mci extensions", "mpv2", "MPEGVideo"},
    {"win.ini", "mci extensions", "3gpp", "MPEGVideo"},
    */
#endif
    /* overwrite section */
    {"win.ini", "mci extensions", NULL, NULL, NULL, NULL, TRUE},
};

/***********************************************************************
 *           GetPrivateProfileInt   (KERNEL.127)
 */
UINT16 WINAPI GetPrivateProfileInt16( LPCSTR section, LPCSTR entry,
                                      INT16 def_val, LPCSTR filename )
{
    /* we used to have some elaborate return value limitation (<= -32768 etc.)
     * here, but Win98SE doesn't care about this at all, so I deleted it.
     * AFAIR versions prior to Win9x had these limits, though. */
    char ini[MAX_PATH];

    LPCSTR filename_file = PathFindFileNameA(filename);
    for (int i = 0; i < ARRAY_SIZE(ini_redirect_list); i++)
    {
        if (!ini_redirect_list[i].entry)
        {
            continue;
        }
        if (!stricmp(ini_redirect_list[i].file, filename_file) && !stricmp(section, ini_redirect_list[i].section) && !stricmp(entry, ini_redirect_list[i].entry))
        {
            if (ini_redirect_list[i].get_int)
                return ini_redirect_list[i].get_int();
        }
    }
    RedirectPrivateProfileStringWindowsDir(filename,ini);
    return (INT16)GetPrivateProfileIntA(section,entry,def_val,ini);
}


char windowsPath[MAX_PATH];
const char *GetRedirectWindowsDir()
{
    if (*windowsPath != '\0')
        return windowsPath;
#ifdef ENABLEREDIRECTSYSTEMDIR
    GetModuleFileNameA(GetModuleHandleA(NULL), windowsPath, MAX_PATH);
    PathRemoveFileSpecA(windowsPath);
    char ini[MAX_PATH];
    krnl386_get_config_string("otvdm", "WINDIR", "WINDOWS", ini, sizeof(ini));
    PathCombineA(windowsPath, windowsPath, ini);
    GetShortPathNameA(windowsPath, windowsPath, sizeof(windowsPath));
    PathRemoveBackslashA(windowsPath);
    if (strstr(windowsPath, "\\\\") || strstr(windowsPath, "/"))
    {
        ERR("WINDIR %s is not a correct path.\n", windowsPath);
    }
    return windowsPath;
#else
    GetWindowsDirectoryA(windowsPath, sizeof(windowsPath));
    GetShortPathNameA(windowsPath, windowsPath, sizeof(windowsPath));
    return windowsPath;
#endif
}

static void RedirectPrivateProfileStringWindowsDir(LPCSTR filename, LPCSTR output)
{
    char buf[MAX_PATH];
    LPCSTR mapped;
    if (!filename)
        filename = "win.ini";
    if (PathIsFileSpecA(filename) && !strchr(filename, '/'))
    {
        PathCombineA(buf, GetRedirectWindowsDir(), filename);
    }
    else
    {
        strcpy(buf, filename);
    }
    /* A profile file named by a full 16-bit path must follow the virtual drive
     * as well, otherwise C:\APP\*.INI would be written to the real C: drive. */
    mapped = RedirectDriveRoot(buf, (LPSTR)output, MAX_PATH, TRUE);
    if (mapped != output)
        lstrcpynA((LPSTR)output, mapped, MAX_PATH);
}
static BOOL append_ini_section(LPSTR data, SIZE_T *buf_pos, SIZE_T len, LPCSTR entry)
{
    SIZE_T ent_len = strlen(entry) + 1;
    memcpy(data + *buf_pos, entry, min(ent_len, len - *buf_pos - 2));
    *buf_pos += min(ent_len, len - *buf_pos - 2);
    data[*buf_pos] = '\0';
    data[*buf_pos + 1] = '\0';
    return TRUE;
}
static int construct_redirected_ini_section(LPCSTR section, LPSTR data, UINT16 size, LPCSTR filename)
{
    LPCSTR filename_file = PathFindFileNameA(filename);
    SIZE_T buf_pos = 0;
    if (size == 0)
    {
        return 0;
    }
    if (size == 1)
    {
        data[0] = '\0';
        return 0;
    }
    data[0] = '\0';
    data[1] = '\0';
    if (size == 2)
    {
        return 0;
    }
    for (int i = 0; i < ARRAY_SIZE(ini_redirect_list); i++)
    {
        if (!ini_redirect_list[i].entry)
        {
            continue;
        }
        if (!stricmp(ini_redirect_list[i].file, filename_file) && !stricmp(section, ini_redirect_list[i].section))
        {
            if (!append_ini_section(data, &buf_pos, size, ini_redirect_list[i].entry))
            {
                break;
            }
        }
    }
    return buf_pos;
}
/***********************************************************************
 *           GetPrivateProfileString   (KERNEL.128)
 */
INT16 WINAPI GetPrivateProfileString16( LPCSTR section, LPCSTR entry,
                                        LPCSTR def_val, LPSTR buffer,
                                        UINT16 len, LPCSTR filename )
{
    char filenamebuf[MAX_PATH];
    BOOL overwrite_section = FALSE;
    TRACE("%s %s %s\n", filename, section, entry);
    if (!section || !filename)
    {
        if (buffer && len) buffer[0] = 0;
        return 0;
    }
    LPCSTR filename_file = PathFindFileNameA(filename);
    if (entry)
    {
        for (int i = 0; i < ARRAY_SIZE(ini_redirect_list); i++)
        {
            if (!ini_redirect_list[i].entry)
            {
                continue;
            }
            if (!stricmp(ini_redirect_list[i].file, filename_file) && !stricmp(section, ini_redirect_list[i].section) && !stricmp(entry, ini_redirect_list[i].entry))
            {
                LPCSTR val = ini_redirect_list[i].value;
                SIZE_T size;
                if (!val && ini_redirect_list[i].get_str)
                {
                    val = ini_redirect_list[i].get_str();
                }
                if (!val)
                {
                    break;
                }
                if (len == 0)
                {
                    return 0;
                }
                else if (len == 1)
                {
                    buffer[0] = '\0';
                    return 0;
                }
                size = min(strlen(val), len - 1);
                memcpy(buffer, val, size);
                buffer[size] = '\0';
                return strlen(buffer);
            }
        }
    }
    else
    {
        for (int i = 0; i < ARRAY_SIZE(ini_redirect_list); i++)
        {
            if (ini_redirect_list[i].entry)
            {
                continue;
            }
            if (ini_redirect_list[i].overwrite_section && !stricmp(ini_redirect_list[i].file, filename_file) && !stricmp(section, ini_redirect_list[i].section))
            {
                overwrite_section = TRUE;
            }
        }
    }
    RedirectPrivateProfileStringWindowsDir(filename, filenamebuf);
    filename = filenamebuf;
    TRACE("(%s, %s, %s, %p, %u, %s)\n", debugstr_a(section), debugstr_a(entry),
          debugstr_a(def_val), buffer, len, debugstr_a(filename));

    /* len = 0 means unlimited buffer length (windows bug?) */
    if (!entry && len == 0)
    {
        len = 0xffff;
    }
    if (!entry)
    {
        /* We have to return the list of keys in the section but without the values
         * so we need to massage the results of GetPrivateProfileSectionA.
         */
        UINT ret, oldlen = len, size = min( len, 1024 );
        LPSTR data, src;

        if (overwrite_section)
            return construct_redirected_ini_section(section, buffer, oldlen, filename);
        for (;;)
        {
            if (!(data = HeapAlloc(GetProcessHeap(), 0, size ))) return 0;
            ret = GetPrivateProfileSectionA( section, data, size, filename );
            if (!ret)
            {
                HeapFree( GetProcessHeap(), 0, data );
                ret = construct_redirected_ini_section(section, buffer, oldlen, filename);
                if (!ret)
                    ret = GetPrivateProfileStringA(section, entry, def_val, buffer, len, filename);
                return ret;
            }
            if (ret != size - 2) break;
            /* overflow, try again */
            size *= 2;
            HeapFree( GetProcessHeap(), 0, data );
        }

        src = data;
        while (len && *src)
        {
            char *p = strchr( src, '=' );

            /* A valid entry is formed by name = value */
            if (!p)
            {
                src += strlen(src) + 1;
                continue;
            }
            if (p - src < len)
            {
                memcpy( buffer, src, p - src );
                buffer += p - src;
                *buffer++ = 0;
                len -= (p - src) + 1;
                src += strlen(src) + 1;
            }
            else  /* overflow */
            {
                memcpy( buffer, src, len );
                buffer += len;
                len = 0;
            }
        }
        HeapFree( GetProcessHeap(), 0, data );

        if (len)
        {
            *buffer = 0;
            return oldlen - len;
        }
        if (oldlen > 2)
        {
            buffer[-2] = 0;
            buffer[-1] = 0;
            return oldlen - 2;
        }
        return 0;
    }
    return GetPrivateProfileStringA( section, entry, def_val, buffer, len, filename );
}

static BOOL16 check_write_profile_error(LPCSTR filename, DWORD error)
{
    if (error == ERROR_ACCESS_DENIED)
    {
        OFSTRUCT ofstr;
        HFILE ret = OpenFile(filename, &ofstr, OF_EXIST);
        if (ret != HFILE_ERROR)
        {
            WARN("Failed to write profile data because it was redirected to the registry");
            return TRUE; // fake success, any written data can't be read anyway
        }
    }
    return FALSE;
}

/***********************************************************************
 *           WritePrivateProfileString   (KERNEL.129)
 */
BOOL16 WINAPI WritePrivateProfileString16( LPCSTR section, LPCSTR entry,
                                           LPCSTR string, LPCSTR filename )
{
    char filenamebuf[MAX_PATH];
    RedirectPrivateProfileStringWindowsDir(filename, &filenamebuf);
    filename = filenamebuf;
    BOOL ret = WritePrivateProfileStringA(section,entry,string,filename);
    if (!ret)
        return check_write_profile_error(filename, GetLastError());
    return ret;
}


/***********************************************************************
 *           GetWindowsDirectory   (KERNEL.134)
 */
UINT16 WINAPI GetWindowsDirectory16( LPSTR path, UINT16 count )
{
    const char *w = GetVirtualWindowsDir();
    UINT16 len = strlen(w);
    if (len + 1 > count)
        return strlen(w) + 1;
    strcpy(path, w);
    return len;
    //return GetWindowsDirectoryA( path, count );
}


/***********************************************************************
 *           GetSystemDirectory   (KERNEL.135)
 */
UINT16 WINAPI GetSystemDirectory16( LPSTR path, UINT16 count )
{
    static const char system16[] = "\\SYSTEM";
    char windir[MAX_PATH];
    UINT16 len;

    len = GetWindowsDirectory16(windir, sizeof(windir) - sizeof(system16) + 1) + sizeof(system16);
    if (count >= len)
    {
        lstrcpyA(path, windir);
        lstrcatA(path, system16);
        len--;  /* space for the terminating zero is not included on success */
    }
    return len;
}


/***********************************************************************
 *           GetDriveType   (KERNEL.136)
 * Get the type of a drive in Win16.
 *
 * RETURNS
 *  The type of the Drive. For a list see GetDriveTypeW from kernel32.
 *
 * NOTES
 *  Note that it returns DRIVE_REMOTE for CD-ROMs, since MSCDEX uses the
 *  remote drive API. The return value DRIVE_REMOTE for CD-ROMs has been
 *  verified on Win 3.11 and Windows 95. Some programs rely on it, so don't
 *  do any pseudo-clever changes.
 */
UINT16 WINAPI GetDriveType16( UINT16 drive ) /* [in] number (NOT letter) of drive */
{
    UINT type;
    WCHAR root[3];

    root[0] = 'A' + drive;
    root[1] = ':';
    root[2] = 0;
    type = GetDriveTypeW( root );
    if (type == DRIVE_CDROM) type = DRIVE_REMOTE;
    else if (type == DRIVE_NO_ROOT_DIR) type = DRIVE_UNKNOWN;
    return type;
}


/***********************************************************************
 *           GetProfileSectionNames   (KERNEL.142)
 */
WORD WINAPI GetProfileSectionNames16(LPSTR buffer, WORD size)

{
    return GetPrivateProfileSectionNamesA(buffer,size,"win.ini");
}


/***********************************************************************
 *           GetPrivateProfileSectionNames   (KERNEL.143)
 */
WORD WINAPI GetPrivateProfileSectionNames16( LPSTR buffer, WORD size,
                                             LPCSTR filename )
{
    return GetPrivateProfileSectionNamesA(buffer,size,filename);
}


/***********************************************************************
 *           CreateDirectory   (KERNEL.144)
 */
BOOL16 WINAPI CreateDirectory16( LPCSTR path, LPVOID dummy )
{
    char b1[MAX_PATH], b2[MAX_PATH];
    LPCSTR p = RedirectPath16(path, b1, b2, MAX_PATH);
    return CreateDirectoryA( p, NULL );
}


/***********************************************************************
 *           RemoveDirectory   (KERNEL.145)
 */
BOOL16 WINAPI RemoveDirectory16( LPCSTR path )
{
    char b1[MAX_PATH], b2[MAX_PATH];
    LPCSTR p = RedirectPath16(path, b1, b2, MAX_PATH);
    return RemoveDirectoryA( p );
}


/***********************************************************************
 *           DeleteFile   (KERNEL.146)
 */
BOOL16 WINAPI DeleteFile16( LPCSTR path )
{
    char b1[MAX_PATH], b2[MAX_PATH];
    LPCSTR p = RedirectPath16(path, b1, b2, MAX_PATH);
    return DeleteFileA( p );
}


/***********************************************************************
 *           SetHandleCount   (KERNEL.199)
 */
UINT16 WINAPI SetHandleCount16( UINT16 count )
{
    return SetHandleCount( count );
}


/***********************************************************************
 *           GetShortPathName   (KERNEL.274)
 */
WORD WINAPI GetShortPathName16( LPCSTR longpath, LPSTR shortpath, WORD len )
{
    char b1[MAX_PATH], b2[MAX_PATH];
    LPCSTR p = RedirectPath16(longpath, b1, b2, MAX_PATH);
    WORD r = GetShortPathNameA( p, shortpath, len );
    if (r && shortpath && len)
    {
        UnredirectSystemDir(shortpath, len);
        UnredirectDriveRoot(shortpath, len);
    }
    return r;
}


/***********************************************************************
 *           WriteOutProfiles   (KERNEL.315)
 */
void WINAPI WriteOutProfiles16(void)
{
    WritePrivateProfileSectionW( NULL, NULL, NULL );
}


/***********************************************************************
 *           WritePrivateProfileStruct (KERNEL.406)
 */
BOOL16 WINAPI WritePrivateProfileStruct16 (LPCSTR section, LPCSTR key,
                                           LPVOID buf, UINT16 bufsize, LPCSTR filename)
{
    char filenamebuf[MAX_PATH];
    RedirectPrivateProfileStringWindowsDir(filename, &filenamebuf);
    filename = filenamebuf;
    return WritePrivateProfileStructA( section, key, buf, bufsize, filename );
}


/***********************************************************************
 *           GetPrivateProfileStruct (KERNEL.407)
 */
BOOL16 WINAPI GetPrivateProfileStruct16(LPCSTR section, LPCSTR key,
                                        LPVOID buf, UINT16 len, LPCSTR filename)
{
    char filenamebuf[MAX_PATH];
    RedirectPrivateProfileStringWindowsDir(filename, &filenamebuf);
    filename = filenamebuf;
    return GetPrivateProfileStructA( section, key, buf, len, filename );
}


/***********************************************************************
 *           GetCurrentDirectory   (KERNEL.411)
 */
UINT16 WINAPI GetCurrentDirectory16( UINT16 buflen, LPSTR buf )
{
    UINT16 r = GetCurrentDirectoryA( buflen, buf );
    if (r && buf && buflen)
    {
        UnredirectSystemDir(buf, buflen);
        UnredirectDriveRoot(buf, buflen);
    }
    return r;
}


/***********************************************************************
 *           SetCurrentDirectory   (KERNEL.412)
 */
BOOL16 WINAPI SetCurrentDirectory16( LPCSTR dir )
{
    char b1[MAX_PATH], b2[MAX_PATH];
    char realwd[MAX_PATH];
    char virt[MAX_PATH];
    LPCSTR p = RedirectPath16(dir, b1, b2, MAX_PATH);

    if (!SetCurrentDirectoryA( p )) return FALSE;
    if (!GetCurrentDirectoryA( MAX_PATH, realwd )) return TRUE;
    /* Keep the TDB in the same short, drive-relative form TASK_Create uses:
     * "\APP" for a subdirectory, "" for a drive root. */
    GetShortPathNameA( realwd, realwd, sizeof(realwd) );
    strcpy( virt, realwd );
    UnredirectSystemDir( virt, MAX_PATH );
    UnredirectDriveRoot( virt, MAX_PATH );

    if (virt[0] && virt[1] == ':')
    {
        TDB *pTask = GlobalLock16( GetCurrentTask() );
        char env_var[4] = "=A:";

        env_var[1] = virt[0];
        SetEnvironmentVariableA( env_var, virt );

        if (pTask)
        {
            pTask->curdrive = 0x80 | (virt[0] - 'A');
            lstrcpynA( pTask->curdir, virt + 2, sizeof(pTask->curdir) );
        }
    }
    return TRUE;
}


/*************************************************************************
 *           FindFirstFile   (KERNEL.413)
 */
HANDLE16 WINAPI FindFirstFile16( LPCSTR path, WIN32_FIND_DATAA *data )
{
    HGLOBAL16 h16;
    HANDLE handle, *ptr;
    char b1[MAX_PATH], b2[MAX_PATH];
    LPCSTR p = RedirectPath16(path, b1, b2, MAX_PATH);

    if (!(h16 = GlobalAlloc16( GMEM_MOVEABLE, sizeof(handle) ))) return INVALID_HANDLE_VALUE16;
    ptr = GlobalLock16( h16 );
    *ptr = handle = FindFirstFileA( p, data );
    GlobalUnlock16( h16 );

    if (handle == INVALID_HANDLE_VALUE)
    {
        GlobalFree16( h16 );
        h16 = INVALID_HANDLE_VALUE16;
    }
    return h16;
}


/*************************************************************************
 *           FindNextFile   (KERNEL.414)
 */
BOOL16 WINAPI FindNextFile16( HANDLE16 handle, WIN32_FIND_DATAA *data )
{
    HANDLE *ptr;
    BOOL ret = FALSE;

    if ((handle == INVALID_HANDLE_VALUE16) || !(ptr = GlobalLock16( handle )))
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return ret;
    }
    ret = FindNextFileA( *ptr, data );
    GlobalUnlock16( handle );
    return ret;
}


/*************************************************************************
 *           FindClose   (KERNEL.415)
 */
BOOL16 WINAPI FindClose16( HANDLE16 handle )
{
    HANDLE *ptr;

    if ((handle == INVALID_HANDLE_VALUE16) || !(ptr = GlobalLock16( handle )))
    {
        SetLastError( ERROR_INVALID_HANDLE );
        return FALSE;
    }
    FindClose( *ptr );
    GlobalUnlock16( handle );
    GlobalFree16( handle );
    return TRUE;
}


/***********************************************************************
 *           WritePrivateProfileSection   (KERNEL.416)
 */
BOOL16 WINAPI WritePrivateProfileSection16( LPCSTR section,
                                            LPCSTR string, LPCSTR filename )
{
    return WritePrivateProfileSectionA( section, string, filename );
}


/***********************************************************************
 *           WriteProfileSection   (KERNEL.417)
 */
BOOL16 WINAPI WriteProfileSection16( LPCSTR section, LPCSTR keys_n_values)
{
    return WritePrivateProfileSection16( section, keys_n_values, "win.ini");
}


/***********************************************************************
 *           GetPrivateProfileSection   (KERNEL.418)
 */
INT16 WINAPI GetPrivateProfileSection16( LPCSTR section, LPSTR buffer,
                                         UINT16 len, LPCSTR filename )
{
    return GetPrivateProfileSectionA( section, buffer, len, filename );
}


/***********************************************************************
 *           GetProfileSection   (KERNEL.419)
 */
INT16 WINAPI GetProfileSection16( LPCSTR section, LPSTR buffer, UINT16 len )
{
    return GetPrivateProfileSection16( section, buffer, len, "win.ini" );
}


/**************************************************************************
 *           GetFileAttributes   (KERNEL.420)
 */
DWORD WINAPI GetFileAttributes16( LPCSTR name )
{
    char b1[MAX_PATH], b2[MAX_PATH];
    LPCSTR p = RedirectPath16(name, b1, b2, MAX_PATH);
    return GetFileAttributesA( p );
}


/**************************************************************************
 *              SetFileAttributes	(KERNEL.421)
 */
BOOL16 WINAPI SetFileAttributes16( LPCSTR lpFileName, DWORD attributes )
{
    char b1[MAX_PATH], b2[MAX_PATH];
    LPCSTR p = RedirectPath16(lpFileName, b1, b2, MAX_PATH);
    return SetFileAttributesA( p, attributes );
}


/***********************************************************************
 *           GetDiskFreeSpace   (KERNEL.422)
 */
BOOL16 WINAPI GetDiskFreeSpace16( LPCSTR root, LPDWORD cluster_sectors,
                                  LPDWORD sector_bytes, LPDWORD free_clusters,
                                  LPDWORD total_clusters )
{
    return GetDiskFreeSpaceA( root, cluster_sectors, sector_bytes,
                                free_clusters, total_clusters );
}

/***********************************************************************
 *	FileCDR (KERNEL.130)
 */
FARPROC16 WINAPI FileCDR16(FARPROC16 x)
{
    FIXME("(%p): stub\n", x);
    return (FARPROC16)TRUE;
}
