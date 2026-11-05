// Win32 GUI for the "consecutive integers that sum to TARGET" program.
//
// Save as series_gui.cpp (rename from .txt) before building.
//
// Build (MSYS2 / MinGW):
//   g++ -std=c++17 -O2 series_gui.cpp -o series_gui.exe -mwindows -static
// Build (Visual Studio Developer Prompt):
//   cl /EHsc /std:c++17 /O2 series_gui.cpp user32.lib gdi32.lib

#define NOMINMAX  // stop windows.h from defining min/max macros that break std::max
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

struct Match {
    long long start;
    long long end;
};

// ---- control IDs and custom message ----------------------------------------
enum {
    ID_LABEL = 100,
    ID_INPUT,
    ID_BUTTON,
    ID_STOP,
    ID_THREADS_LABEL,
    ID_THREADS,
    ID_HINT,
    ID_CLEAR,
    ID_OUTPUT
};
const UINT WM_COMPUTE_DONE = WM_APP + 1;

HWND g_input = nullptr;
HWND g_button = nullptr;
HWND g_stop = nullptr;
HWND g_threads = nullptr;
HWND g_clear = nullptr;
HWND g_output = nullptr;

// logical processors across ALL processor groups, detected at startup
unsigned g_cores = 1;

// set to true by the Stop button; worker threads check it regularly
std::atomic<bool> g_cancel{ false };

// everything shown in the results box; new runs are appended to it
std::string g_log;
int g_runCount = 0;

// ---- processor detection ------------------------------------------------
// GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) counts every logical
// processor on the machine, even on systems with more than 64 of them.
unsigned detectLogicalProcessors() {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (n == 0) {
        // API failed for some reason; fall back to the standard library
        n = std::thread::hardware_concurrency();
    }
    return std::max(1u, (unsigned)n);
}

// ---- the actual computation (multithreaded, same logic as before) -----------
std::string compute(long long target, unsigned numThreads, bool& cancelled) {
    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::vector<Match>> results(numThreads);
    std::vector<std::thread> threads;
    std::atomic<bool> aborted{ false };

    for (unsigned t = 0; t < numThreads; ++t) {
        threads.emplace_back([t, numThreads, target, &results, &aborted]() {
            unsigned counter = 0;
            for (long long start = t; start <= target; start += numThreads) {
                // check the stop flag every 1024 starting numbers
                if ((++counter & 1023u) == 0 && g_cancel.load()) {
                    aborted = true;
                    return;
                }

                long long sum = 0;
                long long n = start;
                while (sum < target) {
                    sum += n;
                    ++n;
                }
                if (sum == target) {
                    results[t].push_back({ start, n - 1 });
                }
            }
            });
    }
    for (auto& th : threads) th.join();
    cancelled = aborted.load();

    std::vector<Match> all;
    for (const auto& r : results) all.insert(all.end(), r.begin(), r.end());
    std::sort(all.begin(), all.end(),
        [](const Match& a, const Match& b) { return a.start < b.start; });

    std::string out;
    out += "Target: " + std::to_string(target) + "\r\n";
    out += "Threads used: " + std::to_string(numThreads) + "\r\n\r\n";
    if (cancelled) {
        out += "*** STOPPED before finishing - results below are partial. ***\r\n\r\n";
    }

    if (all.empty()) {
        out += cancelled ? "No runs found before stopping.\r\n"
            : "No runs of consecutive integers sum to this number.\r\n";
    }
    else {
        out += "Found " + std::to_string(all.size()) + " run(s):\r\n\r\n";
        for (const auto& m : all) {
            out += "Start " + std::to_string(m.start) + ":  " +
                std::to_string(m.start) + " + ... + " + std::to_string(m.end) +
                " = " + std::to_string(target) + "\r\n";
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    auto micros =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    out += "\r\nTotal time:\r\n";
    out += "  " + std::to_string(micros) + " microseconds\r\n";
    out += "  " + std::to_string(micros / 1000.0) + " milliseconds\r\n";
    out += "  " + std::to_string(micros / 1000000.0) + " seconds\r\n";
    return out;
}

// ---- helpers ------------------------------------------------------------
bool parsePositive(const std::string& s, long long& value) {
    if (s.empty() || s.size() > 18) return false;  // keep well inside long long
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    value = std::strtoll(s.c_str(), nullptr, 10);
    return value >= 1;
}

void setFont(HWND h) {
    SendMessageA(h, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

void showLog(const std::string& tail) {
    std::string text = g_log + tail;
    SetWindowTextA(g_output, text.c_str());
    SendMessageA(g_output, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);  // caret to the end
    SendMessageA(g_output, EM_SCROLLCARET, 0, 0);               // scroll it into view
}

void startCompute(HWND hwnd) {
    char buf[64] = {};
    GetWindowTextA(g_input, buf, sizeof(buf));

    long long target = 0;
    if (!parsePositive(buf, target)) {
        MessageBoxA(hwnd, "Please enter a whole number of 1 or greater.",
            "Invalid input", MB_OK | MB_ICONWARNING);
        return;
    }

    char tbuf[16] = {};
    GetWindowTextA(g_threads, tbuf, sizeof(tbuf));
    long long threads = 0;
    if (!parsePositive(tbuf, threads) || threads > (long long)g_cores) {
        std::string m = "Please enter a thread count between 1 and " +
            std::to_string(g_cores) + ".";
        MessageBoxA(hwnd, m.c_str(), "Invalid input", MB_OK | MB_ICONWARNING);
        return;
    }

    g_cancel = false;
    EnableWindow(g_button, FALSE);
    EnableWindow(g_stop, TRUE);
    EnableWindow(g_clear, FALSE);
    showLog("Computing...\r\n");

    // Run on a worker thread so the window never freezes on large targets.
    std::thread([hwnd, target, threads]() {
        bool cancelled = false;
        auto* result = new std::string(compute(target, (unsigned)threads, cancelled));
        PostMessageA(hwnd, WM_COMPUTE_DONE, 0, (LPARAM)result);
        }).detach();
}

// ---- window procedure ---------------------------------------------------
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_cores = detectLogicalProcessors();
        unsigned cores = g_cores;

        HWND label = CreateWindowExA(0, "STATIC", "Sum to compute:",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
            (HMENU)ID_LABEL, nullptr, nullptr);
        g_input = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "10000",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            ES_AUTOHSCROLL | ES_NUMBER,
            0, 0, 0, 0, hwnd, (HMENU)ID_INPUT, nullptr,
            nullptr);
        g_button = CreateWindowExA(0, "BUTTON", "Compute",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_DEFPUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)ID_BUTTON, nullptr,
            nullptr);
        g_stop = CreateWindowExA(0, "BUTTON", "Stop",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED |
            BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)ID_STOP, nullptr,
            nullptr);
        g_clear = CreateWindowExA(0, "BUTTON", "Clear results",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            BS_PUSHBUTTON,
            0, 0, 0, 0, hwnd, (HMENU)ID_CLEAR, nullptr,
            nullptr);
        HWND tLabel = CreateWindowExA(0, "STATIC", "Threads to use:",
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
            (HMENU)ID_THREADS_LABEL, nullptr, nullptr);
        g_threads = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT",
            std::to_string(cores).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP |
            ES_AUTOHSCROLL | ES_NUMBER,
            0, 0, 0, 0, hwnd, (HMENU)ID_THREADS, nullptr,
            nullptr);
        std::string hintText =
            "(" + std::to_string(cores) + " logical processors detected)";
        HWND hint = CreateWindowExA(0, "STATIC", hintText.c_str(),
            WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, hwnd,
            (HMENU)ID_HINT, nullptr, nullptr);
        g_output = CreateWindowExA(
            WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY |
            ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, (HMENU)ID_OUTPUT, nullptr, nullptr);

        SendMessageA(g_output, EM_LIMITTEXT, 0, 0);  // lift the 32K default
        // threads box can't hold more digits than the processor count has
        SendMessageA(g_threads, EM_LIMITTEXT,
            (WPARAM)std::to_string(g_cores).size(), 0);
        setFont(label);
        setFont(g_input);
        setFont(g_button);
        setFont(g_stop);
        setFont(g_clear);
        setFont(tLabel);
        setFont(g_threads);
        setFont(hint);
        setFont(g_output);
        SetFocus(g_input);
        return 0;
    }

    case WM_SIZE: {
        int w = LOWORD(lParam);
        int h = HIWORD(lParam);
        const int m = 12;  // margin
        MoveWindow(GetDlgItem(hwnd, ID_LABEL), m, m + 4, 110, 20, TRUE);
        MoveWindow(g_input, m + 115, m, 160, 26, TRUE);
        MoveWindow(g_button, m + 290, m, 90, 26, TRUE);
        MoveWindow(g_stop, m + 390, m, 90, 26, TRUE);
        MoveWindow(GetDlgItem(hwnd, ID_THREADS_LABEL), m, m + 42, 110, 20, TRUE);
        MoveWindow(g_threads, m + 115, m + 38, 60, 26, TRUE);
        MoveWindow(g_clear, m + 390, m + 38, 90, 26, TRUE);
        MoveWindow(GetDlgItem(hwnd, ID_HINT), m + 190, m + 42, 195, 20, TRUE);
        MoveWindow(g_output, m, m + 76, w - 2 * m, h - 2 * m - 76, TRUE);
        return 0;
    }

    case WM_COMMAND:
        // Clamp the threads box to the number of logical processors,
        // whether the user types or pastes.
        if (LOWORD(wParam) == ID_THREADS && HIWORD(wParam) == EN_CHANGE) {
            if (!g_threads) break;  // fires during creation, before g_threads is set
            char b[16] = {};
            GetWindowTextA(g_threads, b, sizeof(b));
            if (b[0] != '\0' && std::strtoll(b, nullptr, 10) > (long long)g_cores) {
                std::string s = std::to_string(g_cores);
                SetWindowTextA(g_threads, s.c_str());
                SendMessageA(g_threads, EM_SETSEL, (WPARAM)-1, (LPARAM)-1);  // caret to end
            }
            return 0;
        }
        if (LOWORD(wParam) == ID_BUTTON && HIWORD(wParam) == BN_CLICKED) {
            startCompute(hwnd);
            return 0;
        }
        if (LOWORD(wParam) == ID_STOP && HIWORD(wParam) == BN_CLICKED) {
            g_cancel = true;
            EnableWindow(g_stop, FALSE);
            showLog("Stopping...\r\n");
            return 0;
        }
        if (LOWORD(wParam) == ID_CLEAR && HIWORD(wParam) == BN_CLICKED) {
            g_log.clear();
            g_runCount = 0;
            showLog("");
            return 0;
        }
        break;

    case WM_COMPUTE_DONE: {
        std::string* result = (std::string*)lParam;
        ++g_runCount;
        g_log += "===== Run " + std::to_string(g_runCount) + " =====\r\n" +
            *result + "\r\n";
        showLog("");
        delete result;
        EnableWindow(g_button, TRUE);
        EnableWindow(g_stop, FALSE);
        EnableWindow(g_clear, TRUE);
        SetFocus(g_input);
        return 0;
    }

    case WM_DESTROY:
        g_cancel = true;  // let any running calculation wind down
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

// ---- entry point --------------------------------------------------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nCmdShow) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "SeriesSumWindow";
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowExA(0, wc.lpszClassName, "Consecutive Series Sum",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        560, 440, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageA(hwnd, &msg)) {  // gives Tab/Enter keyboard handling
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    return (int)msg.wParam;
}