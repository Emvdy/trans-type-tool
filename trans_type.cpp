#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <string>
#include <vector>

static std::wstring quote_arg(const std::wstring &arg) {
    bool needs_quotes = arg.empty() || arg.find_first_of(L" \t\"") != std::wstring::npos;
    if (!needs_quotes) {
        return arg;
    }

    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t ch : arg) {
        if (ch == L'\\') {
            ++backslashes;
        } else if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(ch);
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(ch);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

static bool exe_dir(std::wstring *dir) {
    std::vector<wchar_t> buffer(32768);
    DWORD len = GetModuleFileNameW(NULL, buffer.data(), (DWORD)buffer.size());
    if (len == 0 || len >= buffer.size()) {
        return false;
    }

    std::wstring path(buffer.data(), len);
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return false;
    }

    *dir = path.substr(0, slash + 1);
    return true;
}

static std::wstring widen_arg(const char *arg) {
    if (arg == NULL || *arg == '\0') {
        return L"";
    }

    int needed = MultiByteToWideChar(CP_ACP, 0, arg, -1, NULL, 0);
    if (needed <= 0) {
        return L"";
    }

    std::vector<wchar_t> buffer((size_t)needed);
    if (MultiByteToWideChar(CP_ACP, 0, arg, -1, buffer.data(), needed) <= 0) {
        return L"";
    }
    return std::wstring(buffer.data());
}

int main(int argc, char **argv) {
    std::wstring dir;
    if (!exe_dir(&dir)) {
        fwprintf(stderr, L"Cannot locate the executable directory.\n");
        return 3;
    }

    std::wstring native_exe = dir + L"trans_type.exe";
    DWORD attrs = GetFileAttributesW(native_exe.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        fwprintf(stderr, L"trans_type.exe was not found next to trans_type_cpp.exe.\n");
        fwprintf(stderr, L"Expected path: %ls\n", native_exe.c_str());
        return 3;
    }

    std::wstring command = quote_arg(native_exe);
    for (int i = 1; i < argc; ++i) {
        command.push_back(L' ');
        command += quote_arg(widen_arg(argv[i]));
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    ZeroMemory(&pi, sizeof(pi));
    si.cb = sizeof(si);

    if (!CreateProcessW(native_exe.c_str(), mutable_command.data(), NULL, NULL, TRUE, 0, NULL, dir.c_str(), &si, &pi)) {
        DWORD err = GetLastError();
        fwprintf(stderr, L"Failed to launch trans_type.exe. Windows error %lu.\n", (unsigned long)err);
        return 7;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code = 7;
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        exit_code = 7;
    }

    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)exit_code;
}
