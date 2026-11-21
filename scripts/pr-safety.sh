#!/usr/bin/env bash
# Scans a pull request's diff for things this app should never do, so a reviewer knows exactly which
# lines to read carefully before merging. It reads the diff as text and never runs code from the PR.
#
#   usage: scripts/pr-safety.sh <diff-file>          (exit 42 when findings are reported)
#
# This is a tripwire, not a guarantee: a clean result still needs a human review of the diff.
set -euo pipefail
diff_file="$1"
summary="${GITHUB_STEP_SUMMARY:-}"

# The app only talks to HID devices and the Windows device APIs. Anything below is out of place in
# its source and must be justified in the PR.
findings=$(awk '
function flag(cat, text) {
    gsub(/\t/, " ", text)
    printf "%s\t%d\t%s\t%s\n", file, line, cat, substr(text, 1, 160)
}
/^diff --git / { file = $0; sub(/^.* b\//, "", file); next }
/^\+\+\+ / {
    file = substr($0, 5); sub(/^b\//, "", file)
    if (file ~ /^(\.github\/|installer\/|scripts\/|res\/)/ || file == "Makefile")
        printf "%s\t0\tsensitive file\tchanges the build, release or check pipeline\n", file
    next
}
/^Binary files / || /^GIT binary patch/ { printf "%s\t0\tbinary file\t%s\n", (file == "" ? "?" : file), $0; next }
/^@@ / { match($0, /\+[0-9]+/); line = substr($0, RSTART + 1, RLENGTH - 1) - 1; next }
/^-/ { next }
/^\+/ {
    line++
    text = substr($0, 2)
    if (file == "build.bat") {  # adding a driver to the file list is expected; anything else is not
        rest = text
        gsub(/src\\[A-Za-z0-9_\\]+\.cpp/, "", rest)
        gsub(/[ \t^]/, "", rest)
        if (rest != "") flag("build command changed", text)
        next
    }
    # Any code file can run: the app itself, or tests and scripts during the CI build.
    if (file !~ /\.(c|cc|cpp|cxx|h|hpp|hxx|ps1|psm1|bat|cmd|sh|py|js|ts|cs|rs|go|rc)$/) next
    if (text ~ /(WinHttp|WinInet|InternetOpen|InternetConnect|HttpOpenRequest|HttpSendRequest|URLDownloadTo|URLOpenStream|WSAStartup|getaddrinfo|gethostbyname|[^a-zA-Z_]socket[ \t]*\(|ws2_32|wininet|winhttp|urlmon|https?:\/\/)/ && text !~ /^[ \t]*\/\//)
        flag("network access", text)
    if (text ~ /(CreateProcess|ShellExecute|WinExec|[^a-zA-Z_]system[ \t]*\(|_wsystem|_popen|_wpopen|CreateService|OpenSCManager|IShellDispatch)/)
        flag("runs other programs", text)
    if (text ~ /(VirtualAlloc|VirtualProtect|WriteProcessMemory|ReadProcessMemory|CreateRemoteThread|OpenProcess|NtCreate|ZwCreate|LoadLibrary|GetProcAddress|__asm|[^a-zA-Z_]asm[ \t]*\()/)
        flag("code loading / memory tricks", text)
    if (text ~ /(SetWindowsHookEx|GetAsyncKeyState|GetKeyState|RegisterRawInputDevices|GetRawInputData|GetClipboardData|OpenClipboard|BitBlt|PrintWindow|CopyFromScreen)/)
        flag("reads input, clipboard or screen", text)
    if (text ~ /(RegSetValue|RegSetKeyValue|RegCreateKey|RegDeleteKey|RegDeleteTree|CurrentVersion\\\\Run|schtasks|\\\\Startup)/)
        flag("registry / startup changes", text)
    if (text ~ /(DeleteFile|MoveFile|CopyFile|SHFileOperation|RemoveDirectory|SetFileAttributes|CryptEncrypt|BCryptEncrypt)/)
        flag("deletes, moves or encrypts files", text)
    if (text ~ /["\047][A-Za-z0-9+\/=]{120,}["\047]/ || text ~ /(0x[0-9A-Fa-f]{2}, *){40,}/)
        flag("long encoded blob (possible hidden payload)", text)
}
' "$diff_file")

report() {
    echo "## PR safety check"
    echo
    if [ -z "$findings" ]; then
        echo "No red flags found. This is not a guarantee: still read the diff before merging."
    else
        echo "These lines need a careful look before merging. Some may be fine (for example an"
        echo "existing call moved around), but each one must be explained by the PR."
        echo
        echo "| File | Line | Why | Code |"
        echo "|---|---|---|---|"
        printf '%s\n' "$findings" | while IFS=$'\t' read -r f l c t; do
            t="${t//|/\\|}"
            printf '| `%s` | %s | %s | `%s` |\n' "$f" "$l" "$c" "$t"
            if [ -n "${GITHUB_ACTIONS:-}" ]; then echo "::warning file=$f,line=$l::$c" >&2; fi
        done
    fi
}

if [ -n "$summary" ]; then report >> "$summary"; else report; fi
if [ -n "$findings" ]; then exit 42; fi
