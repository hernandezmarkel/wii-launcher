#include "wl/process.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <thread>
extern char** environ;
#endif

#include <set>
#include <vector>

namespace wl {

#ifdef _WIN32
namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::wstring quote(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++slashes; continue; }
        if (c == L'"') out.append(slashes * 2 + 1, L'\\');
        else out.append(slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}
}

Spawned spawn(const LaunchSpec& spec) {
    Spawned r;
    fs::path exe = spec.executable;
    exe.make_preferred();
    std::wstring cmd = quote(exe.wstring());
    for (const auto& a : spec.args) cmd += L" " + quote(wide(a));
    // Entorno: el heredado, sustituyendo las claves de spec.env (comparación sin mayúsculas, como Windows).
    std::vector<std::wstring> vars;
    std::set<std::wstring> overridden;
    for (const auto& [k, v] : spec.env) {
        std::wstring key = wide(k);
        for (auto& c : key) c = towupper(c);
        overridden.insert(key);
        vars.push_back(wide(k) + L"=" + wide(v));
    }
    if (wchar_t* block = GetEnvironmentStringsW()) {
        for (wchar_t* p = block; *p; p += wcslen(p) + 1) {
            std::wstring entry = p;
            size_t eq = entry.find(L'=', 1);
            std::wstring key = entry.substr(0, eq);
            for (auto& c : key) c = towupper(c);
            if (!overridden.count(key)) vars.push_back(entry);
        }
        FreeEnvironmentStringsW(block);
    }
    std::wstring env;
    for (const auto& v : vars) env += v + L'\0';
    env += L'\0';
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring dir = spec.working_dir.wstring();
    if (!CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, env.data(),
                        dir.empty() ? nullptr : dir.c_str(), &si, &pi)) {
        r.error = "No se pudo iniciar el juego (error " + std::to_string(GetLastError()) + ")";
        return r;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    r.ok = true;
    r.pid = pi.dwProcessId;
    return r;
}

int wait_exit(long long pid, int timeout_ms) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return -1;
    DWORD code = (DWORD)-1;
    if (WaitForSingleObject(h, timeout_ms < 0 ? INFINITE : (DWORD)timeout_ms) == WAIT_OBJECT_0) GetExitCodeProcess(h, &code);
    CloseHandle(h);
    return (int)code;
}
#else
Spawned spawn(const LaunchSpec& spec) {
    Spawned r;
    std::vector<std::string> env_storage;
    for (char** e = environ; *e; ++e) {
        std::string entry = *e;
        std::string key = entry.substr(0, entry.find('='));
        if (!spec.env.count(key)) env_storage.push_back(entry);
    }
    for (const auto& [k, v] : spec.env) env_storage.push_back(k + "=" + v);
    std::vector<char*> envp;
    for (auto& s : env_storage) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::string exe = spec.executable.string();
    std::vector<std::string> arg_storage{exe};
    arg_storage.insert(arg_storage.end(), spec.args.begin(), spec.args.end());
    std::vector<char*> argv;
    for (auto& s : arg_storage) argv.push_back(s.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 29))
    if (!spec.working_dir.empty()) posix_spawn_file_actions_addchdir_np(&actions, spec.working_dir.c_str());
#endif
    pid_t pid = 0;
    int rc = posix_spawn(&pid, exe.c_str(), &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    if (rc != 0) {
        r.error = std::string("No se pudo iniciar el juego: ") + std::strerror(rc);
        return r;
    }
    r.ok = true;
    r.pid = pid;
    return r;
}

int wait_exit(long long pid, int timeout_ms) {
    auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    for (;;) {
        int status = 0;
        pid_t done = waitpid((pid_t)pid, &status, WNOHANG);
        if (done == (pid_t)pid) return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        if (done < 0) return -1;
        if (timeout_ms >= 0 && std::chrono::steady_clock::now() >= until) return -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
#endif

}
