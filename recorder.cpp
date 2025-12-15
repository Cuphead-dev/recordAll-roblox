#define UNICODE
#define _UNICODE

#include <windows.h>
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

using json = nlohmann::json;
namespace fs = std::filesystem;

// -------------------------- TUNABLE PARAMETERS --------------------------
// Edit these values to tune behavior for your hardware / Roblox feel
static float TUNING_SENSITIVITY         = 1.00f;   // sensitivity applied AFTER filtering (match recording)
static float TUNING_PLAYBACK_VELOCITY   = 1.00f;   // set to 1.0 for 'exact' playback; adjust if you need compensation
static float TUNING_SMOOTH_ALPHA        = 0.70f;   // EMA smoothing while moving (0..1) - larger = less smoothing
static int   TUNING_STOP_THRESHOLD      = 1;       // |delta| <= this counts as "micro stop"
static int   TUNING_STOP_FRAMES         = 2;       // consecutive ticks within threshold to consider STOP
static int   RAW_TICK_MS                = 4;       // raw processing tick in ms (~250 Hz)
// New playback ramp tuning
static int   STOP_RAMP_MS               = 40;      // ms to ramp down instead of abrupt stop
static double RAMP_DECAY                = 0.45;    // decay factor per step (0..1) — lower => faster decay
static bool   ENABLE_PLAYBACK_RAMP      = true;    // master switch for ramp feature

// === Axis-specific raw sensitivity compensation ===
// If your game (Roblox) internally scales Y (vertical) smaller, increase RAW_SENS_Y.
// Typical starting suggestion: RAW_SENS_Y = 1.25 (makes vertical movement bigger).
static double RAW_SENS_X = 1.0;    // adjust if X needs compensation
static double RAW_SENS_Y = 1.0;   // increase if Y feels too small (your reported issue)
// ----------------------------------------------------------------------

enum class ActionType {
    MOUSE_MOVE,
    MOUSE_DELTA,      // Raw mouse delta movement
    MOUSE_PRESS,
    MOUSE_RELEASE,
    MOUSE_SCROLL,
    KEY_PRESS,
    KEY_RELEASE
};

struct Action {
    ActionType type;
    int x = 0, y = 0;                // Absolute position
    double deltaX = 0.0, deltaY = 0.0; // Raw delta (double precision)
    std::string button;
    std::string key;
    DWORD vkCode = 0;
    int scrollDx = 0, scrollDy = 0;
    double time = 0.0;
    bool isRawDelta = false;
};

struct RawDelta {
    int dx;
    int dy;
    double time;
};

class KeyboardMouseRecorder {
private:
    bool recording = false;
    bool loopPlayback = false;
    bool shouldExit = false;
    bool playbackRunning = false;
    bool recordOnMoveAlways = false; // when true records raw deltas even without RMB (Roblox mode)
    std::vector<Action> actions;
    std::chrono::steady_clock::time_point startTime;
    HHOOK mouseHook = nullptr;
    HHOOK keyboardHook = nullptr;
    HWND hiddenWindow = nullptr;

    POINT lastMousePos = {0, 0};
    bool isRightButtonPressed = false;

    static KeyboardMouseRecorder* instance;

    std::mutex actionsMutex;

    // Raw delta buffering and processing
    std::mutex rawMutex;
    std::deque<RawDelta> rawQueue;
    std::atomic<bool> rawProcessorRunning{false};
    std::thread rawProcessorThread;

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

    // Raw input window proc
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
                // allow recording when RMB is down OR when recordOnMoveAlways is true
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
        if (!RegisterClassExW(&wc)) {
            std::cerr << "Failed to register window class!" << std::endl;
            return;
        }
        hiddenWindow = CreateWindowExW(
            0, L"RawInputClass", L"RawInputWindow",
            0, 0, 0, 0, 0,
            HWND_MESSAGE, NULL, GetModuleHandleW(NULL), NULL
        );
        if (!hiddenWindow) {
            std::cerr << "Failed to create hidden window!" << std::endl;
            return;
        }
        RAWINPUTDEVICE rid[1];
        rid[0].usUsagePage = 0x01;
        rid[0].usUsage = 0x02;
        rid[0].dwFlags = RIDEV_INPUTSINK;
        rid[0].hwndTarget = hiddenWindow;
        if (!RegisterRawInputDevices(rid, 1, sizeof(RAWINPUTDEVICE))) {
            std::cerr << "Failed to register raw input device!" << std::endl;
        }
    }

    // Start raw processor: process each raw delta individually and preserve its original timestamp
    void startRawProcessor() {
        rawProcessorRunning = true;
        rawProcessorThread = std::thread([this]() {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            double smoothedX = 0.0, smoothedY = 0.0;
            int consecutiveSmall = 0;
            bool stopped = false;
            // keep a short history to compute recent average velocity
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
                int dx = rd.dx;
                int dy = rd.dy;

                // update recent
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
                        // compute small average of recent movement to determine ramp start velocity
                        double avgX = 0.0, avgY = 0.0;
                        if (!recent.empty()) {
                            for (auto &p : recent) { avgX += p.first; avgY += p.second; }
                            avgX /= static_cast<double>(recent.size());
                            avgY /= static_cast<double>(recent.size());
                        }
                        // create ramp actions rather than single immediate stop marker
                        if (ENABLE_PLAYBACK_RAMP) {
                            int rampSteps = std::max(1, STOP_RAMP_MS / RAW_TICK_MS);
                            double startX = smoothedX == 0.0 ? avgX : smoothedX;
                            double startY = smoothedY == 0.0 ? avgY : smoothedY;
                            double t0 = rd.time;
                            for (int k = 1; k <= rampSteps; ++k) {
                                double fracPrev = std::pow(RAMP_DECAY, (double)(k-1));
                                double fracCurr = std::pow(RAMP_DECAY, (double)k);
                                // delta for this ramp step = start*(fracPrev - fracCurr)
                                double stepX = startX * (fracPrev - fracCurr);
                                double stepY = startY * (fracPrev - fracCurr);

                                // APPLY RAW_SENS compensation NOW so actions store compensated deltas
                                Action ra;
                                ra.type = ActionType::MOUSE_DELTA;
                                ra.deltaX = stepX * RAW_SENS_X;
                                ra.deltaY = stepY * RAW_SENS_Y;
                                // schedule slightly after the last real sample
                                ra.time = t0 + (k * RAW_TICK_MS) / 1000.0;
                                ra.isRawDelta = true;
                                {
                                    std::lock_guard<std::mutex> lk(actionsMutex);
                                    actions.push_back(ra);
                                }
                            }
                        } else {
                            // fallback: push single stop marker
                            Action stopMarker;
                            stopMarker.type = ActionType::MOUSE_DELTA;
                            stopMarker.deltaX = 0.0;
                            stopMarker.deltaY = 0.0;
                            stopMarker.time = rd.time;
                            stopMarker.isRawDelta = true;
                            {
                                std::lock_guard<std::mutex> lk(actionsMutex);
                                actions.push_back(stopMarker);
                            }
                        }

                        smoothedX = 0.0;
                        smoothedY = 0.0;
                        stopped = true;
                    }
                } else {
                    stopped = false;
                    // EMA smoothing per-sample (light)
                    smoothedX = TUNING_SMOOTH_ALPHA * static_cast<double>(dx) + (1.0 - TUNING_SMOOTH_ALPHA) * smoothedX;
                    smoothedY = TUNING_SMOOTH_ALPHA * static_cast<double>(dy) + (1.0 - TUNING_SMOOTH_ALPHA) * smoothedY;
                    Action a;
                    a.type = ActionType::MOUSE_DELTA;

                    // APPLY RAW_SENS compensation BEFORE storing
                    a.deltaX = smoothedX * RAW_SENS_X;
                    a.deltaY = smoothedY * RAW_SENS_Y;

                    a.time = rd.time; // preserve original timestamp
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
            action.deltaX = 0.0;
            action.deltaY = 0.0;
            action.scrollDx = 0;
            action.scrollDy = 0;
            POINT cursorPos;
            GetCursorPos(&cursorPos);
            action.x = cursorPos.x;
            action.y = cursorPos.y;

            switch (wParam) {
                case WM_MOUSEMOVE: {
                    if (!instance->isRightButtonPressed) {
                        // Original behavior: record absolute mouse moves when not rotating camera
                        action.type = ActionType::MOUSE_MOVE;
                        action.deltaX = static_cast<double>(cursorPos.x - instance->lastMousePos.x);
                        action.deltaY = static_cast<double>(cursorPos.y - instance->lastMousePos.y);
                        std::lock_guard<std::mutex> lk(instance->actionsMutex);
                        instance->actions.push_back(action);
                    }
                    instance->lastMousePos = cursorPos;
                    break;
                }

                case WM_LBUTTONDOWN:
                case WM_MBUTTONDOWN: {
                    action.type = ActionType::MOUSE_PRESS;
                    action.button = instance->getButtonName(static_cast<UINT>(wParam));
                    std::lock_guard<std::mutex> lk(instance->actionsMutex);
                    instance->actions.push_back(action);
                    break;
                }

                case WM_RBUTTONDOWN: {
                    action.type = ActionType::MOUSE_PRESS;
                    action.button = "right";
                    std::lock_guard<std::mutex> lk(instance->actionsMutex);
                    instance->actions.push_back(action);
                    instance->isRightButtonPressed = true;
                    instance->lastMousePos = cursorPos;
                    break;
                }

                case WM_LBUTTONUP:
                case WM_MBUTTONUP: {
                    action.type = ActionType::MOUSE_RELEASE;
                    action.button = instance->getButtonName(static_cast<UINT>(wParam));
                    std::lock_guard<std::mutex> lk(instance->actionsMutex);
                    instance->actions.push_back(action);
                    break;
                }

                case WM_RBUTTONUP: {
                    // Immediately flush raw queue, but instead of immediate zero we rely on raw-processor ramp
                    instance->isRightButtonPressed = false;
                    {
                        std::lock_guard<std::mutex> lk(instance->rawMutex);
                        instance->rawQueue.clear();
                    }
                    // we push a (soft) stop marker only if ramping disabled; otherwise raw-processor already queued ramp steps
                    if (!ENABLE_PLAYBACK_RAMP) {
                        Action stopAction;
                        stopAction.type = ActionType::MOUSE_DELTA;
                        stopAction.deltaX = 0.0;
                        stopAction.deltaY = 0.0;
                        stopAction.time = instance->getCurrentTime();
                        stopAction.isRawDelta = true;
                        {
                            std::lock_guard<std::mutex> lk(instance->actionsMutex);
                            instance->actions.push_back(stopAction);
                        }
                    }

                    action.type = ActionType::MOUSE_RELEASE;
                    action.button = "right";
                    {
                        std::lock_guard<std::mutex> lk(instance->actionsMutex);
                        instance->actions.push_back(action);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                    break;
                }

                case WM_MOUSEWHEEL: {
                    action.type = ActionType::MOUSE_SCROLL;
                    action.scrollDx = 0;
                    action.scrollDy = GET_WHEEL_DELTA_WPARAM(mouseInfo->mouseData) / WHEEL_DELTA;
                    std::lock_guard<std::mutex> lk(instance->actionsMutex);
                    instance->actions.push_back(action);
                    break;
                }
            }
        }
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    static LRESULT CALLBACK KeyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam) {
        if (nCode >= 0 && instance) {
            KBDLLHOOKSTRUCT* keyInfo = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
            if (keyInfo->vkCode == 'T' && wParam == WM_KEYDOWN) {
                instance->toggleRecording();
                return 1;
            }
            if (instance->recording) {
                bool isRepeat = (keyInfo->flags & LLKHF_INJECTED) ||
                               ((wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) &&
                                (keyInfo->flags & 0x80));
                if (!isRepeat) {
                    Action action;
                    action.time = instance->getCurrentTime();
                    action.key = instance->getKeyName(keyInfo->vkCode);
                    action.vkCode = keyInfo->vkCode;
                    action.isRawDelta = false;
                    action.deltaX = 0.0;
                    action.deltaY = 0.0;
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
    void toggleRecording() {
        if (!recording && !loopPlayback) {
            startRecording();
        } else if (loopPlayback) {
            recording = false;
            loopPlayback = false;
            std::cout << "Loop playback stopped!" << std::endl;
        } else {
            stopRecording();
        }
    }

    // Expose mode switch
    void setRecordOnMoveAlways(bool v) {
        recordOnMoveAlways = v;
        std::cout << "Record-on-move-always set to: " << (v ? "ON" : "OFF") << std::endl;
    }
    bool getRecordOnMoveAlways() const { return recordOnMoveAlways; }

    void startRecording() {
        recording = true;
        {
            std::lock_guard<std::mutex> lk(actionsMutex);
            actions.clear();
        }
        startTime = std::chrono::steady_clock::now();
        isRightButtonPressed = false;
        GetCursorPos(&lastMousePos);
        std::cout << "Recording started! Press T again to stop." << std::endl;
        std::cout << "Raw Input enabled - camera rotation will be smooth!" << std::endl;
        startRawProcessor();
    }

    void stopRecording() {
        recording = false;
        stopRawProcessor();
        std::cout << "Recording stopped!" << std::endl;
        {
            std::lock_guard<std::mutex> lk(actionsMutex);
            std::cout << "Captured " << actions.size() << " actions." << std::endl;
        }
        if (!actions.empty()) {
            fs::create_directories("recordings");
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            std::stringstream ss;
            ss << "recordings/recording_" << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S") << ".json";
            std::string filename = ss.str();
            saveRecording(filename);
            std::cout << "Playing back recording from " << filename << " ..." << std::endl;
            std::thread(&KeyboardMouseRecorder::playRecording, this, false).detach();
        }
    }

    void saveRecording(const std::string& filename) {
        try {
            json j = json::array();
            {
                std::lock_guard<std::mutex> lk(actionsMutex);
                for (const auto& action : actions) {
                    json actionJson;
                    actionJson["time"] = action.time;
                    switch (action.type) {
                        case ActionType::MOUSE_MOVE:
                            actionJson["type"] = "mouse_move";
                            actionJson["x"] = action.x;
                            actionJson["y"] = action.y;
                            actionJson["deltaX"] = action.deltaX;
                            actionJson["deltaY"] = action.deltaY;
                            break;
                        case ActionType::MOUSE_DELTA:
                            actionJson["type"] = "mouse_delta";
                            actionJson["deltaX"] = action.deltaX;
                            actionJson["deltaY"] = action.deltaY;
                            actionJson["isRaw"] = true;
                            break;
                        case ActionType::MOUSE_PRESS:
                            actionJson["type"] = "mouse_press";
                            actionJson["x"] = action.x;
                            actionJson["y"] = action.y;
                            actionJson["button"] = action.button;
                            break;
                        case ActionType::MOUSE_RELEASE:
                            actionJson["type"] = "mouse_release";
                            actionJson["x"] = action.x;
                            actionJson["y"] = action.y;
                            actionJson["button"] = action.button;
                            break;
                        case ActionType::MOUSE_SCROLL:
                            actionJson["type"] = "mouse_scroll";
                            actionJson["x"] = action.x;
                            actionJson["y"] = action.y;
                            actionJson["dx"] = action.scrollDx;
                            actionJson["dy"] = action.scrollDy;
                            break;
                        case ActionType::KEY_PRESS:
                            actionJson["type"] = "key_press";
                            actionJson["key"] = action.key;
                            actionJson["vkCode"] = action.vkCode;
                            break;
                        case ActionType::KEY_RELEASE:
                            actionJson["type"] = "key_release";
                            actionJson["key"] = action.key;
                            actionJson["vkCode"] = action.vkCode;
                            break;
                    }
                    j.push_back(actionJson);
                }
            }
            std::ofstream file(filename);
            file << j.dump(2);
            file.close();
            std::cout << "Recording saved to: " << filename << std::endl;
        } catch (const std::exception& e) {
            std::cerr << "Error saving recording: " << e.what() << std::endl;
        }
    }

    bool loadRecording(const std::string& filename) {
        try {
            std::ifstream file(filename);
            json j = json::parse(file);
            {
                std::lock_guard<std::mutex> lk(actionsMutex);
                actions.clear();
                for (const auto& actionJson : j) {
                    Action action;
                    action.time = actionJson.at("time");
                    action.deltaX = 0.0;
                    action.deltaY = 0.0;
                    action.scrollDx = 0;
                    action.scrollDy = 0;
                    action.isRawDelta = false;
                    action.vkCode = 0;
                    std::string typeStr = actionJson.at("type");
                    if (typeStr == "mouse_move") {
                        action.type = ActionType::MOUSE_MOVE;
                        action.x = actionJson.at("x");
                        action.y = actionJson.at("y");
                        if (actionJson.contains("deltaX")) action.deltaX = actionJson.at("deltaX");
                        if (actionJson.contains("deltaY")) action.deltaY = actionJson.at("deltaY");
                    } else if (typeStr == "mouse_delta") {
                        action.type = ActionType::MOUSE_DELTA;
                        action.deltaX = actionJson.at("deltaX");
                        action.deltaY = actionJson.at("deltaY");
                        action.isRawDelta = actionJson.value("isRaw", false);
                    } else if (typeStr == "mouse_press") {
                        action.type = ActionType::MOUSE_PRESS;
                        action.x = actionJson.at("x");
                        action.y = actionJson.at("y");
                        action.button = actionJson.at("button");
                    } else if (typeStr == "mouse_release") {
                        action.type = ActionType::MOUSE_RELEASE;
                        action.x = actionJson.at("x");
                        action.y = actionJson.at("y");
                        action.button = actionJson.at("button");
                    } else if (typeStr == "mouse_scroll") {
                        action.type = ActionType::MOUSE_SCROLL;
                        action.x = actionJson.at("x");
                        action.y = actionJson.at("y");
                        action.scrollDx = actionJson.at("dx");
                        action.scrollDy = actionJson.at("dy");
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
            std::cout << "Loaded recording from: " << filename << std::endl;
            return true;
        } catch (const std::exception& e) {
            std::cerr << "Error loading recording: " << e.what() << std::endl;
            return false;
        }
    }

    // NEW playRecording: uses sleep_until, keysDown set, scancodes, ramp and fractional accumulation
    void playRecording(bool loop) {
        {
            std::lock_guard<std::mutex> lk(actionsMutex);
            if (actions.empty()) {
                std::cout << "No recording to play!" << std::endl;
                return;
            }
        }

        playbackRunning = true;
        loopPlayback = loop;
        std::cout << "Starting playback in 2 seconds..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));

        do {
            // snapshot
            std::vector<Action> localActions;
            {
                std::lock_guard<std::mutex> lk(actionsMutex);
                localActions = actions;
            }

            // base steady clock start point
            auto playbackStart = std::chrono::steady_clock::now();

            // track keys currently down to avoid duplicate downs and mismatched releases
            std::unordered_set<WORD> keysDown;

            // fractional accumulators for mouse delta rounding
            double fracAccX = 0.0, fracAccY = 0.0;

            for (size_t idx = 0; idx < localActions.size(); ++idx) {
                if (!loopPlayback && !playbackRunning) break;

                const Action &action = localActions[idx];

                // compute target timepoint
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
                            input.mi.dx = normalizedX;
                            input.mi.dy = normalizedY;
                            SendInput(1, &input, sizeof(INPUT));
                            break;
                        }

                        case ActionType::MOUSE_DELTA: {
                            // detect stop marker ramp (next action is zero-delta marker)
                            bool handledRamp = false;
                            if (ENABLE_PLAYBACK_RAMP && (idx + 1 < localActions.size())) {
                                const auto &nextAct = localActions[idx + 1];
                                if (nextAct.type == ActionType::MOUSE_DELTA && nextAct.isRawDelta &&
                                    std::abs(nextAct.deltaX) < 1e-9 && std::abs(nextAct.deltaY) < 1e-9) {
                                    // ramp from current action velocity down to zero over STOP_RAMP_MS
                                    int rampSteps = std::max(1, STOP_RAMP_MS / std::max(1, RAW_TICK_MS));
                                    double startX = action.deltaX * TUNING_SENSITIVITY * TUNING_PLAYBACK_VELOCITY;
                                    double startY = action.deltaY * TUNING_SENSITIVITY * TUNING_PLAYBACK_VELOCITY;
                                    for (int s = 1; s <= rampSteps; ++s) {
                                        double fracPrev = std::pow(RAMP_DECAY, (double)(s-1));
                                        double fracCurr = std::pow(RAMP_DECAY, (double)s);
                                        double stepFrac = fracPrev - fracCurr;
                                        double dx = startX * stepFrac;
                                        double dy = startY * stepFrac;
                                        double toSendX = dx + fracAccX;
                                        double toSendY = dy + fracAccY;
                                        int ix = static_cast<int>(std::round(toSendX));
                                        int iy = static_cast<int>(std::round(toSendY));
                                        fracAccX = toSendX - ix;
                                        fracAccY = toSendY - iy;
                                        if (ix != 0 || iy != 0) {
                                            INPUT in{}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MOVE; in.mi.dx = ix; in.mi.dy = iy;
                                            SendInput(1, &in, sizeof(INPUT));
                                        }
                                        std::this_thread::sleep_for(std::chrono::milliseconds(RAW_TICK_MS));
                                    }
                                    // skip the zero marker
                                    ++idx;
                                    handledRamp = true;
                                }
                            }

                            if (!handledRamp) {
                                double outDx = action.deltaX * TUNING_SENSITIVITY * TUNING_PLAYBACK_VELOCITY;
                                double outDy = action.deltaY * TUNING_SENSITIVITY * TUNING_PLAYBACK_VELOCITY;
                                double toSendX = outDx + fracAccX;
                                double toSendY = outDy + fracAccY;
                                int ix = static_cast<int>(std::round(toSendX));
                                int iy = static_cast<int>(std::round(toSendY));
                                fracAccX = toSendX - ix;
                                fracAccY = toSendY - iy;
                                if (ix != 0 || iy != 0) {
                                    INPUT input = {0};
                                    input.type = INPUT_MOUSE;
                                    input.mi.dwFlags = MOUSEEVENTF_MOVE;
                                    input.mi.dx = ix;
                                    input.mi.dy = iy;
                                    SendInput(1, &input, sizeof(INPUT));
                                }
                            }
                            break;
                        }

                        case ActionType::MOUSE_PRESS:
                        case ActionType::MOUSE_RELEASE: {
                            if (action.button == "right") {
                                DWORD flag = (action.type == ActionType::MOUSE_PRESS) ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
                                mouse_event(flag, 0, 0, 0, 0);
                                if (action.type == ActionType::MOUSE_RELEASE) std::this_thread::sleep_for(std::chrono::milliseconds(2));
                            } else {
                                int screenWidth = GetSystemMetrics(SM_CXSCREEN);
                                int screenHeight = GetSystemMetrics(SM_CYSCREEN);
                                LONG normalizedX = static_cast<LONG>((action.x * 65535) / screenWidth);
                                LONG normalizedY = static_cast<LONG>((action.y * 65535) / screenHeight);
                                INPUT moveInput = {0};
                                moveInput.type = INPUT_MOUSE;
                                moveInput.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
                                moveInput.mi.dx = normalizedX;
                                moveInput.mi.dy = normalizedY;
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
                            input.type = INPUT_MOUSE;
                            input.mi.dwFlags = MOUSEEVENTF_WHEEL;
                            input.mi.mouseData = action.scrollDy * WHEEL_DELTA;
                            SendInput(1, &input, sizeof(INPUT));
                            break;
                        }

                        case ActionType::KEY_PRESS:
                        case ActionType::KEY_RELEASE: {
                            // Resolve vk code fallback
                            WORD vk = 0;
                            if (action.vkCode != 0) vk = static_cast<WORD>(action.vkCode);
                            else if (!action.key.empty()) vk = (VkKeyScanA(action.key[0]) & 0xFF);
                            if (vk == 0) break; // skip unknown

                            UINT scancode = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);

                            bool isDown = (action.type == ActionType::KEY_PRESS);
                            bool isUp = (action.type == ActionType::KEY_RELEASE);

                            if (isDown) {
                                if (keysDown.find(vk) == keysDown.end()) {
                                    INPUT in = {0};
                                    in.type = INPUT_KEYBOARD;
                                    in.ki.wVk = 0;
                                    in.ki.wScan = static_cast<WORD>(scancode);
                                    in.ki.dwFlags = KEYEVENTF_SCANCODE;
                                    SendInput(1, &in, sizeof(INPUT));
                                    keysDown.insert(vk);
                                }
                            } else if (isUp) {
                                if (keysDown.find(vk) != keysDown.end()) {
                                    INPUT in = {0};
                                    in.type = INPUT_KEYBOARD;
                                    in.ki.wVk = 0;
                                    in.ki.wScan = static_cast<WORD>(scancode);
                                    in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
                                    SendInput(1, &in, sizeof(INPUT));
                                    keysDown.erase(vk);
                                }
                            }
                            break;
                        }

                        default:
                            break;
                    }
                } catch (const std::exception& e) {
                    std::cerr << "Error executing action: " << e.what() << std::endl;
                }
            }

            // release any keys still down to avoid stuck keys
            for (WORD vk : keysDown) {
                UINT sc = MapVirtualKeyA(vk, MAPVK_VK_TO_VSC);
                INPUT in = {0};
                in.type = INPUT_KEYBOARD;
                in.ki.wVk = 0;
                in.ki.wScan = static_cast<WORD>(sc);
                in.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
                SendInput(1, &in, sizeof(INPUT));
            }
            keysDown.clear();

            std::cout << "Playback completed!" << std::endl;
            if (loopPlayback) std::cout << "Looping again... (press T to stop)" << std::endl;
        } while (loopPlayback);

        playbackRunning = false;
    }

    std::vector<std::string> listRecordings() {
        std::string folder = "recordings";
        if (!fs::exists(folder)) fs::create_directories(folder);
        std::vector<std::string> recordings;
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (entry.is_regular_file()) {
                std::string filename = entry.path().filename().string();
                if (filename.find("recording_") == 0 && filename.find(".json") != std::string::npos) recordings.push_back(entry.path().string());
            }
        }
        if (!recordings.empty()) {
            std::cout << "Available recordings:" << std::endl;
            for (size_t i = 0; i < recordings.size(); i++) std::cout << "  " << (i + 1) << ". " << recordings[i] << std::endl;
        } else std::cout << "No recordings found." << std::endl;
        return recordings;
    }

    void interactiveMode() {
        while (!shouldExit) {
            std::cout << "\n" << std::string(50, '=') << std::endl;
            std::cout << "Keyboard & Mouse Recorder - Interactive Mode" << std::endl;
            std::cout << std::string(50, '=') << std::endl;
            std::cout << "Current mode: " << (recordOnMoveAlways ? "Roblox-compatible (record on move)" : "Original (RMB to rotate)") << std::endl;
            std::cout << "1. Start/Stop Recording (Press T)" << std::endl;
            std::cout << "2. Play Last Recording" << std::endl;
            std::cout << "3. Play Last Recording in Loop" << std::endl;
            std::cout << "4. Load and Play Recording File" << std::endl;
            std::cout << "5. List All Recordings" << std::endl;
            std::cout << "6. Exit" << std::endl;
            std::cout << "7. Toggle mode (Original <-> Roblox-compatible)" << std::endl;
            std::cout << std::string(50, '-') << std::endl;
            std::cout << "Choose option (1-7): ";
            std::string choice;
            std::getline(std::cin, choice);
            if (choice == "1") {
                std::cout << "Press T to toggle recording..." << std::endl;
            } else if (choice == "2") {
                std::lock_guard<std::mutex> lk(actionsMutex);
                if (!actions.empty()) std::thread(&KeyboardMouseRecorder::playRecording, this, false).detach();
                else std::cout << "No recording available. Record something first!" << std::endl;
            } else if (choice == "3") {
                std::lock_guard<std::mutex> lk(actionsMutex);
                if (!actions.empty()) std::thread(&KeyboardMouseRecorder::playRecording, this, true).detach();
                else std::cout << "No recording available. Record something first!" << std::endl;
            } else if (choice == "4") {
                auto recordings = listRecordings();
                if (!recordings.empty()) {
                    std::cout << "Enter recording number: ";
                    std::string numStr; std::getline(std::cin, numStr);
                    try {
                        int idx = std::stoi(numStr) - 1;
                        if (idx >= 0 && idx < static_cast<int>(recordings.size())) {
                            std::string selectedFile = recordings[idx];
                            if (loadRecording(selectedFile)) {
                                std::cout << "Playing back: " << selectedFile << std::endl;
                                std::thread(&KeyboardMouseRecorder::playRecording, this, false).detach();
                            }
                        } else std::cout << "Number out of range!" << std::endl;
                    } catch (...) { std::cout << "Invalid number!" << std::endl; }
                } else std::cout << "No recordings found." << std::endl;
            } else if (choice == "5") listRecordings();
            else if (choice == "6") { shouldExit = true; std::cout << "Exiting..." << std::endl; PostQuitMessage(0); break; }
            else if (choice == "7") {
                setRecordOnMoveAlways(!getRecordOnMoveAlways());
            }
            else std::cout << "Invalid option!" << std::endl;
        }
    }

    void startListeners() {
        instance = this;
        createRawInputWindow();
        mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseHookProc, nullptr, 0);
        keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardHookProc, nullptr, 0);
        if (!mouseHook || !keyboardHook) { std::cerr << "Failed to set hooks!" << std::endl; return; }
        std::cout << "Recorder started. Press T to start/stop recording." << std::endl;
        std::cout << "Toggle mode from menu (7) - Roblox-compatible records on move." << std::endl;
        std::cout << "RAW_SENS_X = " << RAW_SENS_X << "  RAW_SENS_Y = " << RAW_SENS_Y << std::endl;
    }

    void stopListeners() {
        if (mouseHook) UnhookWindowsHookEx(mouseHook);
        if (keyboardHook) UnhookWindowsHookEx(keyboardHook);
        if (hiddenWindow) DestroyWindow(hiddenWindow);
        stopRawProcessor();
    }

    void run() {
        startListeners();
        std::thread(&KeyboardMouseRecorder::interactiveMode, this).detach();
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        stopListeners();
    }
};

KeyboardMouseRecorder* KeyboardMouseRecorder::instance = nullptr;

int main() {
    // Ensure console output is UTF-8 friendly where available
    SetConsoleOutputCP(CP_UTF8);
    KeyboardMouseRecorder recorder;
    recorder.run();
    return 0;
}
