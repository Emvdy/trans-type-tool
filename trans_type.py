#!/usr/bin/env python3
import argparse
import ctypes
import locale
import os
import sys
import time
from dataclasses import dataclass
from pathlib import Path

EXIT_OK = 0
EXIT_ARGS = 2
EXIT_FILE = 3
EXIT_ENCODING = 4
EXIT_CONTENT = 5
EXIT_ABORTED = 6
EXIT_INPUT = 7

DEFAULT_DELAY_MS = 20
DEFAULT_LINE_DELAY_MS = 100
DEFAULT_START_DELAY_SEC = 5
DEFAULT_MAX_BYTES = 1024 * 1024
ABSOLUTE_MAX_BYTES = 100 * 1024 * 1024

INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004

VK_TAB = 0x09
VK_RETURN = 0x0D
VK_SHIFT = 0x10
VK_CONTROL = 0x11
VK_MENU = 0x12
VK_PAUSE = 0x13
VK_ESCAPE = 0x1B

TOKEN_QUERY = 0x0008
TOKEN_INTEGRITY_LEVEL = 25
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000


@dataclass
class Options:
    delay_ms: int
    line_delay_ms: int
    start_delay_sec: int
    max_bytes: int
    ascii_only: bool
    ascii_keys: bool
    no_focus_check: bool
    dry_run: bool
    diagnose: bool
    self_test: bool
    debug_input: bool
    enter_mode: str


@dataclass
class TextData:
    path: Path
    text: str
    byte_count: int
    encoding: str


@dataclass
class TextStats:
    lines: int
    non_ascii_count: int
    control_count: int
    surrogate_error_count: int
    first_non_ascii: int
    first_control: int
    first_surrogate_error: int


@dataclass
class TargetWindow:
    hwnd: int
    pid: int
    title: str


@dataclass
class IntegrityInfo:
    rid: int | None
    label: str
    error: str | None = None


def bounded_int(name: str, minimum: int, maximum: int):
    def parse(value: str) -> int:
        try:
            parsed = int(value, 10)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(f"{name} must be an integer") from exc
        if parsed < minimum or parsed > maximum:
            raise argparse.ArgumentTypeError(f"{name} must be between {minimum} and {maximum}")
        return parsed

    return parse


def parse_args(argv: list[str]) -> Options:
    parser = argparse.ArgumentParser(
        prog="trans_type_py.exe",
        description="Type trans.txt into the current foreground Windows window through simulated keyboard input.",
    )
    parser.add_argument("--delay-ms", type=bounded_int("--delay-ms", 0, 5000), default=DEFAULT_DELAY_MS)
    parser.add_argument("--line-delay-ms", type=bounded_int("--line-delay-ms", 0, 5000), default=DEFAULT_LINE_DELAY_MS)
    parser.add_argument("--start-delay-sec", type=bounded_int("--start-delay-sec", 0, 3600), default=DEFAULT_START_DELAY_SEC)
    parser.add_argument("--max-bytes", type=bounded_int("--max-bytes", 1, ABSOLUTE_MAX_BYTES), default=DEFAULT_MAX_BYTES)
    parser.add_argument("--ascii-only", action="store_true")
    parser.add_argument("--ascii-keys", action="store_true")
    parser.add_argument("--no-focus-check", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--diagnose", action="store_true", help="Report the focused target window and integrity levels without typing")
    parser.add_argument("--self-test", action="store_true", help="Test whether SendInput can inject a harmless Shift key event")
    parser.add_argument("--debug-input", action="store_true", help="Run visible SendInput diagnostics against the focused target window")
    parser.add_argument("--enter-mode", choices=("key", "unicode"), default="key")
    args = parser.parse_args(argv)
    return Options(
        delay_ms=args.delay_ms,
        line_delay_ms=args.line_delay_ms,
        start_delay_sec=args.start_delay_sec,
        max_bytes=args.max_bytes,
        ascii_only=args.ascii_only,
        ascii_keys=args.ascii_keys,
        no_focus_check=args.no_focus_check,
        dry_run=args.dry_run,
        diagnose=args.diagnose,
        self_test=args.self_test,
        debug_input=args.debug_input,
        enter_mode=args.enter_mode,
    )


def app_dir() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent
    return Path(__file__).resolve().parent


def trans_path() -> Path:
    return app_dir() / "trans.txt"


def decode_bytes(raw: bytes) -> tuple[str, str]:
    if raw.startswith(b"\xef\xbb\xbf"):
        return raw[3:].decode("utf-8", errors="strict"), "UTF-8 BOM"
    if raw.startswith(b"\xff\xfe"):
        return raw[2:].decode("utf-16-le", errors="strict"), "UTF-16 LE BOM"
    if raw.startswith(b"\xfe\xff"):
        return raw[2:].decode("utf-16-be", errors="strict"), "UTF-16 BE BOM"

    try:
        return raw.decode("utf-8", errors="strict"), "UTF-8"
    except UnicodeDecodeError:
        pass

    fallback = "mbcs" if os.name == "nt" else (locale.getpreferredencoding(False) or "cp1252")
    return raw.decode(fallback, errors="strict"), f"{fallback} fallback"


def read_trans_file(path: Path, opt: Options) -> TextData:
    try:
        stat = path.stat()
    except OSError as exc:
        raise RuntimeError(f"Cannot open trans.txt: {path} ({exc})") from exc

    if stat.st_size == 0:
        raise RuntimeError("trans.txt is empty.")
    if stat.st_size > opt.max_bytes:
        raise RuntimeError(
            f"trans.txt is too large: {stat.st_size} bytes. Limit: {opt.max_bytes} bytes. "
            "Use --max-bytes to override intentionally."
        )

    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise RuntimeError(f"Cannot read trans.txt: {path} ({exc})") from exc

    try:
        text, encoding = decode_bytes(raw)
    except UnicodeDecodeError as exc:
        raise ValueError(f"trans.txt could not be decoded as UTF-8 or the fallback ANSI code page ({exc})") from exc

    if not text:
        raise RuntimeError("trans.txt decoded to empty text.")

    return TextData(path=path, text=text, byte_count=len(raw), encoding=encoding)


def analyze_text(text: str) -> TextStats:
    lines = 1
    non_ascii_count = 0
    control_count = 0
    surrogate_error_count = 0
    first_non_ascii = -1
    first_control = -1
    first_surrogate_error = -1
    i = 0

    while i < len(text):
        code = ord(text[i])
        if text[i] == "\r":
            lines += 1
            if i + 1 < len(text) and text[i + 1] == "\n":
                i += 2
                continue
            i += 1
            continue
        if text[i] == "\n":
            lines += 1
            i += 1
            continue

        if code > 0x7F:
            non_ascii_count += 1
            if first_non_ascii < 0:
                first_non_ascii = i
        if (code < 32 and text[i] != "\t") or code == 127:
            control_count += 1
            if first_control < 0:
                first_control = i
        if 0xD800 <= code <= 0xDFFF:
            surrogate_error_count += 1
            if first_surrogate_error < 0:
                first_surrogate_error = i
        i += 1

    return TextStats(
        lines=lines,
        non_ascii_count=non_ascii_count,
        control_count=control_count,
        surrogate_error_count=surrogate_error_count,
        first_non_ascii=first_non_ascii,
        first_control=first_control,
        first_surrogate_error=first_surrogate_error,
    )


def line_col(text: str, index: int) -> tuple[int, int]:
    line = 1
    col = 1
    i = 0
    stop = min(index, len(text))
    while i < stop:
        if text[i] == "\r":
            line += 1
            col = 1
            if i + 1 < stop and text[i + 1] == "\n":
                i += 2
                continue
        elif text[i] == "\n":
            line += 1
            col = 1
        else:
            col += 1
        i += 1
    return line, col


def validate_text(data: TextData, stats: TextStats, opt: Options) -> None:
    if stats.control_count:
        line, col = line_col(data.text, stats.first_control)
        raise RuntimeError(
            f"trans.txt contains unsupported control characters. First one at line {line}, column {col}. "
            "Allowed control characters are tab and newlines only."
        )
    if stats.surrogate_error_count:
        line, col = line_col(data.text, stats.first_surrogate_error)
        raise RuntimeError(f"trans.txt contains invalid surrogate data. First issue at line {line}, column {col}.")
    if opt.ascii_only and stats.non_ascii_count:
        line, col = line_col(data.text, stats.first_non_ascii)
        raise RuntimeError(
            f"--ascii-only is enabled, but trans.txt has non-ASCII characters. "
            f"First one at line {line}, column {col}."
        )
    if opt.ascii_keys and stats.non_ascii_count:
        line, col = line_col(data.text, stats.first_non_ascii)
        raise RuntimeError(
            f"--ascii-keys cannot type non-ASCII characters. First one at line {line}, column {col}."
        )


def print_summary(data: TextData, stats: TextStats, opt: Options) -> None:
    print(f"File: {data.path}")
    print(f"Encoding: {data.encoding}")
    print(f"Bytes: {data.byte_count}")
    print(f"Characters: {len(data.text)}")
    print(f"Lines: {stats.lines}")
    print(f"Non-ASCII characters: {stats.non_ascii_count}")
    print(f"Delay: {opt.delay_ms} ms per character, {opt.line_delay_ms} ms per line")
    print(f"Focus check: {'off' if opt.no_focus_check else 'on'}")
    print(f"Input mode: {'ASCII virtual keys' if opt.ascii_keys else 'Unicode SendInput'}")
    print(f"Enter mode: {'Unicode CR' if opt.enter_mode == 'unicode' else 'VK_RETURN'}")


class WinKeyboard:
    def __init__(self) -> None:
        if os.name != "nt":
            raise RuntimeError("Keyboard input is only supported on Windows.")

        from ctypes import wintypes

        self.wintypes = wintypes
        self.user32 = ctypes.WinDLL("user32", use_last_error=True)
        self.kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.advapi32 = ctypes.WinDLL("advapi32", use_last_error=True)

        class MOUSEINPUT(ctypes.Structure):
            _fields_ = [
                ("dx", wintypes.LONG),
                ("dy", wintypes.LONG),
                ("mouseData", wintypes.DWORD),
                ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t),
            ]

        class KEYBDINPUT(ctypes.Structure):
            _fields_ = [
                ("wVk", wintypes.WORD),
                ("wScan", wintypes.WORD),
                ("dwFlags", wintypes.DWORD),
                ("time", wintypes.DWORD),
                ("dwExtraInfo", ctypes.c_size_t),
            ]

        class HARDWAREINPUT(ctypes.Structure):
            _fields_ = [
                ("uMsg", wintypes.DWORD),
                ("wParamL", wintypes.WORD),
                ("wParamH", wintypes.WORD),
            ]

        class INPUT_UNION(ctypes.Union):
            _fields_ = [
                ("mi", MOUSEINPUT),
                ("ki", KEYBDINPUT),
                ("hi", HARDWAREINPUT),
            ]

        class INPUT(ctypes.Structure):
            _anonymous_ = ("u",)
            _fields_ = [("type", wintypes.DWORD), ("u", INPUT_UNION)]

        class SID_AND_ATTRIBUTES(ctypes.Structure):
            _fields_ = [
                ("Sid", wintypes.LPVOID),
                ("Attributes", wintypes.DWORD),
            ]

        class TOKEN_MANDATORY_LABEL(ctypes.Structure):
            _fields_ = [("Label", SID_AND_ATTRIBUTES)]

        self.KEYBDINPUT = KEYBDINPUT
        self.INPUT = INPUT
        self.TOKEN_MANDATORY_LABEL = TOKEN_MANDATORY_LABEL

        self.user32.SendInput.argtypes = [wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int]
        self.user32.SendInput.restype = wintypes.UINT
        self.user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
        self.user32.GetAsyncKeyState.restype = ctypes.c_short
        self.user32.GetForegroundWindow.argtypes = []
        self.user32.GetForegroundWindow.restype = wintypes.HWND
        self.user32.GetWindowThreadProcessId.argtypes = [wintypes.HWND, ctypes.POINTER(wintypes.DWORD)]
        self.user32.GetWindowThreadProcessId.restype = wintypes.DWORD
        self.user32.GetWindowTextW.argtypes = [wintypes.HWND, wintypes.LPWSTR, ctypes.c_int]
        self.user32.GetWindowTextW.restype = ctypes.c_int
        self.user32.VkKeyScanW.argtypes = [wintypes.WCHAR]
        self.user32.VkKeyScanW.restype = ctypes.c_short
        self.kernel32.GetCurrentProcessId.argtypes = []
        self.kernel32.GetCurrentProcessId.restype = wintypes.DWORD
        self.kernel32.GetCurrentProcess.argtypes = []
        self.kernel32.GetCurrentProcess.restype = wintypes.HANDLE
        self.kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        self.kernel32.OpenProcess.restype = wintypes.HANDLE
        self.kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.kernel32.CloseHandle.restype = wintypes.BOOL
        self.advapi32.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
        self.advapi32.OpenProcessToken.restype = wintypes.BOOL
        self.advapi32.GetTokenInformation.argtypes = [
            wintypes.HANDLE,
            ctypes.c_int,
            wintypes.LPVOID,
            wintypes.DWORD,
            ctypes.POINTER(wintypes.DWORD),
        ]
        self.advapi32.GetTokenInformation.restype = wintypes.BOOL
        self.advapi32.GetSidSubAuthorityCount.argtypes = [wintypes.LPVOID]
        self.advapi32.GetSidSubAuthorityCount.restype = ctypes.POINTER(ctypes.c_ubyte)
        self.advapi32.GetSidSubAuthority.argtypes = [wintypes.LPVOID, wintypes.DWORD]
        self.advapi32.GetSidSubAuthority.restype = ctypes.POINTER(wintypes.DWORD)

    def key_input(self, vk: int = 0, scan: int = 0, flags: int = 0):
        item = self.INPUT()
        item.type = INPUT_KEYBOARD
        item.ki.wVk = vk
        item.ki.wScan = scan
        item.ki.dwFlags = flags
        item.ki.time = 0
        item.ki.dwExtraInfo = 0
        return item

    def send_inputs(self, inputs: list, what: str) -> None:
        array_type = self.INPUT * len(inputs)
        array = array_type(*inputs)
        sent = self.user32.SendInput(len(inputs), array, ctypes.sizeof(self.INPUT))
        if sent != len(inputs):
            err = ctypes.get_last_error()
            if err:
                raise RuntimeError(f"SendInput failed while typing {what}. Sent {sent}/{len(inputs)} events. Windows error {err}.")
            raise RuntimeError(
                f"SendInput failed while typing {what}. Sent {sent}/{len(inputs)} events. "
                "Windows likely blocked the input because the target window has a higher integrity level. "
                "To run without Administrator, close the target window and reopen it normally, not with Run as administrator. "
                "Otherwise run this tool as Administrator too."
            )

    def send_vk(self, vk: int, what: str) -> None:
        self.send_inputs(
            [
                self.key_input(vk=vk),
                self.key_input(vk=vk, flags=KEYEVENTF_KEYUP),
            ],
            what,
        )

    def send_unicode_unit(self, unit: int, what: str) -> None:
        self.send_inputs(
            [
                self.key_input(scan=unit, flags=KEYEVENTF_UNICODE),
                self.key_input(scan=unit, flags=KEYEVENTF_UNICODE | KEYEVENTF_KEYUP),
            ],
            what,
        )

    def send_unicode_char(self, ch: str) -> None:
        encoded = ch.encode("utf-16-le", errors="strict")
        for offset in range(0, len(encoded), 2):
            unit = encoded[offset] | (encoded[offset + 1] << 8)
            self.send_unicode_unit(unit, "Unicode character")

    def send_ascii_char(self, ch: str) -> None:
        vk_scan = self.user32.VkKeyScanW(ch)
        if vk_scan == -1:
            raise RuntimeError(f"Cannot map ASCII character U+{ord(ch):04X} through the current keyboard layout.")

        vk = vk_scan & 0xFF
        shift_state = (vk_scan >> 8) & 0xFF
        inputs = []

        if shift_state & 1:
            inputs.append(self.key_input(vk=VK_SHIFT))
        if shift_state & 2:
            inputs.append(self.key_input(vk=VK_CONTROL))
        if shift_state & 4:
            inputs.append(self.key_input(vk=VK_MENU))

        inputs.append(self.key_input(vk=vk))
        inputs.append(self.key_input(vk=vk, flags=KEYEVENTF_KEYUP))

        if shift_state & 4:
            inputs.append(self.key_input(vk=VK_MENU, flags=KEYEVENTF_KEYUP))
        if shift_state & 2:
            inputs.append(self.key_input(vk=VK_CONTROL, flags=KEYEVENTF_KEYUP))
        if shift_state & 1:
            inputs.append(self.key_input(vk=VK_SHIFT, flags=KEYEVENTF_KEYUP))

        self.send_inputs(inputs, "ASCII character")

    def get_async_key_state(self, vk: int) -> int:
        return self.user32.GetAsyncKeyState(vk)

    def foreground_window(self) -> TargetWindow:
        hwnd = self.user32.GetForegroundWindow()
        if not hwnd:
            return TargetWindow(hwnd=0, pid=0, title="")
        pid = self.wintypes.DWORD(0)
        self.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        buffer = ctypes.create_unicode_buffer(512)
        self.user32.GetWindowTextW(hwnd, buffer, len(buffer))
        return TargetWindow(hwnd=int(hwnd), pid=int(pid.value), title=buffer.value)

    def current_pid(self) -> int:
        return int(self.kernel32.GetCurrentProcessId())

    @staticmethod
    def integrity_label(rid: int | None) -> str:
        if rid is None:
            return "unknown"
        if rid < 0x1000:
            return "untrusted"
        if rid < 0x2000:
            return "low"
        if rid < 0x3000:
            return "medium"
        if rid < 0x4000:
            return "high / administrator"
        if rid < 0x5000:
            return "system"
        return "protected"

    def token_integrity(self, token) -> IntegrityInfo:
        needed = self.wintypes.DWORD(0)
        self.advapi32.GetTokenInformation(token, TOKEN_INTEGRITY_LEVEL, None, 0, ctypes.byref(needed))
        if needed.value == 0:
            return IntegrityInfo(None, "unknown", f"GetTokenInformation size failed: {ctypes.get_last_error()}")

        buffer = ctypes.create_string_buffer(needed.value)
        ok = self.advapi32.GetTokenInformation(
            token,
            TOKEN_INTEGRITY_LEVEL,
            buffer,
            needed.value,
            ctypes.byref(needed),
        )
        if not ok:
            return IntegrityInfo(None, "unknown", f"GetTokenInformation failed: {ctypes.get_last_error()}")

        label = ctypes.cast(buffer, ctypes.POINTER(self.TOKEN_MANDATORY_LABEL)).contents
        sid = label.Label.Sid
        count_ptr = self.advapi32.GetSidSubAuthorityCount(sid)
        if not count_ptr:
            return IntegrityInfo(None, "unknown", f"GetSidSubAuthorityCount failed: {ctypes.get_last_error()}")
        count = count_ptr.contents.value
        rid_ptr = self.advapi32.GetSidSubAuthority(sid, count - 1)
        if not rid_ptr:
            return IntegrityInfo(None, "unknown", f"GetSidSubAuthority failed: {ctypes.get_last_error()}")
        rid = int(rid_ptr.contents.value)
        return IntegrityInfo(rid, self.integrity_label(rid))

    def current_integrity(self) -> IntegrityInfo:
        token = self.wintypes.HANDLE()
        if not self.advapi32.OpenProcessToken(self.kernel32.GetCurrentProcess(), TOKEN_QUERY, ctypes.byref(token)):
            return IntegrityInfo(None, "unknown", f"OpenProcessToken failed: {ctypes.get_last_error()}")
        try:
            return self.token_integrity(token)
        finally:
            self.kernel32.CloseHandle(token)

    def process_integrity(self, pid: int) -> IntegrityInfo:
        process = self.kernel32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not process:
            return IntegrityInfo(None, "unknown", f"OpenProcess failed: {ctypes.get_last_error()}")
        try:
            token = self.wintypes.HANDLE()
            if not self.advapi32.OpenProcessToken(process, TOKEN_QUERY, ctypes.byref(token)):
                return IntegrityInfo(None, "unknown", f"OpenProcessToken failed: {ctypes.get_last_error()}")
            try:
                return self.token_integrity(token)
            finally:
                self.kernel32.CloseHandle(token)
        finally:
            self.kernel32.CloseHandle(process)


def wait_for_console_enter_or_esc(message: str) -> int:
    import msvcrt

    print(message)
    print("Press Enter in this console to start a new countdown, or Esc to abort.")
    while True:
        ch = msvcrt.getwch()
        if ch == "\x1b":
            return EXIT_ABORTED
        if ch in ("\r", "\n"):
            return EXIT_OK


def countdown(win: WinKeyboard, seconds: int) -> int:
    if seconds <= 0:
        return EXIT_OK

    print()
    print("Focus the target window now.")
    print("Esc aborts. Press Enter during the countdown to start immediately.")
    win.get_async_key_state(VK_RETURN)
    win.get_async_key_state(VK_ESCAPE)
    while win.get_async_key_state(VK_RETURN) & 0x8000:
        time.sleep(0.02)
    win.get_async_key_state(VK_RETURN)
    win.get_async_key_state(VK_ESCAPE)

    for remaining in range(seconds, 0, -1):
        print(f"\rStarting in {remaining} second(s)... ", end="", flush=True)
        for _ in range(10):
            if win.get_async_key_state(VK_ESCAPE) & 0x8000:
                print()
                return EXIT_ABORTED
            if win.get_async_key_state(VK_RETURN) & 0x8000:
                print()
                return EXIT_OK
            time.sleep(0.1)

    print("\rStarting now.               ")
    return EXIT_OK


def print_target(target: TargetWindow) -> None:
    if not target.hwnd:
        print("Target window: <none>")
    elif target.title:
        print(f'Target window: "{target.title}" (pid {target.pid})')
    else:
        print(f"Target window: <untitled> (pid {target.pid})")


def print_integrity_report(win: WinKeyboard, target: TargetWindow) -> None:
    tool = win.current_integrity()
    target_info = win.process_integrity(target.pid)
    print(f"Tool integrity: {tool.label}")
    print(f"Target integrity: {target_info.label}")

    if tool.error:
        print(f"Tool integrity detail: {tool.error}")
    if target_info.error:
        print(f"Target integrity detail: {target_info.error}")

    if tool.rid is not None and target_info.rid is not None and target_info.rid > tool.rid:
        print()
        print("Warning: the target window has a higher integrity level than this tool.")
        print("Windows blocks SendInput from lower-integrity processes to higher-integrity windows.")
        print("No-admin fix: close the target window and reopen it normally, not with Run as administrator.")
        print("For RDP, start mstsc from Win+R or a normal PowerShell/CMD, then run this tool normally.")


def prepare_target_window(win: WinKeyboard, opt: Options) -> tuple[int, TargetWindow | None]:
    self_pid = win.current_pid()
    while True:
        rc = countdown(win, opt.start_delay_sec)
        if rc != EXIT_OK:
            return rc, None

        target = win.foreground_window()
        print_target(target)

        if not target.hwnd:
            rc = wait_for_console_enter_or_esc("No foreground window was detected.")
            if rc != EXIT_OK:
                return rc, None
            continue

        if target.pid == self_pid:
            rc = wait_for_console_enter_or_esc("The tool console is still the foreground window. It will not type into itself.")
            if rc != EXIT_OK:
                return rc, None
            continue

        print_integrity_report(win, target)
        return EXIT_OK, target


def pause_and_recapture(win: WinKeyboard, opt: Options, reason: str) -> tuple[int, TargetWindow | None]:
    print(f"\nPaused: {reason}")
    rc = wait_for_console_enter_or_esc("Typing is paused.")
    if rc != EXIT_OK:
        return rc, None
    return prepare_target_window(win, opt)


def check_runtime_controls(win: WinKeyboard, opt: Options, target: TargetWindow) -> tuple[int, TargetWindow | None]:
    if win.get_async_key_state(VK_ESCAPE) & 0x8000:
        return EXIT_ABORTED, target

    if win.get_async_key_state(VK_PAUSE) & 0x0001:
        return pause_and_recapture(win, opt, "Pause/Break was pressed.")

    if not opt.no_focus_check:
        current = win.foreground_window()
        if current.hwnd != target.hwnd:
            return pause_and_recapture(win, opt, "Foreground window changed.")

    return EXIT_OK, target


def sleep_ms(ms: int) -> None:
    if ms > 0:
        time.sleep(ms / 1000.0)


def self_test_sendinput(win: WinKeyboard) -> int:
    print("Running SendInput self-test with a harmless Shift key press.")
    print("This does not type visible text.")
    print(f"Tool integrity: {win.current_integrity().label}")
    try:
        win.send_vk(VK_SHIFT, "self-test Shift")
    except RuntimeError as exc:
        print(f"Self-test failed: {exc}", file=sys.stderr)
        return EXIT_INPUT
    print("Self-test passed: SendInput accepted the Shift key events.")
    return EXIT_OK


def send_debug_ascii(win: WinKeyboard, text: str, delay_ms: int) -> None:
    for ch in text:
        win.send_ascii_char(ch)
        sleep_ms(delay_ms)


def send_debug_unicode(win: WinKeyboard, text: str, delay_ms: int) -> None:
    for ch in text:
        win.send_unicode_char(ch)
        sleep_ms(delay_ms)


def debug_input(win: WinKeyboard, opt: Options) -> int:
    print("Input debug mode.")
    print("It will type this visible marker into the focused target:")
    print("  TTDBG_ASCII TTDBG_UNICODE U+4E2D=中")
    print("Open a safe text field, Notepad, or a scratch shell before the countdown ends.")

    rc, target = prepare_target_window(win, opt)
    if rc != EXIT_OK or target is None:
        return rc

    tests = [
        ("No-visible Shift key", lambda: win.send_vk(VK_SHIFT, "debug Shift")),
        ("ASCII virtual keys", lambda: send_debug_ascii(win, "TTDBG_ASCII ", opt.delay_ms)),
        ("Unicode ASCII", lambda: send_debug_unicode(win, "TTDBG_UNICODE ", opt.delay_ms)),
        ("Unicode Chinese", lambda: send_debug_unicode(win, "U+4E2D=中", opt.delay_ms)),
    ]

    failed = False
    for name, fn in tests:
        print(f"Testing: {name}")
        try:
            fn()
        except RuntimeError as exc:
            failed = True
            print(f"FAILED: {name}: {exc}", file=sys.stderr)
            break
        print(f"OK: {name}")
        sleep_ms(max(opt.delay_ms, 30))

    print()
    if failed:
        tool = win.current_integrity()
        target_info = win.process_integrity(target.pid)
        if tool.rid is not None and target_info.rid is not None and target_info.rid > tool.rid:
            print("Diagnosis: target integrity is higher than the tool. Windows UIPI can block SendInput.")
        else:
            print("Diagnosis: integrity levels do not prove an admin mismatch.")
            print("Possible causes: secure desktop, RDP/input policy, minimized or disconnected RDP session,")
            print("endpoint security blocking synthetic input, or target app rejecting injected events.")
        print("Next test: run --debug-input against local Notepad. If Notepad passes but RDP fails, the issue is RDP/session/target-specific.")
        return EXIT_INPUT

    print("Debug input completed. If the marker is visible in the target, SendInput works in this session.")
    print("If the command reported OK but no marker appeared, the target had focus/acceptance issues rather than SendInput failure.")
    return EXIT_OK


def type_text(win: WinKeyboard, data: TextData, stats: TextStats, opt: Options, target: TargetWindow) -> int:
    line = 1
    col = 1
    typed_units = 0
    i = 0

    print()
    print("Typing started. Esc aborts, Pause/Break pauses.")
    print(f"Progress: line {line}/{stats.lines}")

    while i < len(data.text):
        rc, new_target = check_runtime_controls(win, opt, target)
        if rc != EXIT_OK:
            if rc == EXIT_ABORTED:
                print(f"\nAborted at line {line}, column {col} after {typed_units} character/event unit(s).")
            return rc
        if new_target is not None:
            target = new_target

        ch = data.text[i]
        try:
            if ch == "\r":
                if i + 1 < len(data.text) and data.text[i + 1] == "\n":
                    i += 1
                if opt.enter_mode == "unicode":
                    win.send_unicode_unit(ord("\r"), "Enter")
                else:
                    win.send_vk(VK_RETURN, "Enter")
                line += 1
                col = 1
                typed_units += 1
                print(f"\rProgress: line {min(line, stats.lines)}/{stats.lines}", end="", flush=True)
                sleep_ms(opt.line_delay_ms)
            elif ch == "\n":
                if opt.enter_mode == "unicode":
                    win.send_unicode_unit(ord("\r"), "Enter")
                else:
                    win.send_vk(VK_RETURN, "Enter")
                line += 1
                col = 1
                typed_units += 1
                print(f"\rProgress: line {min(line, stats.lines)}/{stats.lines}", end="", flush=True)
                sleep_ms(opt.line_delay_ms)
            elif ch == "\t":
                win.send_vk(VK_TAB, "Tab")
                col += 1
                typed_units += 1
                sleep_ms(opt.delay_ms)
            elif opt.ascii_keys:
                win.send_ascii_char(ch)
                col += 1
                typed_units += 1
                sleep_ms(opt.delay_ms)
            else:
                win.send_unicode_char(ch)
                col += 1
                typed_units += 1
                sleep_ms(opt.delay_ms)
        except RuntimeError as exc:
            print(f"\nInput failed at line {line}, column {col}: {exc}", file=sys.stderr)
            return EXIT_INPUT

        i += 1

    print(f"\rProgress: line {stats.lines}/{stats.lines}")
    print(f"Typing completed. Typed {typed_units} character/event unit(s).")
    return EXIT_OK


def run(argv: list[str]) -> int:
    try:
        opt = parse_args(argv)
    except SystemExit as exc:
        return int(exc.code) if isinstance(exc.code, int) else EXIT_ARGS

    if opt.self_test or opt.diagnose or opt.debug_input:
        if os.name != "nt":
            print("Keyboard input diagnostics are only supported on Windows.", file=sys.stderr)
            return EXIT_INPUT
        win = WinKeyboard()
        if opt.self_test:
            rc = self_test_sendinput(win)
            if rc != EXIT_OK or not (opt.diagnose or opt.debug_input):
                return rc
        if opt.debug_input:
            return debug_input(win, opt)
        if opt.diagnose:
            rc, _target = prepare_target_window(win, opt)
            return rc

    try:
        data = read_trans_file(trans_path(), opt)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return EXIT_FILE
    except ValueError as exc:
        print(exc, file=sys.stderr)
        return EXIT_ENCODING

    stats = analyze_text(data.text)
    print_summary(data, stats, opt)

    try:
        validate_text(data, stats, opt)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return EXIT_CONTENT

    if opt.dry_run:
        print("Dry run passed. No typing was performed.")
        return EXIT_OK

    if os.name != "nt":
        print("Keyboard input and Windows exe packaging must run on Windows. Use GitHub Actions or a Windows machine.", file=sys.stderr)
        return EXIT_INPUT

    if stats.non_ascii_count:
        print()
        print(f"Warning: trans.txt contains {stats.non_ascii_count} non-ASCII character(s).")
        print("Unicode input usually works through RDP, but some target applications may reject it.")

    win = WinKeyboard()
    rc, target = prepare_target_window(win, opt)
    if rc != EXIT_OK or target is None:
        return rc
    return type_text(win, data, stats, opt, target)


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
