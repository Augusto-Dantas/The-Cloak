// launcher.cpp - DXGI Overlay Launcher
// Injects dxgi_hook.dll into a target .exe using CreateRemoteThread
// Provides a simple console UI and named-pipe log reader

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <string>
#include <iostream>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <iomanip>

// ─────────────────────────────────────────────────────────────────────────────
// Overlay config shared with the injected DLL via shared memory
// ─────────────────────────────────────────────────────────────────────────────
struct OverlayConfig {
    char  overlayText[256]  = "DXGI Overlay Active";
    float r = 0.0f, g = 1.0f, b = 0.8f, a = 0.85f;
    int   posX = 10, posY = 10;
    bool  showFps  = true;
    bool  showTime = true;
};

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────
static std::string LastErrorStr(DWORD code = GetLastError()) {
    char buf[512]{};
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, buf, sizeof(buf), nullptr);
    return buf;
}

static DWORD FindPID(const std::string& exeName) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32 pe{ sizeof(pe) };
    DWORD pid = 0;
    if (Process32First(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, std::wstring(exeName.begin(), exeName.end()).c_str()) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32Next(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// ─────────────────────────────────────────────────────────────────────────────
// Share config with the DLL via a named shared memory section
// ─────────────────────────────────────────────────────────────────────────────
static HANDLE g_hMapFile = nullptr;

static bool WriteSharedConfig(const OverlayConfig& cfg) {
    if (!g_hMapFile) {
        g_hMapFile = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr,
            PAGE_READWRITE, 0, sizeof(OverlayConfig), "DXGIOverlayCfg");
        if (!g_hMapFile) return false;
    }
    void* pView = MapViewOfFile(g_hMapFile, FILE_MAP_WRITE, 0, 0, sizeof(OverlayConfig));
    if (!pView) return false;
    memcpy(pView, &cfg, sizeof(OverlayConfig));
    UnmapViewOfFile(pView);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Named pipe log server — reads log messages from the injected DLL
// ─────────────────────────────────────────────────────────────────────────────
static std::atomic<bool> g_running{true};

static void LogServer() {
    while (g_running) {
        HANDLE pipe = CreateNamedPipeA(
            "\\\\.\\pipe\\DXGIOverlayLog",
            PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            0, 4096, 100, nullptr);

        if (pipe == INVALID_HANDLE_VALUE) { Sleep(200); continue; }

        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            char buf[512]{};
            DWORD read;
            while (ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
                buf[read] = '\0';
                std::cout << "\033[36m[DLL] \033[0m" << buf;
            }
        }
        CloseHandle(pipe);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Core injection using CreateRemoteThread + LoadLibraryA
// ─────────────────────────────────────────────────────────────────────────────
static bool InjectDLL(DWORD pid, const std::string& dllPath) {
    HANDLE hProc = OpenProcess(
        PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION |
        PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, pid);
    if (!hProc) {
        std::cerr << "[!] OpenProcess failed: " << LastErrorStr() << "\n";
        return false;
    }

    // Allocate space for DLL path in target process
    size_t pathLen = dllPath.size() + 1;
    void* remoteStr = VirtualAllocEx(hProc, nullptr, pathLen,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!remoteStr) {
        std::cerr << "[!] VirtualAllocEx failed: " << LastErrorStr() << "\n";
        CloseHandle(hProc);
        return false;
    }

    if (!WriteProcessMemory(hProc, remoteStr, dllPath.c_str(), pathLen, nullptr)) {
        std::cerr << "[!] WriteProcessMemory failed: " << LastErrorStr() << "\n";
        VirtualFreeEx(hProc, remoteStr, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    HMODULE kernel32 = GetModuleHandleA("kernel32.dll");
    FARPROC loadLibAddr = GetProcAddress(kernel32, "LoadLibraryA");

    HANDLE hThread = CreateRemoteThread(hProc, nullptr, 0,
        (LPTHREAD_START_ROUTINE)loadLibAddr, remoteStr, 0, nullptr);
    if (!hThread) {
        std::cerr << "[!] CreateRemoteThread failed: " << LastErrorStr() << "\n";
        VirtualFreeEx(hProc, remoteStr, 0, MEM_RELEASE);
        CloseHandle(hProc);
        return false;
    }

    WaitForSingleObject(hThread, 8000);  // wait up to 8s for DllMain
    DWORD exitCode = 0;
    GetExitCodeThread(hThread, &exitCode);

    CloseHandle(hThread);
    VirtualFreeEx(hProc, remoteStr, 0, MEM_RELEASE);
    CloseHandle(hProc);

    return exitCode != 0;  // LoadLibraryA returns HMODULE (non-zero on success)
}

// ─────────────────────────────────────────────────────────────────────────────
// Launch a new process and inject
// ─────────────────────────────────────────────────────────────────────────────
static DWORD LaunchAndInject(const std::string& exePath, const std::string& dllPath) {
    STARTUPINFOA si{ sizeof(si) };
    PROCESS_INFORMATION pi{};

    // Launch suspended so we can inject before any D3D init
    std::string cmd = "\"" + exePath + "\"";
    if (!CreateProcessA(exePath.c_str(), cmd.data(), nullptr, nullptr,
        FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        std::cerr << "[!] CreateProcess failed: " << LastErrorStr() << "\n";
        return 0;
    }

    std::cout << "[+] Process created (PID " << pi.dwProcessId << "), injecting...\n";
    Sleep(200);  // give loader a moment

    if (!InjectDLL(pi.dwProcessId, dllPath)) {
        std::cerr << "[!] Injection failed, resuming anyway\n";
    } else {
        std::cout << "[+] DLL injected successfully\n";
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pi.dwProcessId;
}

// ─────────────────────────────────────────────────────────────────────────────
// Print styled banner
// ─────────────────────────────────────────────────────────────────────────────
static void PrintBanner() {
    std::cout <<
        "\033[92m"
        " ██████╗ ██╗  ██╗ ██████╗ ██╗      \n"
        " ██╔══██╗╚██╗██╔╝██╔════╝ ██║      \n"
        " ██║  ██║ ╚███╔╝ ██║  ███╗██║      \n"
        " ██║  ██║ ██╔██╗ ██║   ██║██║      \n"
        " ██████╔╝██╔╝ ██╗╚██████╔╝██║      \n"
        " ╚═════╝ ╚═╝  ╚═╝ ╚═════╝ ╚═╝      \n"
        "\033[96m"
        " OVERLAY PLANE INJECTOR  v1.0\n"
        " github.com/your-repo/dxgi-overlay\n"
        "\033[0m\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Interactive menu
// ─────────────────────────────────────────────────────────────────────────────
static void InteractiveMenu(const std::string& dllPath) {
    OverlayConfig cfg;

    while (true) {
        std::cout <<
            "\n\033[93m══════════════════════════════════\033[0m\n"
            " [1] Launch .exe and inject overlay\n"
            " [2] Attach to running process by name\n"
            " [3] Attach to running process by PID\n"
            " [4] Configure overlay text\n"
            " [5] Configure overlay color\n"
            " [6] Toggle FPS counter (" << (cfg.showFps ? "ON" : "OFF") << ")\n"
            " [7] Toggle clock      (" << (cfg.showTime ? "ON" : "OFF") << ")\n"
            " [Q] Quit\n"
            "\033[93m══════════════════════════════════\033[0m\n"
            " > ";

        std::string choice;
        std::getline(std::cin, choice);

        if (choice == "1") {
            std::cout << "Path to .exe: ";
            std::string path; std::getline(std::cin, path);
            if (!std::filesystem::exists(path)) {
                std::cerr << "[!] File not found\n"; continue;
            }
            WriteSharedConfig(cfg);
            LaunchAndInject(path, dllPath);

        } else if (choice == "2") {
            std::cout << "Process name (e.g. game.exe): ";
            std::string name; std::getline(std::cin, name);
            DWORD pid = FindPID(name);
            if (!pid) { std::cerr << "[!] Process not found\n"; continue; }
            std::cout << "[+] Found PID " << pid << "\n";
            WriteSharedConfig(cfg);
            InjectDLL(pid, dllPath)
                ? std::cout << "[+] Injected OK\n"
                : std::cerr << "[!] Injection failed\n";

        } else if (choice == "3") {
            std::cout << "PID: ";
            std::string pidStr; std::getline(std::cin, pidStr);
            DWORD pid = std::stoul(pidStr);
            WriteSharedConfig(cfg);
            InjectDLL(pid, dllPath)
                ? std::cout << "[+] Injected OK\n"
                : std::cerr << "[!] Injection failed\n";

        } else if (choice == "4") {
            std::cout << "Overlay label (max 255 chars): ";
            std::string text; std::getline(std::cin, text);
            strncpy_s(cfg.overlayText, text.c_str(), 255);
            WriteSharedConfig(cfg);
            std::cout << "[+] Config updated\n";

        } else if (choice == "5") {
            std::cout << "R G B A (0.0-1.0, space separated): ";
            std::string line; std::getline(std::cin, line);
            std::istringstream ss(line);
            ss >> cfg.r >> cfg.g >> cfg.b >> cfg.a;
            WriteSharedConfig(cfg);
            std::cout << "[+] Color updated\n";

        } else if (choice == "6") {
            cfg.showFps = !cfg.showFps;
            WriteSharedConfig(cfg);

        } else if (choice == "7") {
            cfg.showTime = !cfg.showTime;
            WriteSharedConfig(cfg);

        } else if (choice == "q" || choice == "Q") {
            break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    // Enable ANSI escape codes in Windows console
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode; GetConsoleMode(hOut, &mode);
    SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    PrintBanner();

    // Resolve DLL path (same directory as launcher by default)
    std::string dllPath;
    if (argc >= 2) {
        dllPath = argv[1];
    } else {
        char selfPath[MAX_PATH];
        GetModuleFileNameA(nullptr, selfPath, MAX_PATH);
        std::filesystem::path p(selfPath);
        dllPath = (p.parent_path() / "dxgi_hook.dll").string();
    }

    if (!std::filesystem::exists(dllPath)) {
        std::cerr << "[!] DLL not found at: " << dllPath
                  << "\n    Build dxgi_hook.dll first or pass its path as argv[1]\n";
        // Don't exit — allow demo mode
    } else {
        std::cout << "[+] DLL: " << dllPath << "\n";
    }

    // Start log server thread
    std::thread logThread(LogServer);
    logThread.detach();

    std::cout << "[+] Log pipe server listening on \\\\.\\pipe\\DXGIOverlayLog\n";

    // Quick-launch mode: launcher.exe <dll> <target.exe>
    if (argc >= 3) {
        OverlayConfig cfg;
        WriteSharedConfig(cfg);
        LaunchAndInject(argv[2], dllPath);
        std::cout << "\nPress Enter to exit...\n";
        std::cin.get();
    } else {
        InteractiveMenu(dllPath);
    }

    g_running = false;
    if (g_hMapFile) CloseHandle(g_hMapFile);
    return 0;
}
