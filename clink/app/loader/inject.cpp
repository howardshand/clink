// Copyright (c) 2012 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "paths.h"
#include "inject_args.h"

#include <core/base.h>
#include <core/log.h>
#include <core/path.h>
#include <core/str.h>
#include <getopt.h>
#include <process/process.h>
#include <process/vm.h>

#define CLINK_DLL_NAME "clink_" AS_STR(PLATFORM) ".dll"

//------------------------------------------------------------------------------
int     initialise_clink(const inject_args*);
void    puts_help(const char**, int);
void    cpy_path_as_abs(str_base&, const char*);

//------------------------------------------------------------------------------
static int check_dll_version(const char* clink_dll)
{
    char buffer[1024];
    if (GetFileVersionInfo(clink_dll, 0, sizeof(buffer), buffer) != TRUE)
        return 0;

    VS_FIXEDFILEINFO* file_info;
    if (VerQueryValue(buffer, "\\", (void**)&file_info, nullptr) != TRUE)
        return 0;

    LOG("DLL version: %08x %08x",
        file_info->dwFileVersionMS,
        file_info->dwFileVersionLS
    );

    int error = 0;
    error = (HIWORD(file_info->dwFileVersionMS) != CLINK_VER_MAJOR);
    error = (LOWORD(file_info->dwFileVersionMS) != CLINK_VER_MINOR);
    error = (HIWORD(file_info->dwFileVersionLS) != CLINK_VER_POINT);

    return !error;
}

//------------------------------------------------------------------------------
static int do_inject(DWORD target_pid)
{
#ifdef __MINGW32__
    HMODULE kernel32 = LoadLibraryA("kernel32.dll");
    typedef BOOL (WINAPI *_IsWow64Process)(HANDLE, BOOL*);
    _IsWow64Process IsWow64Process = (_IsWow64Process)GetProcAddress(
        kernel32,
        "IsWow64Process"
    );
#endif // __MINGW32__

    // Get path to clink's DLL that we'll inject.
    str<256> dll_path;
    process().get_file_name(dll_path);
    path::get_directory(dll_path);
    path::append(dll_path, CLINK_DLL_NAME);

    // Reset log file, start logging!
    SYSTEM_INFO sys_info;
    GetSystemInfo(&sys_info);

    OSVERSIONINFOEX osvi;
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    GetVersionEx((OSVERSIONINFO*)&osvi);

    LOG("System: ver=%d.%d %d.%d arch=%d cpus=%d cpu_type=%d page_size=%d",
        osvi.dwMajorVersion,
        osvi.dwMinorVersion,
        osvi.wServicePackMajor,
        osvi.wServicePackMinor,
        sys_info.wProcessorArchitecture,
        sys_info.dwNumberOfProcessors,
        sys_info.dwProcessorType,
        sys_info.dwPageSize
    );
    LOG("Version: %d.%d.%d",
        CLINK_VER_MAJOR,
        CLINK_VER_MINOR,
        CLINK_VER_POINT
    );
    LOG("DLL: %s", dll_path);

    LOG("Parent pid: %d", target_pid);

    // Check Dll's version.
    if (!check_dll_version(dll_path.c_str()))
    {
        ERR("DLL failed version check.");
        return 0;
    }

    // Inject Clink DLL.
    process cmd_process(target_pid);
    return !!cmd_process.inject_module(dll_path.c_str());
}

//------------------------------------------------------------------------------
static int is_clink_present(DWORD target_pid)
{
    int ret;
    BOOL ok;
    MODULEENTRY32 module_entry;

    HANDLE th32 = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, target_pid);
    if (th32 == INVALID_HANDLE_VALUE)
    {
        LOG("Failed to snapshot module state.");
        return 0;
    }

    ret = 0;
    ok = Module32First(th32, &module_entry);
    while (ok != FALSE)
    {
        if (_stricmp(module_entry.szModule, CLINK_DLL_NAME) == 0)
        {
            LOG("Clink already installed in process.");
            ret = 1;
            break;
        }

        ok = Module32Next(th32, &module_entry);
    }

    CloseHandle(th32);
    return ret;
}

//------------------------------------------------------------------------------
void get_profile_path(const char* in, str_base& out)
{
    if (in[0] == '~' && (in[1] == '\\' || in[1] == '/'))
    {
        char dir[MAX_PATH];
        if (SHGetFolderPath(0, CSIDL_LOCAL_APPDATA, nullptr, 0, dir) == S_OK)
        {
            out << dir << "." << (in + 1);
            return;
        }
    }

    cpy_path_as_abs(out, in);
}

//------------------------------------------------------------------------------
int inject(int argc, char** argv)
{
    struct option options[] = {
        { "profile",     required_argument,  nullptr, 'p' },
        { "quiet",       no_argument,        nullptr, 'q' },
        { "pid",         required_argument,  nullptr, 'd' },
        { "nolog",       no_argument,        nullptr, 'l' },
        { "autorun",     no_argument,        nullptr, '_' },
        { "help",        no_argument,        nullptr, 'h' },
        { nullptr, 0, nullptr, 0 }
    };

    const char* help[] = {
        "-p, --profile <path>", "Specifies and alternative path for profile data.",
        "-q, --quiet",          "Suppress copyright output.",
        "-d, --pid <pid>",      "Inject into the process specified by <pid>.",
        "-l, --nolog",          "Disable file logging.",
        "-h, --help",           "Shows this help text.",
    };

    extern const char* g_clink_header;

    // Parse arguments
    bool is_autorun = false;
    DWORD target_pid = 0;
    inject_args inject_args = { 0 };
    int i;
    int ret = 0;
    while ((i = getopt_long(argc, argv, "nalqhp:d:", options, nullptr)) != -1)
    {
        switch (i)
        {
        case 'p':
            {
                char* data = inject_args.profile_path;
                int size = sizeof_array(inject_args.profile_path);
                str_base buffer(data, size);
                get_profile_path(optarg, buffer);
            }
            break;

        case 'q': inject_args.quiet = 1;         break;
        case 'd': target_pid = atoi(optarg);     break;
        case '_': is_autorun = true;             break;

        case 'l':
            inject_args.no_log = 1;
            break;

        case '?':
            goto end;

        case 'h':
        default:
            puts(g_clink_header);
            puts_help(help, sizeof_array(help));
            goto end;
        }
    }

    // Restart the log file on every inject.
    str<256> log_path;
    get_log_dir(log_path);
    log_path << "/clink.log";
    unlink(log_path.c_str());

    // Unless a target pid was specified on the command line, use our parent
    // process pid.
    if (target_pid == 0)
    {
        target_pid = process().get_parent_pid();
        if (target_pid == 0)
        {
            LOG("Failed to find parent pid.");
            goto end;
        }
    }

    // Check to see if clink is already installed.
    if (is_clink_present(target_pid))
        goto end;

    // Inject Clink's DLL and remotely call Clink's initialisation function.
    if (!do_inject(target_pid))
        goto end;

    // On Windows a DLL will have the same address in every process' address
    // space, hence we're able to use 'initialise_clink' directly here.
    vm_access target_vm(target_pid);
    void* remote_inject_args = target_vm.alloc(sizeof(inject_args));
    int ret = process(target_pid).remote_call(initialise_clink, remote_inject_args);
    target_vm.free(remote_inject_args);

end:
    return is_autorun ? 1 : ret;
}