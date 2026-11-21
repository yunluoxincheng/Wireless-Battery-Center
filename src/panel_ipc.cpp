#include "panel_ipc.h"

#include <aclapi.h>
#include <vector>

namespace panel {
namespace {

enum class Operation { Connect, Read, Write, Drain };

HANDLE pipe = INVALID_HANDLE_VALUE;
HANDLE event = nullptr;
PTP_WAIT wait = nullptr;
OVERLAPPED io{};
HWND hwnd = nullptr;
UINT completionMessage = 0;
RequestHandler handleRequest = nullptr;
Operation operation = Operation::Connect;
bool pending = false;
ULONGLONG connectedAt = 0;
char buffer[1024];
std::string request, response;

HANDLE CreatePrivatePipe() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return INVALID_HANDLE_VALUE;
    DWORD size = 0;
    GetTokenInformation(token, TokenLogonSid, nullptr, 0, &size);
    std::vector<BYTE> groups(size);
    bool ok = size && GetTokenInformation(token, TokenLogonSid, groups.data(), size, &size);
    CloseHandle(token);
    if (!ok) return INVALID_HANDLE_VALUE;
    auto logon = reinterpret_cast<const TOKEN_GROUPS*>(groups.data());
    if (logon->GroupCount != 1) return INVALID_HANDLE_VALUE;

    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE;
    access.grfAccessMode = SET_ACCESS;
    BuildTrusteeWithSidW(&access.Trustee, logon->Groups[0].Sid);
    PACL acl = nullptr;
    if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) return INVALID_HANDLE_VALUE;
    SECURITY_DESCRIPTOR sd{};
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), &sd, FALSE};
    HANDLE result = INVALID_HANDLE_VALUE;
    if (InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) && SetSecurityDescriptorDacl(&sd, TRUE, acl, FALSE)) {
        const std::wstring name = PipePath();
        result = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                  PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 16384, 8192, 0, &attributes);
    }
    LocalFree(acl);
    return result;
}

void CALLBACK NotifyCompletion(PTP_CALLBACK_INSTANCE, void*, PTP_WAIT, TP_WAIT_RESULT) {
    PostMessageW(hwnd, completionMessage, 0, 0);
}

void Disconnect() {
    DisconnectNamedPipe(pipe);
    operation = Operation::Connect;
    connectedAt = 0;
    request.clear();
    response.clear();
}

void OnCompleted(DWORD transferred) {
    switch (operation) {
        case Operation::Connect:
            connectedAt = GetTickCount64();
            operation = Operation::Read;
            break;
        case Operation::Read: {
            if (!transferred || request.size() + transferred > MaxRequest) {
                Disconnect();
                break;
            }
            request.append(buffer, transferred);
            const auto newline = request.find('\n');
            if (newline == std::string::npos) break;
            if (newline + 1 != request.size()) {
                Disconnect();
                break;
            }
            response = handleRequest(std::string_view(request).substr(0, newline));
            if (response.size() + 1 > MaxResponse) response = R"({"ok":false})";
            response.push_back('\n');
            operation = Operation::Write;
            break;
        }
        case Operation::Write:
            // Wait for the client to read and close. DisconnectNamedPipe discards unread bytes.
            operation = Operation::Drain;
            break;
        case Operation::Drain:
            Disconnect();
            break;
    }
}

void Pump() {
    for (;;) {
        ResetEvent(event);
        io = {};
        io.hEvent = event;
        DWORD count = 0;
        BOOL done;
        if (operation == Operation::Connect)
            done = ConnectNamedPipe(pipe, &io);
        else if (operation == Operation::Write)
            done = WriteFile(pipe, response.data(), static_cast<DWORD>(response.size()), &count, &io);
        else
            done = ReadFile(pipe, buffer, sizeof(buffer), &count, &io);

        const DWORD error = done ? ERROR_SUCCESS : GetLastError();
        if (error == ERROR_IO_PENDING) {
            pending = true;
            SetThreadpoolWait(wait, event, nullptr);
            return;
        }
        if (done || (operation == Operation::Connect && error == ERROR_PIPE_CONNECTED)) {
            OnCompleted(count);
        } else if (operation == Operation::Connect && error != ERROR_NO_DATA) {
            return;
        } else {
            Disconnect();
        }
    }
}

}  // namespace

bool Start(HWND window, UINT message, RequestHandler handler) {
    pipe = CreatePrivatePipe();
    if (pipe == INVALID_HANDLE_VALUE) return false;
    event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    wait = CreateThreadpoolWait(NotifyCompletion, nullptr, nullptr);
    if (!event || !wait) {
        Stop();
        return false;
    }
    hwnd = window;
    completionMessage = message;
    handleRequest = handler;
    Pump();
    return true;
}

void Complete() {
    if (!pending) return;
    DWORD count = 0;
    const bool done = GetOverlappedResult(pipe, &io, &count, FALSE);
    if (!done && GetLastError() == ERROR_IO_INCOMPLETE) {
        SetThreadpoolWait(wait, event, nullptr);
        return;
    }
    pending = false;
    if (done) OnCompleted(count);
    else Disconnect();
    Pump();
}

void ExpireClient() {
    if (pending && connectedAt && GetTickCount64() - connectedAt > 5000) CancelIoEx(pipe, &io);
}

void Stop() {
    if (wait) {
        SetThreadpoolWait(wait, nullptr, nullptr);
        WaitForThreadpoolWaitCallbacks(wait, TRUE);
        CloseThreadpoolWait(wait);
        wait = nullptr;
    }
    if (pending) {
        CancelIoEx(pipe, &io);
        DWORD count = 0;
        GetOverlappedResult(pipe, &io, &count, TRUE);
        pending = false;
    }
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    if (event) CloseHandle(event);
    pipe = INVALID_HANDLE_VALUE;
    event = nullptr;
}

}
