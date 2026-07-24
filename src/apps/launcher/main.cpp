#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace {
constexpr wchar_t service_name[] = L"everything_sm";
constexpr wchar_t fallback_mutex_name[] =
    L"Local\\everything_sm_fallback_server";

std::filesystem::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) return {};
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

std::wstring read_setting(const std::filesystem::path& ini,
                          const wchar_t* key,
                          const wchar_t* fallback) {
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetPrivateProfileStringW(
        L"search", key, fallback, buffer.data(),
        static_cast<DWORD>(buffer.size()), ini.c_str());
    buffer.resize(length);
    return buffer;
}

std::wstring quote_argument(std::wstring_view argument) {
    if (argument.find_first_of(L" \t\"") == std::wstring_view::npos)
        return std::wstring(argument);

    std::wstring quoted{L'\"'};
    std::size_t slashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++slashes;
            continue;
        }
        if (ch == L'\"') {
            quoted.append(slashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
        } else {
            quoted.append(slashes, L'\\');
            quoted.push_back(ch);
        }
        slashes = 0;
    }
    quoted.append(slashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

bool create_process(const std::filesystem::path& executable,
                    const std::wstring& arguments,
                    const std::filesystem::path& working_directory,
                    DWORD creation_flags,
                    bool inherit_handles,
                    PROCESS_INFORMATION* result = nullptr) {
    std::wstring command = quote_argument(executable.wstring());
    if (!arguments.empty()) {
        command.push_back(L' ');
        command.append(arguments);
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr,
                        nullptr, inherit_handles ? TRUE : FALSE,
                        creation_flags, nullptr,
                        working_directory.empty()
                            ? nullptr : working_directory.c_str(),
                        &startup, &process)) {
        return false;
    }

    CloseHandle(process.hThread);
    if (result) {
        *result = process;
        result->hThread = nullptr;
    } else {
        CloseHandle(process.hProcess);
    }
    return true;
}

std::wstring normalized_pipe_path(std::wstring pipe_name) {
    constexpr std::wstring_view prefix = L"\\\\.\\pipe\\";
    if (!pipe_name.starts_with(prefix)) pipe_name.insert(0, prefix);
    return pipe_name;
}

bool pipe_available(const std::wstring& pipe_name) {
    const auto path = normalized_pipe_path(pipe_name);
    if (WaitNamedPipeW(path.c_str(), 0)) return true;
    return GetLastError() == ERROR_SEM_TIMEOUT;
}

bool service_is_available_or_starting() {
    SC_HANDLE manager = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!manager) return false;

    SC_HANDLE service = OpenServiceW(
        manager, service_name, SERVICE_QUERY_STATUS | SERVICE_START);
    if (!service && GetLastError() == ERROR_ACCESS_DENIED)
        service = OpenServiceW(manager, service_name, SERVICE_QUERY_STATUS);
    if (!service) {
        CloseServiceHandle(manager);
        return false;
    }

    SERVICE_STATUS_PROCESS status{};
    DWORD bytes = 0;
    bool available = QueryServiceStatusEx(
        service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes) != FALSE;

    if (available && status.dwCurrentState == SERVICE_STOPPED) {
        if (StartServiceW(service, 0, nullptr)) {
            status.dwCurrentState = SERVICE_START_PENDING;
        } else {
            available = GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        }
    }

    if (available) {
        available = status.dwCurrentState == SERVICE_RUNNING ||
                    status.dwCurrentState == SERVICE_START_PENDING ||
                    status.dwCurrentState == SERVICE_CONTINUE_PENDING;
    }

    CloseServiceHandle(service);
    CloseServiceHandle(manager);
    return available;
}

void show_launch_error(const wchar_t* detail) {
    std::wstring message = L"everything_sm 无法启动。\n\n";
    message += detail;
    message += L"\n\n请尝试重新安装，或查看安装目录中的 README.md。";
    MessageBoxW(nullptr, message.c_str(), L"everything_sm",
                MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}
} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    const auto directory = executable_directory();
    if (directory.empty()) {
        show_launch_error(L"无法确定程序安装目录。");
        return 1;
    }

    const auto gui = directory / L"esm_gui.exe";
    const auto server = directory / L"esm_server.exe";
    const auto ini = directory / L"everything_sm.ini";
    const std::wstring pipe_name =
        read_setting(ini, L"pipe_name", L"everything_sm_service");
    std::wstring scan_root =
        read_setting(ini, L"scan_root", L"C:\\");
    if (scan_root.size() == 2 && scan_root[1] == L':')
        scan_root.push_back(L'\\');

    if (!std::filesystem::exists(gui) || !std::filesystem::exists(server)) {
        show_launch_error(L"安装文件不完整。");
        return 1;
    }

    const std::wstring gui_arguments = quote_argument(pipe_name);
    if (pipe_available(pipe_name) || service_is_available_or_starting()) {
        if (!create_process(gui, gui_arguments, directory, 0, false)) {
            show_launch_error(L"无法启动图形界面。");
            return 1;
        }
        return 0;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE fallback_mutex =
        CreateMutexW(&security, FALSE, fallback_mutex_name);
    if (!fallback_mutex) {
        show_launch_error(L"无法创建后台索引器互斥量。");
        return 1;
    }
    const bool another_fallback = GetLastError() == ERROR_ALREADY_EXISTS;

    if (!another_fallback) {
        const std::wstring server_arguments =
            L"scan " + quote_argument(scan_root) + L" " +
            quote_argument(pipe_name);
        if (!create_process(server, server_arguments, directory,
                            CREATE_NO_WINDOW, true)) {
            CloseHandle(fallback_mutex);
            show_launch_error(L"无法启动后台索引器。");
            return 1;
        }
    }

    if (!create_process(gui, gui_arguments, directory, 0, false)) {
        CloseHandle(fallback_mutex);
        show_launch_error(L"无法启动图形界面。");
        return 1;
    }

    // The first fallback server inherits this handle, keeping the named
    // object alive while its initial scan is in progress and preventing a
    // second launcher from creating a competing server for the same Pipe.
    CloseHandle(fallback_mutex);
    return 0;
}
