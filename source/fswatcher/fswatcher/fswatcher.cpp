#include <windows.h>
#include <string>
#include <queue>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <chrono>
#include <vector>

// ----------------------------------------------------
// GLOBAL STATE
// ----------------------------------------------------

static HANDLE dirHandle = INVALID_HANDLE_VALUE;
static HANDLE stopEvent = INVALID_HANDLE_VALUE;
static std::thread worker;
static std::queue<std::string> eventQueue;
static std::mutex queueMutex;
static std::atomic<bool> running(false);

static std::string basePath;
static std::string renameOldPath;

// Debounce map
static std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastModifiedTime;
static const int DEBOUNCE_MS = 100;

// ----------------------------------------------------
// Helpers
// ----------------------------------------------------

std::string utf16_to_utf8(const std::wstring& wstr)
{
    if (wstr.empty()) return std::string();
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), NULL, 0, NULL, NULL);
    std::string strTo(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0, &wstr[0], (int)wstr.size(), &strTo[0], size_needed, NULL, NULL);
    return strTo;
}

bool shouldEmitModified(const std::string& path)
{
    using namespace std::chrono;
    auto now = steady_clock::now();

    auto it = lastModifiedTime.find(path);
    if (it != lastModifiedTime.end())
    {
        auto diff = duration_cast<milliseconds>(now - it->second).count();
        if (diff < DEBOUNCE_MS)
            return false;
    }

    lastModifiedTime[path] = now;
    return true;
}

// ----------------------------------------------------
// Worker Thread
// ----------------------------------------------------

void watchThread()
{
    std::vector<BYTE> buffer(16384);
    OVERLAPPED overlapped = { 0 };
    overlapped.hEvent = CreateEvent(NULL, TRUE, FALSE, NULL);

    HANDLE events[2] = { overlapped.hEvent, stopEvent };

    while (running)
    {
        DWORD bytesReturned = 0;
        BOOL result = ReadDirectoryChangesW(
            dirHandle,
            buffer.data(),
            (DWORD)buffer.size(),
            TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE,
            NULL,
            &overlapped,
            NULL);

        if (!result && GetLastError() != ERROR_IO_PENDING)
        {
            break;
        }

        DWORD waitResult = WaitForMultipleObjects(2, events, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0) // File system event
        {
            if (GetOverlappedResult(dirHandle, &overlapped, &bytesReturned, FALSE) && bytesReturned > 0)
            {
                FILE_NOTIFY_INFORMATION* info = (FILE_NOTIFY_INFORMATION*)buffer.data();

                while (true)
                {
                    std::wstring ws(info->FileName, info->FileNameLength / sizeof(WCHAR));
                    std::string filename = utf16_to_utf8(ws);
                    std::string fullPath = basePath + "\\" + filename;

                    std::string message;

                    switch (info->Action)
                    {
                    case FILE_ACTION_ADDED:
                        message = "CREATED|" + fullPath;
                        break;
                    case FILE_ACTION_REMOVED:
                        message = "DELETED|" + fullPath;
                        break;
                    case FILE_ACTION_MODIFIED:
                        if (shouldEmitModified(fullPath))
                            message = "MODIFIED|" + fullPath;
                        break;
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        renameOldPath = fullPath;
                        break;
                    case FILE_ACTION_RENAMED_NEW_NAME:
                        message = "RENAMED|" + renameOldPath + "|" + fullPath;
                        renameOldPath.clear();
                        break;
                    }

                    if (!message.empty())
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        eventQueue.push(message);
                    }

                    if (!info->NextEntryOffset) break;
                    info = (FILE_NOTIFY_INFORMATION*)((BYTE*)info + info->NextEntryOffset);
                }
            }
            ResetEvent(overlapped.hEvent);
        }
        else if (waitResult == WAIT_OBJECT_0 + 1) // Stop event
        {
            CancelIo(dirHandle);
            break;
        }
        else
        {
            break;
        }
    }

    CloseHandle(overlapped.hEvent);
}

// ----------------------------------------------------
// EXPORTS
// ----------------------------------------------------

extern "C" __declspec(dllexport)
double fswatcher_start(const char* path)
{
    if (running) return 0;

    basePath = path;
    // Remove trailing backslash if present
    if (!basePath.empty() && (basePath.back() == '\\' || basePath.back() == '/')) {
        basePath.pop_back();
    }

    dirHandle = CreateFileA(
        path,
        FILE_LIST_DIRECTORY,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
        NULL);

    if (dirHandle == INVALID_HANDLE_VALUE) return 0;

    stopEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
    running = true;
    worker = std::thread(watchThread);

    return 1;
}

extern "C" __declspec(dllexport)
const char* fswatcher_poll()
{
    static std::string result;
    std::lock_guard<std::mutex> lock(queueMutex);

    if (eventQueue.empty()) return "";

    result = eventQueue.front();
    eventQueue.pop();

    return result.c_str();
}

extern "C" __declspec(dllexport)
double fswatcher_stop()
{
    if (!running) return 0;

    running = false;
    if (stopEvent != INVALID_HANDLE_VALUE) {
        SetEvent(stopEvent);
    }

    if (worker.joinable())
        worker.join();

    if (dirHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(dirHandle);
        dirHandle = INVALID_HANDLE_VALUE;
    }

    if (stopEvent != INVALID_HANDLE_VALUE) {
        CloseHandle(stopEvent);
        stopEvent = INVALID_HANDLE_VALUE;
    }

    // Cleanup queue
    std::lock_guard<std::mutex> lock(queueMutex);
    while (!eventQueue.empty()) eventQueue.pop();

    lastModifiedTime.clear();
    renameOldPath.clear();

    return 1;
}

// ----------------------------------------------------
// DllMain
// ----------------------------------------------------

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    if (ul_reason_for_call == DLL_PROCESS_DETACH) {
        fswatcher_stop();
    }
    return TRUE;
}

