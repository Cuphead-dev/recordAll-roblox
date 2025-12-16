#include <windows.h>
#include <commctrl.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <chrono>
#include <thread>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <mutex>
#include <deque>
#include <atomic>
#include <cmath>
#include <unordered_set>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")

using json = nlohmann::json;
namespace fs = std::filesystem;

// GUI Control IDs
#define IDC_BTN_RECORD          1001
#define IDC_BTN_PLAY            1002
#define IDC_BTN_STOP            1003
#define IDC_BTN_TOGGLE_MODE     1004
#define IDC_LIST_RECORDINGS     1005
#define IDC_BTN_LOAD            1006
#define IDC_EDIT_SENS           1007
#define IDC_EDIT_RAMP           1008
#define IDC_BTN_SAVE_SETTINGS   1009
#define IDC_STATUS_TEXT         1010
#define IDC_TIMER_UPDATE        1011

// NEW IDs for loop controls
#define IDC_BTN_LOOP            1012
#define IDC_EDIT_LOOP_COUNT     1013

// Tunable parameters
static float TUNING_SENSITIVITY         = 1.00f;
static float TUNING_PLAYBACK_VELOCITY   = 1.00f;
static float TUNING_SMOOTH_ALPHA        = 0.70f;
static int   TUNING_STOP_THRESHOLD      = 1;
static int   TUNING_STOP_FRAMES         = 2;
static int   RAW_TICK_MS                = 4;
static int   STOP_RAMP_MS               = 40;
static double RAMP_DECAY                = 0.45;
static bool   ENABLE_PLAYBACK_RAMP      = true;
static double RAW_SENS_X = 1.0;
static double RAW_SENS_Y = 1.0;

enum class ActionType {
    MOUSE_MOVE, MOUSE_DELTA, MOUSE_PRESS, MOUSE_RELEASE, MOUSE_SCROLL,
    KEY_PRESS, KEY_RELEASE
};

struct Action {
    ActionType type;
    int x = 0, y = 0;
    double deltaX = 0.0, deltaY = 0.0;
    std::string button;
    std::string key;
    DWORD vkCode = 0;
    int scrollDx = 0, scrollDy = 0;
    double time = 0.0;
    bool isRawDelta = false;
};

struct RawDelta {
    int dx, dy;
    double time;
};

class KeyboardMouseRecorder {
private:
    bool recording = false;
    bool loopPlayback = false;
    bool shouldExit = false;
    bool playbackRunning = false;
    bool recordOnMoveAlways = false;
    std::vector<Action> actions;
    std::chrono::steady_clock::time_point startTime;
    std::chrono::steady_clock::time_point recordStartTime;
    HHOOK mouseHook = nullptr;
    HHOOK keyboardHook = nullptr;
    HWND hiddenWindow = nullptr;
    HWND mainWindow = nullptr;

    POINT lastMousePos = {0, 0};
    bool isRightButtonPressed = false;
    static KeyboardMouseRecorder* instance;

    std::mutex actionsMutex;
    std::mutex rawMutex;
    std::deque<RawDelta> rawQueue;
    std::atomic<bool> rawProcessorRunning{false};
    std::thread rawProcessorThread;

    // NEW: loop config
    int loopTimes = 1;        // number of times to loop; 0 = infinite when loopEnabled true
    bool loopEnabled = false; // whether looping is requested

    double getCurrentTime() {
        if (recording) {
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - startTime);
            return duration.count() / 1000.0;
        }
        return 0.0;
    }

    std::string getButtonName(UINT mouseMsg) {
        if (mouseMsg == WM_LBUTTONDOWN || mouseMsg == WM_LBUTTONUP) return "left";
        if (mouseMsg == WM_RBUTTONDOWN || mouseMsg == WM_RBUTTONUP) return "right";
        if (mouseMsg == WM_MBUTTONDOWN || mouseMsg == WM_MBUTTONUP) return "middle";
        return "unknown";
    }

    std::string getKeyName(DWORD vkCode) {
        char keyName[256] = {0};
        UINT scanCode = MapVirtualKeyA(vkCode, MAPVK_VK_TO_VSC);
        LONG lParam = (scanCode << 16);
        if (GetKeyNameTextA(lParam, keyName, sizeof(keyName)) != 0) {
            return std::string(keyName);
        }
        if (vkCode >= 0x20 && vkCode <= 0x7E) {
            return std::string(1, static_cast<char>(vkCode));
        }
        return "key_" + std::to_string(vkCode);
    }

    static LRESULT CALLBACK RawInputWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
        if (msg == WM_INPUT && instance && instance->recording) {
            UINT dwSize = 0;
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER)) == (UINT)-1) {
                return 0;
            }
            std::vector<BYTE> lpb(dwSize);
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb.data(), &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) {
                return 0;
            }
            RAWINPUT* raw = reinterpret_cast<RAWINPUT*>(lpb.data());
            if (raw->header.dwType == RIM_TYPEMOUSE) {
                int deltaX = raw->data.mouse.lLastX;
                int deltaY = raw->data.mouse.lLastY;
                if ((deltaX != 0 || deltaY != 0) && (instance->isRightButtonPressed || instance->recordOnMoveAlways)) {
                    RawDelta rd{ deltaX, deltaY, instance->getCurrentTime() };
                    std::lock_guard<std::mutex> lk(instance->rawMutex);
                    instance->rawQueue.push_back(rd);
                }
            }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    void createRawInputWindow() {
        WNDCLASSEXW wc = {0};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = RawInputWindowProc;
        wc.hInstance = GetModuleHandleW(NULL);
        wc.lpszClassName = L"RawInputClass";
        RegisterClassExW(&wc);
        
        hiddenWindow = CreateWindowExW(0, L"RawInputClass", L"RawInputWindow",
            0, 0, 0, 0, 0, HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL);
        
        RAWINPUTDEVICE rid[1];
        rid[0].usUsagePage = 0x01;
        rid[0].usUsage = 0x02;
        rid[0].dwFlags = RIDEV_INPUTSINK;
        rid[0].hwndTarget = hiddenWindow;
        RegisterRawInputDevices(rid, 1, sizeof(RAWINPUTDEVICE));
    }

    void startRawProcessor() {
        rawProcessorRunning = true;
        rawProcessorThread = std::thread([this]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            double smoothedX = 0.0, smoothedY = 0.0;
            int consecutiveSmall = 0;
            bool stopped = false;
            std::deque<std::pair<int,int>> recent;
            const size_t RECENT_MAX = 6;

            while (rawProcessorRunning) {
                RawDelta rd;
                bool have = false;
                {
                    std::lock_guard<std::mutex> lk(rawMutex);
                    if (!rawQueue.empty()) {
                        rd = rawQueue.front();
                        rawQueue.pop_front();
                        have = true;
                    }
                }
                if (!have) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(RAW_TICK_MS));
                    consecutiveSmall++;
                    continue;
                }
                int dx = rd.dx, dy = rd.dy;
                recent.push_back({dx,dy});
                if (recent.size() > RECENT_MAX) recent.pop_front();

                int absVal = std::max(std::abs(dx), std::abs(dy));
                if (absVal <= TUNING_STOP_THRESHOLD) {
                    consecutiveSmall++;
                } else {
                    consecutiveSmall = 0;
                }

                if (consecutiveSmall >= TUNING_STOP_FRAMES) {
                    if (!stopped) {
                        double avgX = 0.0, avgY = 0.0;
                        if (!recent.empty()) {
                            for (auto &p : recent) { avgX += p.first; avgY += p.second; }
                            avgX /= static_cast<double>(recent.size());
                            avgY /= static_cast<double>(recent.size());
                        }
                        if (ENABLE_PLAYBACK_RAMP) {
                            int rampSteps = std::max(1, STOP_RAMP_MS / RAW_TICK_MS);
                            double startX = smoothedX == 0.0 ? avgX : smoothedX;
                            double startY = smoothedY == 0.0 ? avgY : smoothedY;
                            double t0 = rd.time;
                            for (int k = 1; k <= rampSteps; ++k) {
                                double fracPrev = std::pow(RAMP_DECAY, (double)(k-1));
                                double fracCurr = std::pow(RAMP_DECAY, (double)k);
                                double stepX = startX * (fracPrev - fracCurr);
                                double stepY = startY * (fracPrev - fracCurr);
                                Action ra;
                                ra.type = ActionType::MOUSE_DELTA;
                                ra.deltaX = stepX * RAW_SENS_X;
                                ra.deltaY = stepY * RAW_SENS_Y;
                                ra.time = t0 + (k * RAW_TICK_MS) / 1000.0;
                                ra.isRawDelta = true;
                                {
                                    std::lock_guard<std::mutex> lk(actionsMutex);
                                    actions.push_back(ra);
                                }
                            }
                        }
                        smoothedX = 0.0;
                        smoothedY = 0.0;
                        stopped = true;
                    }
                } else {
                    stopped = false;
                    smoothedX = TUNING_SMOOTH_ALPHA * static_cast<double>(dx) + (1.0 - TUNING_SMOOTH_ALPHA) * smoothedX;
                    smoothedY = TUNING_SMOOTH_ALPHA * static_cast<double>(dy) + (1.0 - TUNING_SMOOTH_ALPHA) * smoothedY;
                    Action a;
                    a.type = ActionType::MOUSE_DELTA;
                    a.deltaX = smoothedX * RAW_SENS_X;
                    a.deltaY = smoothedY * RAW_SENS_Y;
                    a.time = rd.time;
                    a.isRawDelta = true;
                    {
                        std::lock_guard<std::mutex> lk(actionsMutex);
                        actions.push_back(a);
                    }
                }
            }
        });
    }

    void stopRawProcessor() {
        rawProcessorRunning = false;
        if (rawProcessorThread.joinable()) rawProcessorThread.join();
    }

    static LRESULT CALLBACK MouseHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0 && instance && instance->recording) {
            MSLLHOOKSTRUCT* mouseInfo = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
            Action action;
            action.time = instance->getCurrentTime();
            action.isRawDelta = false;
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            action.x = cursorPos.x;
            action.y = cursorPos.y;

            switch (wParam) {
                case WM_MOUSEMOVE:
                    if (!instance->isRightButtonPressed) {
                        action.type = ActionType::MOUSE_MOVE;
                        action.deltaX = static_cast<double>(cursorPos.x - instance->lastMousePos.x);
                        action.deltaY = static_cast<double>(cursorPos.y - instance->lastMousePos.y);
                        std::lock_guard<std::mutex> lk(instance->actionsMutex);
                        instance->actions.push_back(action);
                    }
                    instance->lastMousePos = cursorPos;
                    break;
                case WM_LBUTTONDOWN:
                case WM_MBUTTONDOWN:
                    action.type = ActionType::MOUSE_PRESS;
                    action.button = instance->getButtonName(static_cast<UINT>(wParam));
                    { std::lock_guard<std::mutex> lk(instance->actionsMutex); instance->actions.push_back(action); }
                    break;
                case WM_RBUTTONDOWN:
                    action.type = ActionType::MOUSE_PRESS;
                    action.button = "right";
                    { std::lock_guard<std::mutex> lk(instance->actionsMutex); instance->actions.push_back(action); }
                    instance->isRightButtonPressed = true;
                    instance->lastMousePos = cursorPos;
                    break;
                case WM_LBUTTONUP:
                case WM_MBUTTONUP:
                    action.type = ActionType::MOUSE_RELEASE;
                    action.button = instance->getButtonName(static_cast<UINT>(wParam));
                    { std::lock_guard<std::mutex> lk(instance->actionsMutex); instance->actions.push_back(action); }
                    break;
                case WM_RBUTTONUP:
                    instance->isRightButtonPressed = false;
                    { std::lock_guard<std::mutex> lk(instance->rawMutex); instance->rawQueue.clear(); }
                    action.type = ActionType::MOUSE_RELEASE;
                    action.button = "right";
                    { std::lock_guard<std::mutex> lk(instance->actionsMutex); instance->actions.push_back(action); }
                    break;
                case WM_MOUSEWHEEL:
                    action.type = ActionType::MOUSE_SCROLL;
                    action.scrollDx = 0;
                    action.scrollDy = GET_WHEEL_DELTA_WPARAM(mouseInfo->mouseData) / WHEEL_DELTA;
                    { std::lock_guard<std::mutex> lk(instance->actionsMutex); instance->actions.push_back(action); }
                    break;
            }
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    static LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0 && instance) {
            KBDLLHOOKSTRUCT* keyInfo = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            
            // Hotkeys
            if (wParam == WM_KEYDOWN) {
                if (keyInfo->vkCode == VK_F1) { instance->toggleRecording(); return 1; }
                if (keyInfo->vkCode == VK_F2) { instance->playLast(); return 1; }
                if (keyInfo->vkCode == VK_F3) { instance->stopPlayback(); return 1; }
                if (keyInfo->vkCode == VK_F4) { instance->toggleMode(); return 1; }
                if (keyInfo->vkCode == VK_ESCAPE) { instance->emergencyStop(); return 1; }
            }
            
            if (instance->recording) {
                bool isRepeat = (keyInfo->flags & LLKHF_INJECTED) ||
                               ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) && (keyInfo->flags & 0x80));
                if (!isRepeat) {
                    Action action;
                    action.time = instance->getCurrentTime();
                    action.key = instance->getKeyName(keyInfo->vkCode);
                    action.vkCode = keyInfo->vkCode;
                    action.isRawDelta = false;
                    if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                        action.type = ActionType::KEY_PRESS;
                        std::lock_guard<std::mutex> lk(instance->actionsMutex);
                        instance->actions.push_back(action);
                    } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                        action.type = ActionType::KEY_RELEASE;
                        std::lock_guard<std::mutex> lk(instance->actionsMutex);
                        instance->actions.push_back(action);
                    }
                }
            }
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

public:
    void setMainWindow(HWND hwnd) { mainWindow = hwnd; }

    // public setters for loop UI
    void setLoopEnabled(bool v) { loopEnabled = v; }
    void setLoopTimes(int n) { loopTimes = n; }

    void updateGUI() {
        if (!mainWindow) return;
        
        wchar_t status[512];
        if (recording) {
            auto elapsed = std::chrono::steady_clock::now() - recordStartTime;
            auto secs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() / 1000.0;
            size_t count = 0;
            { std::lock_guard<std::mutex> lk(actionsMutex); count = actions.size(); }
            swprintf_s(status, L"🔴 RECORDING (%.1fs) | Actions: %zu | Mode: %s", 
                secs, count, recordOnMoveAlways ? L"Roblox-compatible" : L"Original");
        } else if (playbackRunning) {
            if (loopEnabled) {
                if (loopTimes <= 0) {
                    swprintf_s(status, L"▶️ PLAYING | Loop: ∞ | Mode: %s", recordOnMoveAlways ? L"Roblox-compatible" : L"Original");
                } else {
                    swprintf_s(status, L"▶️ PLAYING | Loop x%d | Mode: %s", loopTimes, recordOnMoveAlways ? L"Roblox-compatible" : L"Original");
                }
            } else {
                swprintf_s(status, L"▶️ PLAYING | Mode: %s", 
                    recordOnMoveAlways ? L"Roblox-compatible" : L"Original");
            }
        } else {
            size_t count = 0;
            { std::lock_guard<std::mutex> lk(actionsMutex); count = actions.size(); }
            swprintf_s(status, L"⏸️ IDLE | Actions: %zu | Mode: %s", 
                count, recordOnMoveAlways ? L"Roblox-compatible" : L"Original");
        }
        SetDlgItemTextW(mainWindow, IDC_STATUS_TEXT, status);
    }

    void toggleRecording() {
        if (!recording) startRecording();
        else stopRecording();
        updateGUI();
    }

    void toggleMode() {
        recordOnMoveAlways = !recordOnMoveAlways;
        updateGUI();
    }

    void startRecording() {
        recording = true;
        { std::lock_guard<std::mutex> lk(actionsMutex); actions.clear(); }
        startTime = std::chrono::steady_clock::now();
        recordStartTime = startTime;
        isRightButtonPressed = false;
        GetCursorPos(&lastMousePos);
        startRawProcessor();
        updateGUI();
    }

    void stopRecording() {
        recording = false;
        stopRawProcessor();
        updateGUI();
        if (!actions.empty()) {
            fs::create_directories("recordings");
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss;
            ss << "recordings/recording_" << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S") << ".json";
            saveRecording(ss.str());
            refreshRecordingsList();
        }
    }

    void playLast() {
        std::lock_guard<std::mutex> lk(actionsMutex);
        if (!actions.empty() && !playbackRunning) {
            // start playback with current loop config
            std::thread(&KeyboardMouseRecorder::playRecording, this, loopEnabled, loopTimes).detach();
        }
    }

    void stopPlayback() {
        loopPlayback = false;
        playbackRunning = false;
        updateGUI();
    }

    void emergencyStop() {
        recording = false;
        loopPlayback = false;
        playbackRunning = false;
        stopRawProcessor();
        updateGUI();
    }

    void saveRecording(const std::string& filename) {
        try {
            json j = json::array();
            { std::lock_guard<std::mutex> lk(actionsMutex);
                for (const auto& action : actions) {
                    json actionJson;
                    actionJson["time"] = action.time;
                    switch (action.type) {
                        case ActionType::MOUSE_MOVE:
                            actionJson["type"] = "mouse_move";
                            actionJson["x"] = action.x; actionJson["y"] = action.y;
                            actionJson["deltaX"] = action.deltaX; actionJson["deltaY"] = action.deltaY;
                            break;
                        case ActionType::MOUSE_DELTA:
                            actionJson["type"] = "mouse_delta";
                            actionJson["deltaX"] = action.deltaX; actionJson["deltaY"] = action.deltaY;
                            actionJson["isRaw"] = true;
                            break;
                        case ActionType::MOUSE_PRESS:
                        case ActionType::MOUSE_RELEASE:
                            actionJson["type"] = (action.type == ActionType::MOUSE_PRESS) ? "mouse_press" : "mouse_release";
                            actionJson["x"] = action.x; actionJson["y"] = action.y;
                            actionJson["button"] = action.button;
                            break;
                        case ActionType::MOUSE_SCROLL:
                            actionJson["type"] = "mouse_scroll";
                            actionJson["x"] = action.x; actionJson["y"] = action.y;
                            actionJson["dx"] = action.scrollDx; actionJson["dy"] = action.scrollDy;
                            break;
                        case ActionType::KEY_PRESS:
                        case ActionType::KEY_RELEASE:
                            actionJson["type"] = (action.type == ActionType::KEY_PRESS) ? "key_press" : "key_release";
                            actionJson["key"] = action.key;
                            actionJson["vkCode"] = action.vkCode;
                            break;
                    }
                    j.push_back(actionJson);
                }
            }
            std::ofstream file(filename);
            file << j.dump(2);
        } catch (...) {}
    }

    bool loadRecording(const std::string& filename) {
        try {
            std::ifstream file(filename);
            json j = json::parse(file);
            { std::lock_guard<std::mutex> lk(actionsMutex);
                actions.clear();
                for (const auto& actionJson : j) {
                    Action action;
                    action.time = actionJson.at("time");
                    std::string typeStr = actionJson.at("type");
                    if (typeStr == "mouse_move") {
                        action.type = ActionType::MOUSE_MOVE;
                        action.x = actionJson.at("x"); action.y = actionJson.at("y");
                        if (actionJson.contains("deltaX")) action.deltaX = actionJson.at("deltaX");
                        if (actionJson.contains("deltaY")) action.deltaY = actionJson.at("deltaY");
                    } else if (typeStr == "mouse_delta") {
                        action.type = ActionType::MOUSE_DELTA;
                        action.deltaX = actionJson.at("deltaX"); action.deltaY = actionJson.at("deltaY");
                        action.isRawDelta = actionJson.value("isRaw", false);
                    } else if (typeStr == "mouse_press") {
                        action.type = ActionType::MOUSE_PRESS;
                        action.x = actionJson.at("x"); action.y = actionJson.at("y");
                        action.button = actionJson.at("button");
                    } else if (typeStr == "mouse_release") {
                        action.type = ActionType::MOUSE_RELEASE;
                        action.x = actionJson.at("x"); action.y = actionJson.at("y");
                        action.button = actionJson.at("button");
                    } else if (typeStr == "mouse_scroll") {
                        action.type = ActionType::MOUSE_SCROLL;
                        action.x = actionJson.at("x"); action.y = actionJson.at("y");
                        action.scrollDx = actionJson.at("dx"); action.scrollDy = actionJson.at("dy");
                    } else if (typeStr == "key_press") {
                        action.type = ActionType::KEY_PRESS;
                        action.key = actionJson.at("key");
                        action.vkCode = actionJson.value("vkCode", 0);
                    } else if (typeStr == "key_release") {
                        action.type = ActionType::KEY_RELEASE;
                        action.key = actionJson.at("key");
                        action.vkCode = actionJson.value("vkCode", 0);
                    }
                    actions.push_back(action);
                }
            }
            return true;
        } catch (...) { return false; }
    }

    // playRecording now supports loop flag + count (0 = infinite if loop==true)
    void playRecording(bool loop, int loopCount) {
        {
            std::lock_guard<std::mutex> lk(actionsMutex);
            if (actions.empty()) return;
        }

        playbackRunning = true;
        loopPlayback = loop;
        updateGUI();

        std::this_thread::sleep_for(std::chrono::seconds(2));

        // local snapshot outside loop to avoid re-locking next iterations
        std::vector<Action> localActions;
        {
            std::lock_guard<std::mutex> lk(actionsMutex);
            localActions = actions;
        }

        auto doPlayOnce = [&](void)->bool {
            auto playbackStart = std::chrono::steady_clock::now();
            std::unordered_set<WORD> keysDown;
            double fracAccX = 0.0, fracAccY = 0.0;

            for (size_t idx = 0; idx < localActions.size(); ++idx) {
                if (!playbackRunning) break;

                const Action &action = localActions[idx];
                auto targetTime = playbackStart + std::chrono::milliseconds(static_cast<long long>(action.time * 1000.0 + 0.5));
                std::this_thread::sleep_until(targetTime);

                try {
                    switch (action.type) {
                        case ActionType::MOUSE_MOVE: {
                            int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                            int screenHeight = GetSystemMetrics(SM_CYSCREEN);
                            LONG normalizedX = static_cast<LONG>((action.x * 65535) / screenWidth);
                            LONG normalizedY = static_cast<LONG>((action.y * 65535) / screenHeight);
                            INPUT input = {0};
                            input.type = INPUT_MOUSE;
                            input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                            input.mi.dx = normalizedX; input.mi.dy = normalizedY;
                            SendInput(1, &input, sizeof(INPUT));
                            break;
                        }
                        case ActionType::MOUSE_DELTA: {
                            double outDx = action.deltaX * TUNING_SENSITIVITY * TUNING_PLAYBACK_VELOCITY;
                            double outDy = action.deltaY * TUNING_SENSITIVITY * TUNING_PLAYBACK_VELOCITY;
                            double toSendX = outDx + fracAccX;
                            double toSendY = outDy + fracAccY;
                            int ix = static_cast<int>(std::round(toSendX));
                            int iy = static_cast<int>(std::round(toSendY));
                            fracAccX = toSendX - ix; fracAccY = toSendY - iy;
                            if (ix != 0 || iy != 0) {
                                INPUT input = {0};
                                input.type = INPUT_MOUSE; input.mi.dwFlags = MOUSEEVENTF_MOVE;
                                input.mi.dx = ix; input.mi.dy = iy;
                                SendInput(1, &input, sizeof(INPUT));
                            }
                            break;
                        }
                        case ActionType::MOUSE_PRESS:
                        case ActionType::MOUSE_RELEASE: {
                            if (action.button == "right") {
                                DWORD flag = (action.type == ActionType::MOUSE_PRESS) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                                mouse_event(flag, 0, 0, 0, 0);
                            } else {
                                int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                                int screenHeight = GetSystemMetrics(SM_CYSCREEN);
                                LONG normalizedX = static_cast<LONG>((action.x * 65535) / screenWidth);
                                LONG normalizedY = static_cast<LONG>((action.y * 65535) / screenHeight);
                                INPUT moveInput = {0};
                                moveInput.type = INPUT_MOUSE;
                                moveInput.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                                moveInput.mi.dx = normalizedX; moveInput.mi.dy = normalizedY;
                                SendInput(1, &moveInput, sizeof(INPUT));
                                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                                if (action.button == "left") {
                                    DWORD flag = (action.type == ActionType::MOUSE_PRESS) ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
                                    mouse_event(flag, 0, 0, 0, 0);
                                } else if (action.button == "middle") {
                                    DWORD flag = (action.type == ActionType::MOUSE_PRESS) ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
                                    mouse_event(flag, 0, 0, 0, 0);
                                }
                            }
                            break;
                        }
                        case ActionType::MOUSE_SCROLL: {
                            INPUT input = {0};
                            input.type = INPUT_MOUSE; input.mi.dwFlags = MOUSEEVENTF_WHEEL;
                            input.mi.mouseData = action.scrollDy * WHEEL_DELTA;
                            SendInput(1, &input, sizeof(INPUT));
                            break;
                        }
                        case ActionType::KEY_PRESS:
                        case ActionType::KEY_RELEASE: {
                            WORD vk = (action.vkCode != 0) ? static_cast<WORD>(action.vkCode) : (VkKeyScanA(action.key[0]) & 0xFF);
                            if (vk == 0) break;
                            UINT scancode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
                            if (action.type == ActionType::KEY_PRESS) {
                                if (keysDown.find(vk) == keysDown.end()) {
                                    INPUT in = {0}; in.type = INPUT_KEYBOARD;
                                    in.ki.wScan = static_cast<WORD>(scancode);
                                    in.ki.dwFlags = KEYEVENTF_SCANCODE;
                                    SendInput(1, &in, sizeof(INPUT));
                                    keysDown.insert(vk);
                                }
                            } else {
                                if (keysDown.find(vk) != keysDown.end()) {
                                    INPUT in = {0}; in.type = INPUT_KEYBOARD;
                                    in.ki.wScan = static_cast<WORD>(scancode);
                                    in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
                                    SendInput(1, &in, sizeof(INPUT));
                                    keysDown.erase(vk);
                                }
                            }
                            break;
                        }
                    }
                } catch (...) {}
            }

            for (WORD vk : keysDown) {
                UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
                INPUT in = {0}; in.type = INPUT_KEYBOARD;
                in.ki.wScan = static_cast<WORD>(sc);
                in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
                SendInput(1, &in, sizeof(INPUT));
            }
            keysDown.clear();

            return playbackRunning; // return false if stopped during playback
        };

        if (loop && loopCount <= 0) {
            // infinite loop until stopped
            while (playbackRunning) {
                if (!doPlayOnce()) break;
            }
        } else {
            int remaining = loop ? loopCount : 1;
            while (remaining-- > 0 && playbackRunning) {
                if (!doPlayOnce()) break;
            }
        }

        playbackRunning = false;
        updateGUI();
    }

    void refreshRecordingsList() {
        if (!mainWindow) return;
        HWND hList = GetDlgItem(mainWindow, IDC_LIST_RECORDINGS);
        SendMessageW(hList, LB_RESETCONTENT, 0, 0);
        
        std::string folder = "recordings";
        if (fs::exists(folder)) {
            for (const auto& entry : fs::directory_iterator(folder)) {
                if (entry.is_regular_file()) {
                    std::string filename = entry.path().filename().string();
                    if (filename.find("recording_") == 0 && filename.find(".json") != std::string::npos) {
                        std::wstring wname(filename.begin(), filename.end());
                        SendMessageW(hList, LB_ADDSTRING, 0, (LPARAM)wname.c_str());
                    }
                }
            }
        }
    }

    void loadSelectedRecording() {
        HWND hList = GetDlgItem(mainWindow, IDC_LIST_RECORDINGS);
        int sel = (int)SendMessageW(hList, LB_GETCURSEL, 0, 0);
        if (sel != LB_ERR) {
            wchar_t buf[256];
            SendMessageW(hList, LB_GETTEXT, sel, (LPARAM)buf);
            std::wstring wname(buf);
            std::string filename(wname.begin(), wname.end());
            std::string path = "recordings/" + filename;
            if (loadRecording(path)) {
                updateGUI();
            }
        }
    }

    void startListeners() {
        instance = this;
        createRawInputWindow();
        mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, nullptr, 0);
        keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, nullptr, 0);
    }

    void stopListeners() {
        if (mouseHook) UnhookWindowsHookEx(mouseHook);
        if (keyboardHook) UnhookWindowsHookEx(keyboardHook);
        if (hiddenWindow) DestroyWindow(hiddenWindow);
        stopRawProcessor();
    }
};

KeyboardMouseRecorder* KeyboardMouseRecorder::instance = nullptr;

// GUI Window Procedure
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    static KeyboardMouseRecorder* recorder = nullptr;
    
    switch (msg) {
        case WM_CREATE: {
            recorder = reinterpret_cast<KeyboardMouseRecorder*>(
                ((CREATESTRUCT*)lParam)->lpCreateParams);
            recorder->setMainWindow(hwnd);

            // Status text
            CreateWindowW(L"STATIC", L"Status: Idle", WS_VISIBLE | WS_CHILD | SS_LEFT,
                20, 20, 560, 60, hwnd, (HMENU)IDC_STATUS_TEXT, nullptr, nullptr);

            // Control buttons
            CreateWindowW(L"BUTTON", L"🔴 Record [F1]", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                20, 90, 130, 35, hwnd, (HMENU)IDC_BTN_RECORD, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"▶️ Play [F2]", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                160, 90, 130, 35, hwnd, (HMENU)IDC_BTN_PLAY, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"⏹️ Stop [F3]", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                300, 90, 130, 35, hwnd, (HMENU)IDC_BTN_STOP, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"🔄 Mode [F4]", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                440, 90, 140, 35, hwnd, (HMENU)IDC_BTN_TOGGLE_MODE, nullptr, nullptr);

            // Recordings list
            CreateWindowW(L"STATIC", L"Recordings:", WS_VISIBLE | WS_CHILD,
                20, 140, 200, 20, hwnd, nullptr, nullptr, nullptr);
            CreateWindowW(L"LISTBOX", nullptr, WS_VISIBLE | WS_CHILD | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
                20, 165, 450, 200, hwnd, (HMENU)IDC_LIST_RECORDINGS, nullptr, nullptr);
            CreateWindowW(L"BUTTON", L"Load & Play", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                480, 165, 100, 30, hwnd, (HMENU)IDC_BTN_LOAD, nullptr, nullptr);

            // Settings
            CreateWindowW(L"STATIC", L"Sensitivity:", WS_VISIBLE | WS_CHILD,
                20, 380, 100, 20, hwnd, nullptr, nullptr, nullptr);
            CreateWindowW(L"EDIT", L"1.00", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT,
                120, 378, 80, 22, hwnd, (HMENU)IDC_EDIT_SENS, nullptr, nullptr);
            
            CreateWindowW(L"STATIC", L"Ramp (ms):", WS_VISIBLE | WS_CHILD,
                220, 380, 100, 20, hwnd, nullptr, nullptr, nullptr);
            CreateWindowW(L"EDIT", L"40", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT,
                320, 378, 80, 22, hwnd, (HMENU)IDC_EDIT_RAMP, nullptr, nullptr);
            
            CreateWindowW(L"BUTTON", L"Save Settings", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                420, 375, 100, 28, hwnd, (HMENU)IDC_BTN_SAVE_SETTINGS, nullptr, nullptr);

            // NEW: Loop checkbox + times
            CreateWindowW(L"BUTTON", L"🔁 Loop", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                20, 420, 100, 25, hwnd, (HMENU)IDC_BTN_LOOP, nullptr, nullptr);
            CreateWindowW(L"STATIC", L"Times (0=∞):", WS_VISIBLE | WS_CHILD,
                130, 422, 80, 20, hwnd, nullptr, nullptr, nullptr);
            CreateWindowW(L"EDIT", L"1", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_LEFT,
                210, 420, 80, 22, hwnd, (HMENU)IDC_EDIT_LOOP_COUNT, nullptr, nullptr);

            // Timer for updates
            SetTimer(hwnd, IDC_TIMER_UPDATE, 100, nullptr);
            
            recorder->refreshRecordingsList();
            return 0;
        }

        case WM_TIMER:
            if (wParam == IDC_TIMER_UPDATE && recorder) {
                recorder->updateGUI();
            }
            return 0;

        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            switch (wmId) {
                case IDC_BTN_RECORD:
                    if (recorder) recorder->toggleRecording();
                    break;
                case IDC_BTN_PLAY:
                    if (recorder) {
                        // Read loop checkbox and count, set recorder config, then play
                        bool checked = (SendMessageW(GetDlgItem(hwnd, IDC_BTN_LOOP), BM_GETCHECK, 0, 0) == BST_CHECKED);
                        recorder->setLoopEnabled(checked);
                        wchar_t buf[32];
                        GetDlgItemTextW(hwnd, IDC_EDIT_LOOP_COUNT, buf, 32);
                        int cnt = _wtoi(buf);
                        recorder->setLoopTimes(cnt);
                        recorder->playLast();
                    }
                    break;
                case IDC_BTN_STOP:
                    if (recorder) recorder->stopPlayback();
                    break;
                case IDC_BTN_TOGGLE_MODE:
                    if (recorder) recorder->toggleMode();
                    break;
                case IDC_BTN_LOAD:
                    if (recorder) {
                        recorder->loadSelectedRecording();
                        // optionally auto-play when loading? current behavior just loads; user can press Play
                    }
                    break;
                case IDC_LIST_RECORDINGS:
                    if (HIWORD(wParam) == LBN_DBLCLK && recorder) {
                        recorder->loadSelectedRecording();
                    }
                    break;
                case IDC_BTN_SAVE_SETTINGS: {
                    wchar_t buf[32];
                    GetDlgItemTextW(hwnd, IDC_EDIT_SENS, buf, 32);
                    TUNING_SENSITIVITY = (float)_wtof(buf);
                    GetDlgItemTextW(hwnd, IDC_EDIT_RAMP, buf, 32);
                    STOP_RAMP_MS = _wtoi(buf);
                    MessageBoxW(hwnd, L"Settings saved!", L"Info", MB_OK);
                    break;
                }
            }
            return 0;
        }

        case WM_DESTROY:
            KillTimer(hwnd, IDC_TIMER_UPDATE);
            if (recorder) recorder->stopListeners();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    InitCommonControls();
    
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"RecorderMainClass";
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);

    KeyboardMouseRecorder recorder;
    
    HWND hwnd = CreateWindowExW(0, L"RecorderMainClass",
        L"Keyboard & Mouse Recorder - GUI Edition",
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 620, 480,
        nullptr, nullptr, hInstance, &recorder);

    if (!hwnd) return 0;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    
    recorder.startListeners();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    return 0;
}
