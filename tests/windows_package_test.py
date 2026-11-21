"""Exercise the actual Windows release assets. Use a disposable account or runner."""

from ctypes import byref, create_unicode_buffer, windll, WINFUNCTYPE, wintypes as wint
from hashlib import file_digest
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import winreg
from zipfile import ZipFile


BACKGROUND = "WirelessBatteryCenter.exe"
PANEL = "WirelessBatteryCenterPanel.exe"
BACKGROUND_CLASS = "WirelessBatteryCenterWnd"
PANEL_PROPERTY = "WirelessBatteryCenter.Panel.MainWindow"
RUN_KEY = r"Software\Microsoft\Windows\CurrentVersion\Run"
UNINSTALL_KEY = r"Software\Microsoft\Windows\CurrentVersion\Uninstall\{29FA890E-D56F-4B01-8D9F-CB7B03DE1524}_is1"
DEMO_PROFILES = r"Software\WirelessBatteryCenter\Demo\DeviceProfiles"
LEGACY_PROFILES = r"Software\WirelessBattery\DeviceProfiles"
WINDOW_CALLBACK = WINFUNCTYPE(wint.BOOL, wint.HWND, wint.LPARAM)

user32 = windll.user32
kernel32 = windll.kernel32
user32.FindWindowW.argtypes = (wint.LPCWSTR, wint.LPCWSTR)
user32.FindWindowW.restype = wint.HWND
user32.GetPropW.argtypes = (wint.HWND, wint.LPCWSTR)
user32.GetPropW.restype = wint.HANDLE
user32.GetWindowThreadProcessId.argtypes = (wint.HWND, wint.LPDWORD)
user32.PostMessageW.argtypes = (wint.HWND, wint.UINT, wint.WPARAM, wint.LPARAM)
user32.GetWindowTextW.argtypes = (wint.HWND, wint.LPWSTR, wint.INT)
kernel32.CreateMutexW.argtypes = (wint.LPVOID, wint.BOOL, wint.LPCWSTR)
kernel32.CreateMutexW.restype = wint.HANDLE
kernel32.CloseHandle.argtypes = (wint.HANDLE,)
kernel32.OpenProcess.argtypes = (wint.DWORD, wint.BOOL, wint.DWORD)
kernel32.OpenProcess.restype = wint.HANDLE
kernel32.QueryFullProcessImageNameW.argtypes = (wint.HANDLE, wint.DWORD, wint.LPWSTR, wint.LPDWORD)

session = wint.DWORD()
assert kernel32.ProcessIdToSessionId(os.getpid(), byref(session))
PIPE = rf"\\.\pipe\WirelessBatteryCenter.Panel.v1.{session.value}"


def registry(path, name):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
            return winreg.QueryValueEx(key, name)[0]
    except FileNotFoundError:
        return None


def registry_values(path):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path) as key:
            return sorted(winreg.EnumValue(key, i) for i in range(winreg.QueryInfoKey(key)[1]))
    except FileNotFoundError:
        return []


def wait_for(predicate, label, timeout=30):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(0.15)
    raise AssertionError(f"Timed out waiting for {label}")


def request(command="snapshot"):
    try:
        with open(PIPE, "r+b", buffering=0) as pipe:
            pipe.write((command + "\n").encode("ascii"))
            return json.loads(pipe.readline())
    except OSError:
        return None


def snapshot():
    state = request()
    return state if state and state["ok"] and state["poll"] > 0 else None


def find_panel(pid):
    matches = []

    @WINDOW_CALLBACK
    def visit(window, _):
        owner = wint.DWORD()
        user32.GetWindowThreadProcessId(window, byref(owner))
        if owner.value == pid and user32.GetPropW(window, PANEL_PROPERTY):
            matches.append(window)
        return True

    user32.EnumWindows(visit, 0)
    return matches[0] if matches else None


def background_path():
    hwnd = user32.FindWindowW(BACKGROUND_CLASS, None)
    if not hwnd:
        return None
    pid = wint.DWORD()
    user32.GetWindowThreadProcessId(hwnd, byref(pid))
    handle = kernel32.OpenProcess(0x1000, False, pid.value)
    assert handle, "Cannot query the background process image"
    try:
        path = create_unicode_buffer(32768)
        size = wint.DWORD(len(path))
        assert kernel32.QueryFullProcessImageNameW(handle, 0, path, byref(size))
        return Path(path.value).resolve()
    finally:
        kernel32.CloseHandle(handle)


def app_env():
    values = {name: value for name, value in os.environ.items() if name.upper() not in {
        "PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML_IMPORT_PATH", "QML2_IMPORT_PATH"
    }}
    windows = Path(os.environ["SystemRoot"])
    values["PATH"] = os.pathsep.join(map(str, (windows / "System32", windows)))
    values["QT_FORCE_STDERR_LOGGING"] = "1"
    return values


def launch_panel(folder, log):
    with log.open("wb") as stream:
        panel = subprocess.Popen([folder / PANEL], cwd=folder, env=app_env(), stdout=stream, stderr=stream)
    hwnd = wait_for(lambda: find_panel(panel.pid), f"native panel window (see {log})")
    title = create_unicode_buffer(256)
    user32.GetWindowTextW(hwnd, title, len(title))
    assert title.value == "Wireless Battery Center", title.value
    assert wait_for(background_path, "background") == folder / BACKGROUND
    return panel, hwnd


def quit_app(folder):
    exe = folder / BACKGROUND
    if exe.is_file() and background_path() == exe:
        subprocess.run([exe, "--quit"], check=True, timeout=20, cwd=folder)
        wait_for(lambda: not background_path(), "background shutdown")


def setup(exe, folder, log, tasks, default_dir=False):
    directory_args = [] if default_dir else [f"/DIR={folder}"]
    subprocess.run([exe, "/CURRENTUSER", "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                    *directory_args, f"/TASKS={tasks}", f"/LOG={log}"], check=True, timeout=180)
    assert registry(UNINSTALL_KEY, "DisplayName") == "Wireless Battery Center"
    assert registry(UNINSTALL_KEY, "InstallLocation").rstrip("\\") == str(folder)


def uninstall(folder, log):
    exe = folder / "unins000.exe"
    subprocess.run([exe, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", f"/LOG={log}"],
                   check=True, timeout=180)
    # Inno's copied uninstaller continues after the launcher exits.
    wait_for(lambda: not exe.exists() and not (folder / BACKGROUND).exists() and
             registry(UNINSTALL_KEY, "DisplayName") is None, "clean uninstall")


def digest(path):
    with path.open("rb") as file:
        return file_digest(file, "sha256").hexdigest()


def start_menu_shortcut():
    path = create_unicode_buffer(260)
    assert windll.shell32.SHGetFolderPathW(None, 0x0002, None, 0, path) == 0
    return Path(path.value) / "Wireless Battery Center.lnk"


def shortcut_target(path):
    powershell = Path(os.environ["SystemRoot"]) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    output = subprocess.check_output([powershell, "-NoProfile", "-Command",
        "[Console]::OutputEncoding = [Text.UTF8Encoding]::new(); "
        "(New-Object -ComObject WScript.Shell).CreateShortcut($env:WBC_TEST_SHORTCUT).TargetPath"],
        env={**os.environ, "WBC_TEST_SHORTCUT": str(path)}, encoding="utf-8", timeout=15)
    return Path(output.strip()).resolve()


def run(artifacts, qa):
    assert not background_path() and registry(UNINSTALL_KEY, "DisplayName") is None, "Center is already running or installed"
    assert registry(RUN_KEY, "WirelessBatteryCenter") is None, "Existing Center startup configuration"
    assert not user32.FindWindowW("WirelessBatteryWnd", None), "Run the test in an account without a live legacy poller"
    default_install = (Path(os.environ["LOCALAPPDATA"]) / "Programs/Wireless Battery Center").resolve()
    assert not default_install.exists(), "Existing Center default installation directory"
    qa.mkdir(parents=True)
    (installer,) = artifacts.glob("*-setup.exe")
    (archive,) = artifacts.glob("*-portable.zip")
    hashes = dict(line.split("  ", maxsplit=1)[::-1] for line in
                  (artifacts / "SHA256SUMS.txt").read_text(encoding="ascii").splitlines())
    assert set(hashes) == {installer.name, archive.name, "headsetcontrol-4.3.0.tar.gz"}
    assert all(digest(artifacts / name) == expected for name, expected in hashes.items())
    portable, installed = qa / "portable", qa / "installed app"
    with ZipFile(archive) as package:
        expected_files = [name for name in package.namelist() if not name.endswith("/")]
        package.extractall(portable)
    assert set(expected_files).issuperset({BACKGROUND, PANEL, "headsetcontrol.exe", "Qt6Quick.dll",
        "platforms/qwindows.dll", "qml/QtQuick/Dialogs/qmldir", "LICENSE.txt", "LUCIDE-LICENSE.txt",
        "THIRD_PARTY_NOTICES.txt", "licenses/Qt/LGPL-3.0-only.txt"})

    legacy_startup = registry(RUN_KEY, "WirelessBattery")
    legacy_profiles = registry_values(LEGACY_PROFILES)
    legacy_mutexes = [kernel32.CreateMutexW(None, False, f"Local\\{name}") for name in
                      ("WirelessBatteryTray", "WirelessBatteryPanel")]
    assert all(legacy_mutexes)
    try:
        legacy_folder = qa / "legacy install"
        legacy_folder.mkdir()
        marker = legacy_folder / "WirelessBattery.exe"
        marker.write_bytes(b"legacy installation marker")
        refused = subprocess.run([installer, "/CURRENTUSER", "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                                  f"/DIR={legacy_folder}", f"/LOG={qa / 'legacy-folder.log'}"], timeout=180)
        assert refused.returncode != 0 and marker.read_bytes() == b"legacy installation marker"
        assert registry(UNINSTALL_KEY, "DisplayName") is None

        setup(installer, installed, qa / "install.log", "autostart")
        assert registry(RUN_KEY, "WirelessBatteryCenter") == f'"{installed / BACKGROUND}"'
        assert registry(RUN_KEY, "WirelessBattery") == legacy_startup
        shortcut = start_menu_shortcut()
        assert shortcut.is_file() and shortcut_target(shortcut) == installed / PANEL
        assert all(digest(installed / file) == digest(portable / file) for file in expected_files)
        print(f"Install and ZIP contain the same {len(expected_files)} files. SHA-256 verified.", flush=True)

        panel, hwnd = launch_panel(installed, qa / "installed-panel.log")
        wait_for(snapshot, "initial installed IPC snapshot")
        refresh = wait_for(lambda: request("refresh"), "refresh acknowledgement")
        assert refresh["ok"]
        wait_for(lambda: (state := snapshot()) and state["refreshCompleted"] >= refresh["refreshRequested"],
                 "background refresh completion")
        assert user32.PostMessageW(hwnd, 0x0010, 0, 0)
        assert panel.wait(timeout=15) == 0
        assert background_path() == installed / BACKGROUND and snapshot()
        quit_app(installed)

        subprocess.Popen([installed / BACKGROUND, "--demo"], cwd=installed, env=app_env())
        wait_for(background_path, "demo background")
        panel, _ = launch_panel(installed, qa / "demo-before-uninstall.log")
        state = wait_for(snapshot, "demo archive")
        assert state["demo"]
        device = next(item for item in state["devices"] if item["key"] == "razer:demo")
        persisted = registry(DEMO_PROFILES, "razer:demo")
        assert persisted and device["firstOnline"] > 0
        uninstall(installed, qa / "uninstall.log")
        assert panel.wait(timeout=15) == 0 and not background_path()
        assert not shortcut.exists()
        assert registry(RUN_KEY, "WirelessBatteryCenter") is None
        assert registry(RUN_KEY, "WirelessBattery") == legacy_startup
        assert registry(DEMO_PROFILES, "razer:demo")
        assert registry_values(LEGACY_PROFILES) == legacy_profiles

        setup(installer, default_install, qa / "reinstall.log", "", default_dir=True)
        assert registry(RUN_KEY, "WirelessBatteryCenter") is None
        assert shortcut_target(start_menu_shortcut()) == default_install / PANEL
        subprocess.Popen([default_install / BACKGROUND, "--demo"], cwd=default_install, env=app_env())
        restarted = wait_for(snapshot, "reinstalled demo snapshot")
        assert restarted["demo"]
        loaded = next(item for item in restarted["devices"] if item["key"] == "razer:demo")
        assert loaded["firstOnline"] == device["firstOnline"]
        quit_app(default_install)
        uninstall(default_install, qa / "final-uninstall.log")

        panel, _ = launch_panel(portable, qa / "portable-panel.log")
        wait_for(snapshot, "portable IPC snapshot")
        quit_app(portable)
        assert panel.wait(timeout=15) == 0
        print("First launch, IPC refresh, independent mutexes, uninstall/reinstall, archive retention and portable launch passed.", flush=True)
    finally:
        quit_app(portable)
        for folder in (installed, default_install):
            quit_app(folder)
            registered = registry(UNINSTALL_KEY, "InstallLocation")
            if registered and Path(registered).resolve() == folder and (folder / "unins000.exe").exists():
                uninstall(folder, qa / "cleanup-uninstall.log")
        for handle in legacy_mutexes:
            kernel32.CloseHandle(handle)


if __name__ == "__main__":
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
