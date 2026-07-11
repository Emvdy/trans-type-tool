#!/usr/bin/env python3
import argparse
import ctypes
import hashlib
import io
import locale
import os
import stat
import sys
import time
import zipfile
from dataclasses import dataclass, replace
from pathlib import Path, PureWindowsPath

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
DEFAULT_HEX_CHUNK_BYTES = 240
MAX_HEX_CHUNK_BYTES = 2048
MAX_DIRECTORY_ENTRIES = 10000
REMOTE_HEX = "tt.hex"
REMOTE_ZIP = "tt.zip"
REMOTE_STAGE = "tt.out"
REMOTE_HELPER_HEX = "tt.cmd.hex"
REMOTE_HELPER = "tt.cmd"
REMOTE_TEMP_NAMES = frozenset(
    {REMOTE_HEX, REMOTE_ZIP, REMOTE_STAGE, REMOTE_HELPER_HEX, REMOTE_HELPER}
)

INPUT_KEYBOARD = 1
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004

VK_TAB = 0x09
VK_BACK = 0x08
VK_RETURN = 0x0D
VK_SHIFT = 0x10
VK_CONTROL = 0x11
VK_MENU = 0x12
VK_CAPITAL = 0x14
VK_ESCAPE = 0x1B
VK_SPACE = 0x20
VK_F24 = 0x87
TOKEN_QUERY = 0x0008
TOKEN_INTEGRITY_LEVEL = 25
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
CF_UNICODETEXT = 13


@dataclass
class Options:
    delay_ms: int
    line_delay_ms: int
    start_delay_sec: int
    max_bytes: int
    source: str
    file_path: Path | None
    mode: str
    remote_output: str
    remote_output_set: bool
    output_encoding: str
    commands_out: Path | None
    hex_chunk_bytes: int
    ascii_only: bool
    ascii_keys: bool
    legacy_keys: bool
    sendinput: bool
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
    raw_bytes: bytes | None = None
    source_kind: str = "text"
    directory_entries: tuple["DirectoryEntry", ...] = ()


@dataclass(frozen=True)
class DirectoryEntry:
    path: Path
    archive_name: str
    is_directory: bool
    byte_count: int


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
        add_help=False,
        formatter_class=lambda prog: argparse.RawTextHelpFormatter(
            prog, max_help_position=31, width=100
        ),
        description=(
            "通过模拟键盘向当前前台 Windows 窗口输入文本或文件传输命令。\n"
            "Type text or file-transfer commands into the foreground Windows window."
        ),
        epilog=(
            "模式、内容与文件 / modes, content, and files:\n"
            "  simple:\n"
            "    内容/content : 受限小写文本 / restricted lowercase text\n"
            "    允许/allowed : a-z 0-9 space tab newline ` - = [ ] \\ ; ' , . /\n"
            "    文件/files   : 直接输入，不创建远端文件 / direct typing; no remote file\n"
            "  cmd-hex:\n"
            "    内容/content : 文本；preserve 可传任意单文件 / text; preserve allows one arbitrary file\n"
            "    文件/files   : 输出名默认跟随来源；中间文件自动删除 / output follows source; temp deleted\n"
            "  zip-hex (文本或文件 / text or file):\n"
            "    内容/content : 文本；preserve 可传任意单文件 / text; preserve allows one arbitrary file\n"
            "    文件/files   : 输出名默认跟随来源；中间文件自动删除 / output follows source; temp deleted\n"
            "  zip-hex (目录 / directory):\n"
            "    内容/content : 一个真实目录，不允许链接 / one real directory; no links\n"
            "    文件/files   : 解压到来源同名目录；中间文件自动删除 / source-named folder; temp deleted\n"
            "  clipboard 默认目标 .\\trans.txt；路径来源默认目标 .\\basename。\n"
            "  Clipboard defaults to .\\trans.txt; path sources default to .\\basename.\n\n"
            "  直接目标/direct intermediates: cmd-hex=tt.hex; zip-hex=tt.hex+tt.zip。\n"
            "  复杂目标/helper adds: tt.cmd.hex+tt.cmd; cmd-hex also adds tt.out；均自动删除/deleted.\n"
            "  remote-output 是名称或完整目标/name or full target；目录结尾自动追加原名。\n"
            "  A trailing directory appends the source name; default: ./source-name.\n\n"
            "  示例/examples: name=.\\name, ../=..\\source-name, c:/dir/=c:\\dir\\source-name。\n\n"
            "本地默认 / local defaults: source=trans.txt，remote-output=./basename，commands-out=unset。\n\n"
            "运行控制 / runtime controls: Esc 中止/abort，Space 暂停或继续/pause or resume。"
        ),
    )
    parser._optionals.title = "选项 / options"
    parser.add_argument("-h", "--help", action="help", help="显示帮助并退出 / show help and exit")
    parser.add_argument("--delay-ms", metavar="N", type=bounded_int("--delay-ms", 0, 5000), default=DEFAULT_DELAY_MS,
                        help="每个字符延迟 / delay per character (0..5000, default: 20)")
    parser.add_argument("--line-delay-ms", metavar="N", type=bounded_int("--line-delay-ms", 0, 5000), default=DEFAULT_LINE_DELAY_MS,
                        help="每行延迟 / delay after each line (0..5000, default: 100)")
    parser.add_argument("--start-delay-sec", metavar="N", type=bounded_int("--start-delay-sec", 0, 3600), default=DEFAULT_START_DELAY_SEC,
                        help="开始前倒计时 / countdown before typing (default: 5)")
    parser.add_argument("--max-bytes", metavar="N", type=bounded_int("--max-bytes", 1, ABSOLUTE_MAX_BYTES), default=DEFAULT_MAX_BYTES,
                        help="文件字节上限 / source byte limit (default: 1048576)")
    parser.add_argument(
        "--source",
        metavar="SOURCE",
        help="来源：clipboard、file(trans.txt)或本地路径 / source selector (default: file)",
    )
    parser.add_argument("--mode", metavar="MODE", choices=("simple", "cmd-hex", "zip-hex", "complex", "cmd", "zip"), default="simple",
                        help="传输模式 / simple, cmd-hex, or zip-hex (default: simple)")
    parser.add_argument(
        "--remote-output",
        metavar="TARGET",
        help="远端名称或路径 / name or relative/absolute path (default: ./basename)",
    )
    parser.add_argument(
        "--output-encoding",
        choices=("utf8", "utf8-bom", "preserve"),
        default="utf8",
        metavar="E",
        help="输出编码 / utf8, utf8-bom, or preserve (default: utf8)",
    )
    parser.add_argument("--commands-out", metavar="PATH", type=Path,
                        help="导出复杂模式命令 / export generated command stream (default: unset)")
    parser.add_argument(
        "--hex-chunk-bytes",
        type=bounded_int("--hex-chunk-bytes", 1, MAX_HEX_CHUNK_BYTES),
        default=DEFAULT_HEX_CHUNK_BYTES,
        metavar="N",
        help=f"每行 hex 的原始字节数 / bytes per hex line (default: {DEFAULT_HEX_CHUNK_BYTES})",
    )
    parser.add_argument("--ascii-only", action="store_true",
                        help="拒绝非 ASCII 文本 / reject non-ASCII text (default: off)")
    parser.add_argument("--ascii-keys", action="store_true",
                        help="使用 SendInput 虚拟键 / use SendInput virtual keys (default: off)")
    parser.add_argument("--legacy-keys", action="store_true",
                        help="使用 keybd_event ASCII 键 / use legacy ASCII keys (default)")
    parser.add_argument(
        "--sendinput",
        action="store_true",
        help="诊断用 Unicode SendInput / diagnostic transport (default: off)",
    )
    parser.add_argument("--enter-mode", metavar="MODE", choices=("key", "unicode"), default="key",
                        help="回车方式 / key or unicode (default: key)")
    parser.add_argument("--no-focus-check", action="store_true",
                        help="关闭前台窗口检查 / disable foreground-window check (default: check on)")
    parser.add_argument("--dry-run", action="store_true",
                        help="只验证，不输入 / validate without typing (default: off)")
    parser.add_argument("--diagnose", action="store_true",
                        help="显示窗口和完整性级别 / report target and integrity levels (default: off)")
    parser.add_argument("--self-test", action="store_true",
                        help="发送无害 F24 测试键 / send a harmless F24 test key (default: off)")
    parser.add_argument("--debug-input", action="store_true",
                        help="可见输入诊断 / run visible input diagnostics (default: off)")
    args = parser.parse_args(argv)
    if args.mode in ("cmd-hex", "complex", "cmd"):
        mode = "cmd-hex"
    elif args.mode in ("zip-hex", "zip"):
        mode = "zip-hex"
    else:
        mode = "simple"

    source_arg = args.source
    if source_arg in (None, "file"):
        source = "file"
        file_path = None
    elif source_arg == "clipboard":
        source = "clipboard"
        file_path = None
    else:
        source = "file"
        file_path = Path(source_arg)

    if source == "clipboard" or file_path is None:
        source_name = "trans.txt"
    elif "\\" in source_arg:
        source_name = PureWindowsPath(source_arg).name
    else:
        source_name = Path(source_arg).name
    if not source_name:
        parser.error("Cannot derive --remote-output from the source path; specify --remote-output TARGET")
    remote_output = normalize_remote_output(args.remote_output, source_name)
    if remote_output is None:
        parser.error("--remote-output is empty or cannot be normalized")

    if mode == "simple" and args.remote_output is not None:
        parser.error("--remote-output is only valid with cmd-hex or zip-hex")
    if mode == "simple" and args.commands_out is not None:
        parser.error("--commands-out is only valid with --mode cmd-hex or --mode zip-hex")
    if mode in ("cmd-hex", "zip-hex"):
        error = remote_output_error(remote_output)
        if error is not None:
            parser.error(error)

    if args.output_encoding == "preserve" and mode == "simple":
        parser.error("--output-encoding preserve is only valid with --mode cmd-hex or --mode zip-hex")
    if args.output_encoding == "preserve" and source == "clipboard":
        parser.error("--output-encoding preserve requires a file source, not clipboard text")
    return Options(
        delay_ms=args.delay_ms,
        line_delay_ms=args.line_delay_ms,
        start_delay_sec=args.start_delay_sec,
        max_bytes=args.max_bytes,
        source=source,
        file_path=file_path,
        mode=mode,
        remote_output=remote_output,
        remote_output_set=args.remote_output is not None,
        output_encoding=args.output_encoding,
        commands_out=args.commands_out,
        hex_chunk_bytes=args.hex_chunk_bytes,
        ascii_only=args.ascii_only,
        ascii_keys=args.ascii_keys,
        legacy_keys=(args.legacy_keys or not args.sendinput) and not args.ascii_keys,
        sendinput=args.sendinput,
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


def read_clipboard_text(opt: Options) -> TextData:
    if os.name != "nt":
        raise RuntimeError("Clipboard input is only supported by the Windows exe/runtime.")

    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    user32.OpenClipboard.argtypes = [wintypes.HWND]
    user32.OpenClipboard.restype = wintypes.BOOL
    user32.CloseClipboard.argtypes = []
    user32.CloseClipboard.restype = wintypes.BOOL
    user32.GetClipboardData.argtypes = [wintypes.UINT]
    user32.GetClipboardData.restype = wintypes.HANDLE
    kernel32.GlobalLock.argtypes = [wintypes.HANDLE]
    kernel32.GlobalLock.restype = wintypes.LPVOID
    kernel32.GlobalUnlock.argtypes = [wintypes.HANDLE]
    kernel32.GlobalUnlock.restype = wintypes.BOOL

    if not user32.OpenClipboard(None):
        raise RuntimeError(f"Cannot open clipboard. Windows error {ctypes.get_last_error()}.")
    try:
        handle = user32.GetClipboardData(CF_UNICODETEXT)
        if not handle:
            raise RuntimeError("Clipboard does not contain Unicode text.")
        pointer = kernel32.GlobalLock(handle)
        if not pointer:
            raise RuntimeError(f"Cannot lock clipboard text. Windows error {ctypes.get_last_error()}.")
        try:
            text = ctypes.wstring_at(pointer)
        finally:
            kernel32.GlobalUnlock(handle)
    finally:
        user32.CloseClipboard()

    if not text:
        raise RuntimeError("Clipboard text is empty.")
    byte_count = len(text.encode("utf-16-le", errors="strict"))
    if byte_count > opt.max_bytes:
        raise RuntimeError("Clipboard text is larger than --max-bytes.")
    return TextData(path=Path("Clipboard"), text=text, byte_count=byte_count, encoding="Windows clipboard Unicode string")


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


def is_reparse_path(path: Path) -> bool:
    try:
        info = path.lstat()
    except OSError:
        return False
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    return path.is_symlink() or bool(getattr(info, "st_file_attributes", 0) & reparse_flag)


def scan_directory(path: Path, opt: Options) -> tuple[tuple[DirectoryEntry, ...], int]:
    if is_reparse_path(path):
        raise RuntimeError("Directory source cannot be a symbolic link, junction, or other reparse point.")

    entries: list[DirectoryEntry] = []
    total_bytes = 0

    def scan(current: Path, relative: Path) -> None:
        nonlocal total_bytes
        try:
            with os.scandir(current) as iterator:
                children = sorted(iterator, key=lambda item: item.name)
        except OSError as exc:
            raise RuntimeError(f"Cannot enumerate source directory: {current} ({exc})") from exc

        for child in children:
            child_path = Path(child.path)
            child_relative = relative / child.name
            if is_reparse_path(child_path):
                raise RuntimeError(
                    f"Directory source contains a symbolic link, junction, or reparse point: {child_path}"
                )
            try:
                child_stat = child.stat(follow_symlinks=False)
            except OSError as exc:
                raise RuntimeError(f"Cannot inspect directory entry: {child_path} ({exc})") from exc

            if stat.S_ISDIR(child_stat.st_mode):
                entries.append(DirectoryEntry(child_path, child_relative.as_posix() + "/", True, 0))
                if len(entries) > MAX_DIRECTORY_ENTRIES:
                    raise RuntimeError(f"Directory contains more than {MAX_DIRECTORY_ENTRIES} entries.")
                scan(child_path, child_relative)
                continue
            if not stat.S_ISREG(child_stat.st_mode):
                raise RuntimeError(f"Directory source contains an unsupported special file: {child_path}")

            total_bytes += child_stat.st_size
            if total_bytes > opt.max_bytes:
                raise RuntimeError(
                    f"Directory file data is too large: more than {opt.max_bytes} bytes. "
                    "Use --max-bytes to override intentionally."
                )
            entries.append(DirectoryEntry(child_path, child_relative.as_posix(), False, child_stat.st_size))
            if len(entries) > MAX_DIRECTORY_ENTRIES:
                raise RuntimeError(f"Directory contains more than {MAX_DIRECTORY_ENTRIES} entries.")

    scan(path, Path())
    return tuple(entries), total_bytes


def read_trans_file(path: Path, opt: Options) -> TextData:
    try:
        file_stat = path.stat()
    except OSError as exc:
        raise RuntimeError(f"Cannot open input file: {path} ({exc})") from exc

    if stat.S_ISDIR(file_stat.st_mode):
        if opt.mode != "zip-hex":
            raise RuntimeError("Directory sources are only supported with --mode zip-hex.")
        entries, total_bytes = scan_directory(path, opt)
        return TextData(
            path=path,
            text="",
            byte_count=total_bytes,
            encoding="directory tree (file bytes preserved)",
            source_kind="directory",
            directory_entries=entries,
        )
    if not stat.S_ISREG(file_stat.st_mode) or is_reparse_path(path):
        raise RuntimeError("Input path must be a regular file, or a real directory in zip-hex mode.")

    preserve_file = opt.mode in ("cmd-hex", "zip-hex") and opt.output_encoding == "preserve"
    if file_stat.st_size == 0 and not preserve_file:
        raise RuntimeError("Input file is empty.")
    if file_stat.st_size > opt.max_bytes:
        raise RuntimeError(
            f"Input file is too large: {file_stat.st_size} bytes. Limit: {opt.max_bytes} bytes. "
            "Use --max-bytes to override intentionally."
        )

    try:
        raw = path.read_bytes()
    except OSError as exc:
        raise RuntimeError(f"Cannot read input file: {path} ({exc})") from exc

    if preserve_file:
        return TextData(
            path=path,
            text="",
            byte_count=len(raw),
            encoding="raw file bytes (preserved)",
            raw_bytes=raw,
            source_kind="file",
        )

    try:
        text, encoding = decode_bytes(raw)
    except UnicodeDecodeError as exc:
        raise ValueError(f"Input file could not be decoded as UTF-8 or the fallback ANSI code page ({exc})") from exc

    if not text:
        raise RuntimeError("Input file decoded to empty text.")

    return TextData(path=path, text=text, byte_count=len(raw), encoding=encoding, raw_bytes=raw)


def read_text_source(opt: Options) -> TextData:
    if opt.source == "clipboard":
        return read_clipboard_text(opt)
    return read_trans_file(opt.file_path if opt.file_path is not None else trans_path(), opt)


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


def is_safe_cmd_hex_path(path: str) -> bool:
    if not path or len(path) > 240 or path[0] in "/\\" or path[-1] in "/\\":
        return False
    normalized = path.replace("\\", "/")
    for part in normalized.split("/"):
        if part in ("", ".", "..") or part.startswith("-") or part.endswith("."):
            return False
        base = part.split(".", 1)[0]
        if base in {"con", "prn", "aux", "nul"}:
            return False
        if len(base) == 4 and base[:3] in {"com", "lpt"} and base[3] in "123456789":
            return False
    for ch in path:
        if "a" <= ch <= "z" or "0" <= ch <= "9":
            continue
        if ch in ".-/\\":
            continue
        return False
    return True


def collapse_remote_separators(path: str) -> str:
    converted = path.replace("/", "\\")
    is_unc = converted.startswith("\\\\")
    collapsed: list[str] = []
    for ch in converted:
        if ch == "\\" and collapsed and collapsed[-1] == "\\":
            continue
        collapsed.append(ch)
    result = "".join(collapsed)
    if is_unc and result.startswith("\\"):
        result = "\\" + result
    return result


def normalize_remote_output(value: str | None, source_name: str) -> str | None:
    if not source_name:
        return None
    if value is None:
        return ".\\" + source_name
    if not value:
        return None

    directory_target = value[-1] in "/\\" or value in (".", "..")
    normalized = collapse_remote_separators(value)
    if directory_target:
        base = normalized.rstrip("\\")
        if not base:
            return "\\" + source_name
        return base + "\\" + source_name
    if "\\" not in normalized and ":" not in normalized:
        return ".\\" + normalized
    return normalized


def remote_output_parts(target: str) -> tuple[str, str]:
    split_at = target.rfind("\\")
    if split_at < 0:
        return ".", target
    name = target[split_at + 1 :]
    if split_at == 0:
        return "\\", name
    if split_at == 2 and len(target) >= 3 and target[1] == ":":
        return target[:3], name
    return target[:split_at], name


def is_reserved_windows_name(component: str) -> bool:
    base = component.split(".", 1)[0].casefold()
    return base in {"con", "prn", "aux", "nul"} or (
        len(base) == 4 and base[:3] in {"com", "lpt"} and base[3] in "123456789"
    )


def remote_output_error(target: str) -> str | None:
    if not target:
        return "--remote-output must not be empty"
    try:
        units = len(target.encode("utf-16-le", errors="strict")) // 2
    except UnicodeEncodeError:
        return "--remote-output contains invalid Unicode"
    if units > 240:
        return "--remote-output is longer than 240 UTF-16 code units"
    if any(ord(ch) < 32 for ch in target):
        return "--remote-output contains a control character"

    is_unc = target.startswith("\\\\")
    drive_absolute = len(target) >= 3 and target[0].isalpha() and target[1:3] == ":\\"
    if len(target) >= 2 and target[1] == ":" and not drive_absolute:
        return "--remote-output drive paths must be absolute, for example c:/work/file.txt"
    if ":" in (target[3:] if drive_absolute else target):
        return "--remote-output contains ':' outside a drive prefix"

    if is_unc:
        components = target[2:].split("\\")
        if len(components) < 3 or not components[0] or not components[1]:
            return "--remote-output UNC paths must include server, share, and output name"
        if any(part in (".", "..") for part in components):
            return "--remote-output UNC paths cannot contain '.' or '..' segments"
    else:
        start = 3 if drive_absolute else (1 if target.startswith("\\") else 0)
        components = target[start:].split("\\")

    invalid = set('<>"|?*')
    for component in components:
        if component in (".", ".."):
            continue
        if not component:
            return "--remote-output contains an empty path component"
        if component[-1] in ". ":
            return "--remote-output components cannot end with a dot or space"
        if any(ch in invalid for ch in component):
            return "--remote-output contains a Windows-invalid path character"
        if is_reserved_windows_name(component):
            return "--remote-output contains a reserved Windows device name"
        if component.casefold() in REMOTE_TEMP_NAMES:
            return "--remote-output uses a reserved temporary name"

    _parent, name = remote_output_parts(target)
    if name in ("", ".", ".."):
        return "--remote-output must end with a file or directory name"
    return None


def is_shift_free_remote_output(target: str) -> bool:
    return all("a" <= ch <= "z" or "0" <= ch <= "9" or ch in ".-\\" for ch in target)


SHIFT_FREE_SIMPLE_PUNCTUATION = set(" `-=[]\\;',./")
GENERATED_COMMAND_PUNCTUATION = set(" \r\n-./\\'")


def is_shift_free_simple_char(ch: str) -> bool:
    return "a" <= ch <= "z" or "0" <= ch <= "9" or ch in SHIFT_FREE_SIMPLE_PUNCTUATION


def validate_generated_command_stream(text: str) -> None:
    for index, ch in enumerate(text):
        if "a" <= ch <= "z" or "0" <= ch <= "9" or ch in GENERATED_COMMAND_PUNCTUATION:
            continue
        line, col = line_col(text, index)
        raise RuntimeError(
            f"Generated command stream contains forbidden character U+{ord(ch):04X} "
            f"at line {line}, column {col}."
        )


def output_bytes(data: TextData, opt: Options) -> bytes:
    if opt.output_encoding == "preserve":
        if data.raw_bytes is None:
            raise RuntimeError("--output-encoding preserve requires a file path supplied through --source.")
        return data.raw_bytes
    encoded = data.text.encode("utf-8", errors="strict")
    if opt.output_encoding == "utf8-bom":
        return b"\xef\xbb\xbf" + encoded
    return encoded


def validate_complex_paths(opt: Options, directory_source: bool = False) -> None:
    del directory_source
    error = remote_output_error(opt.remote_output)
    if error is not None:
        raise RuntimeError(error)


def validate_text(data: TextData, stats: TextStats, opt: Options) -> None:
    if data.source_kind == "directory":
        if opt.mode != "zip-hex":
            raise RuntimeError("Directory sources are only supported with --mode zip-hex.")
        validate_complex_paths(opt, directory_source=True)
        return
    if data.source_kind == "file":
        if opt.mode not in ("cmd-hex", "zip-hex") or opt.output_encoding != "preserve":
            raise RuntimeError("Raw file bytes require cmd-hex or zip-hex with --output-encoding preserve.")
        if data.raw_bytes is None:
            raise RuntimeError("Raw file source is missing its original bytes.")
        validate_complex_paths(opt)
        return

    if not data.text:
        raise RuntimeError("Input decoded to empty text.")
    if stats.control_count:
        line, col = line_col(data.text, stats.first_control)
        raise RuntimeError(
            f"Input contains unsupported control characters. First one at line {line}, column {col}. "
            "Allowed control characters are tab and newlines only."
        )
    if stats.surrogate_error_count:
        line, col = line_col(data.text, stats.first_surrogate_error)
        raise RuntimeError(f"Input contains invalid surrogate data. First issue at line {line}, column {col}.")
    if opt.ascii_only and stats.non_ascii_count:
        line, col = line_col(data.text, stats.first_non_ascii)
        raise RuntimeError(
            f"--ascii-only is enabled, but input has non-ASCII characters. "
            f"First one at line {line}, column {col}."
        )
    if opt.mode in ("cmd-hex", "zip-hex"):
        if opt.output_encoding == "preserve" and data.raw_bytes is None:
            raise RuntimeError("--output-encoding preserve requires file input; clipboard text has no original byte encoding.")
        validate_complex_paths(opt)
        return
    if opt.commands_out is not None:
        raise RuntimeError("--commands-out is only valid with --mode cmd-hex or --mode zip-hex.")
    if opt.remote_output_set:
        raise RuntimeError("--remote-output is only valid with cmd-hex or zip-hex.")
    for index, ch in enumerate(data.text):
        if ch in "\r\n\t" or is_shift_free_simple_char(ch):
            continue
        line, col = line_col(data.text, index)
        raise RuntimeError(
            f"Simple mode only accepts lowercase, digits, and unmodified US-keyboard characters. "
            f"Character U+{ord(ch):04X} at line {line}, column {col} would require a modifier or Unicode input. "
            "Use --mode cmd-hex or --mode zip-hex."
        )


def print_summary(data: TextData, stats: TextStats, opt: Options) -> None:
    print(f"Source: {data.path}")
    print(f"Encoding: {data.encoding}")
    print(f"Bytes: {data.byte_count}")
    if data.source_kind == "directory":
        file_count = sum(not entry.is_directory for entry in data.directory_entries)
        directory_count = sum(entry.is_directory for entry in data.directory_entries)
        print(f"Directory entries: {file_count} file(s), {directory_count} subdirectory(s)")
    elif data.source_kind == "file":
        print("Content: arbitrary regular file bytes")
    else:
        print(f"Characters: {len(data.text)}")
        print(f"Lines: {stats.lines}")
        print(f"Non-ASCII characters: {stats.non_ascii_count}")
    print(f"Delay: {opt.delay_ms} ms per character, {opt.line_delay_ms} ms per line")
    print(f"Focus check: {'off' if opt.no_focus_check else 'on'}")
    if opt.mode == "cmd-hex":
        print("Mode: cmd-hex transfer through remote cmd/certutil")
        print(f"Output encoding: {opt.output_encoding}")
        print(f"Remote output: {opt.remote_output}")
        print(f"Path transport: {'hex helper' if not is_shift_free_remote_output(opt.remote_output) else 'direct'}")
        print(f"Hex chunk: {opt.hex_chunk_bytes} bytes per generated line")
    elif opt.mode == "zip-hex":
        print("Mode: zip-hex transfer through remote PowerShell/certutil/Expand-Archive")
        if data.source_kind == "directory":
            print("Output encoding: directory file bytes preserved")
            print(f"Remote destination directory: {opt.remote_output}")
        else:
            print(f"Output encoding: {opt.output_encoding}")
            print(f"Remote output: {opt.remote_output}")
        print(f"Path transport: {'hex helper' if not is_shift_free_remote_output(opt.remote_output) else 'direct'}")
        print(f"Hex chunk: {opt.hex_chunk_bytes} bytes per generated line")
    elif opt.legacy_keys:
        input_mode = "Legacy keybd_event ASCII keys"
        print(f"Input mode: {input_mode}")
    elif opt.ascii_keys:
        input_mode = "ASCII virtual keys"
        print(f"Input mode: {input_mode}")
    else:
        input_mode = "Unicode SendInput"
        print(f"Input mode: {input_mode}")
    if opt.mode in ("cmd-hex", "zip-hex"):
        enter_mode = "physical Return key (forced for complex mode)"
    elif opt.legacy_keys:
        enter_mode = "legacy VK_RETURN"
    else:
        enter_mode = "Unicode CR" if opt.enter_mode == "unicode" else "VK_RETURN"
    print(f"Enter mode: {enter_mode}")


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
        self.user32.keybd_event.argtypes = [wintypes.BYTE, wintypes.BYTE, wintypes.DWORD, ctypes.c_size_t]
        self.user32.keybd_event.restype = None
        self.user32.GetAsyncKeyState.argtypes = [ctypes.c_int]
        self.user32.GetAsyncKeyState.restype = ctypes.c_short
        self.user32.GetKeyState.argtypes = [ctypes.c_int]
        self.user32.GetKeyState.restype = ctypes.c_short
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
        ctypes.set_last_error(0)
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

    def legacy_vk(self, vk: int) -> None:
        self.user32.keybd_event(vk, 0, 0, 0)
        self.user32.keybd_event(vk, 0, KEYEVENTF_KEYUP, 0)

    def legacy_ascii_char(self, ch: str) -> None:
        vk_scan = self.user32.VkKeyScanW(ch)
        if vk_scan == -1:
            raise RuntimeError(f"Cannot map ASCII character U+{ord(ch):04X} through the current keyboard layout.")

        vk = vk_scan & 0xFF
        shift_state = (vk_scan >> 8) & 0xFF
        if shift_state != 0:
            raise RuntimeError(
                f"Character U+{ord(ch):04X} requires Shift/Ctrl/Alt in the current keyboard layout. "
                "Use cmd-hex or zip-hex."
            )
        self.legacy_vk(vk)

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
        if shift_state != 0:
            raise RuntimeError(
                f"Character U+{ord(ch):04X} requires Shift/Ctrl/Alt in the current keyboard layout. "
                "Use cmd-hex or zip-hex."
            )
        self.send_inputs(
            [self.key_input(vk=vk), self.key_input(vk=vk, flags=KEYEVENTF_KEYUP)],
            "ASCII character",
        )

    def get_async_key_state(self, vk: int) -> int:
        return self.user32.GetAsyncKeyState(vk)

    def keyboard_state_error(self) -> str | None:
        if self.user32.GetKeyState(VK_CAPITAL) & 0x0001:
            return "Caps Lock is on. Turn it off before typing."
        for vk, name in ((VK_SHIFT, "Shift"), (VK_CONTROL, "Ctrl"), (VK_MENU, "Alt")):
            if self.get_async_key_state(vk) & 0x8000:
                return f"{name} is currently held. Release all modifier keys before typing."
        return None

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

        state_error = win.keyboard_state_error()
        if state_error:
            rc = wait_for_console_enter_or_esc(state_error)
            if rc != EXIT_OK:
                return rc, None
            continue

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


def send_pause_backspace(win: WinKeyboard, opt: Options) -> None:
    if opt.legacy_keys:
        win.legacy_vk(VK_BACK)
    else:
        win.send_vk(VK_BACK, "pause Space cleanup Backspace")


def wait_for_space_release(win: WinKeyboard, target: TargetWindow, opt: Options) -> int:
    while win.get_async_key_state(VK_SPACE) & 0x8000:
        if win.get_async_key_state(VK_ESCAPE) & 0x8000:
            return EXIT_ABORTED
        if not opt.no_focus_check and win.foreground_window().hwnd != target.hwnd:
            return EXIT_ABORTED
        time.sleep(0.01)
    return EXIT_OK


def pause_until_space(win: WinKeyboard, opt: Options, target: TargetWindow) -> tuple[int, TargetWindow]:
    rc = wait_for_space_release(win, target, opt)
    if rc != EXIT_OK:
        return rc, target
    if not opt.no_focus_check and win.foreground_window().hwnd != target.hwnd:
        print("\nForeground window changed while handling Space; aborting.", file=sys.stderr)
        return EXIT_ABORTED, target
    try:
        send_pause_backspace(win, opt)
    except RuntimeError as exc:
        print(f"\nCould not erase the pause Space from the target: {exc}", file=sys.stderr)
        return EXIT_INPUT, target

    print("\nPaused. Press Space again to continue; Esc aborts.")
    while True:
        if win.get_async_key_state(VK_ESCAPE) & 0x8000:
            return EXIT_ABORTED, target
        if not opt.no_focus_check and win.foreground_window().hwnd != target.hwnd:
            print("\nForeground window changed while paused; aborting.", file=sys.stderr)
            return EXIT_ABORTED, target
        if win.get_async_key_state(VK_SPACE) & 0x8000:
            rc = wait_for_space_release(win, target, opt)
            if rc != EXIT_OK:
                return rc, target
            if not opt.no_focus_check and win.foreground_window().hwnd != target.hwnd:
                print("\nForeground window changed while paused; aborting.", file=sys.stderr)
                return EXIT_ABORTED, target
            try:
                send_pause_backspace(win, opt)
            except RuntimeError as exc:
                print(f"\nCould not erase the resume Space from the target: {exc}", file=sys.stderr)
                return EXIT_INPUT, target
            print("Resuming.")
            return EXIT_OK, target
        time.sleep(0.01)


def check_runtime_controls(win: WinKeyboard, opt: Options, target: TargetWindow) -> tuple[int, TargetWindow | None]:
    if win.get_async_key_state(VK_ESCAPE) & 0x8000:
        return EXIT_ABORTED, target

    if not opt.no_focus_check:
        current = win.foreground_window()
        if current.hwnd != target.hwnd:
            return pause_and_recapture(win, opt, "Foreground window changed.")

    if win.get_async_key_state(VK_SPACE) & 0x8000:
        return pause_until_space(win, opt, target)

    state_error = win.keyboard_state_error()
    if state_error:
        return pause_and_recapture(win, opt, state_error)

    return EXIT_OK, target


def wait_typing_delay(
    win: WinKeyboard, opt: Options, target: TargetWindow, delay_ms: int
) -> tuple[int, TargetWindow | None]:
    remaining_ms = delay_ms
    while remaining_ms > 0:
        rc, new_target = check_runtime_controls(win, opt, target)
        if rc != EXIT_OK:
            return rc, new_target
        if new_target is not None:
            target = new_target
        slice_ms = min(10, remaining_ms)
        time.sleep(slice_ms / 1000.0)
        remaining_ms -= slice_ms
    return EXIT_OK, target


def sleep_ms(ms: int) -> None:
    if ms > 0:
        time.sleep(ms / 1000.0)


def self_test_sendinput(win: WinKeyboard) -> int:
    print("Running SendInput self-test with a harmless F24 key press.")
    print("This does not type visible text.")
    print(f"Python: {sys.version.split()[0]} ({'64-bit' if ctypes.sizeof(ctypes.c_void_p) == 8 else '32-bit'})")
    print(f"ctypes INPUT size: {ctypes.sizeof(win.INPUT)} bytes")
    print(f"ctypes KEYBDINPUT size: {ctypes.sizeof(win.KEYBDINPUT)} bytes")
    print(f"Tool integrity: {win.current_integrity().label}")
    target = win.foreground_window()
    print_target(target)
    if target.hwnd:
        print_integrity_report(win, target)
    try:
        win.send_vk(VK_F24, "self-test F24")
    except RuntimeError as exc:
        print(f"Self-test failed: {exc}", file=sys.stderr)
        print("Trying legacy keybd_event F24 test. This API has no reliable success return value.")
        try:
            win.legacy_vk(VK_F24)
            print("Legacy keybd_event call completed. If this also has no effect, the system/session is blocking synthetic input.")
        except Exception as legacy_exc:
            print(f"Legacy keybd_event call raised an exception: {legacy_exc}", file=sys.stderr)
        return EXIT_INPUT
    print("Self-test passed: SendInput accepted the F24 key events.")
    return EXIT_OK


def self_test_legacy(win: WinKeyboard) -> int:
    print("Running legacy keybd_event self-test with a harmless F24 key press.")
    print("This does not type visible text.")
    print("Note: keybd_event does not report whether the target accepted the event.")
    print(f"Python: {sys.version.split()[0]} ({'64-bit' if ctypes.sizeof(ctypes.c_void_p) == 8 else '32-bit'})")
    print(f"Tool integrity: {win.current_integrity().label}")
    target = win.foreground_window()
    print_target(target)
    if target.hwnd:
        print_integrity_report(win, target)
    try:
        win.legacy_vk(VK_F24)
    except Exception as exc:
        print(f"Legacy self-test failed: {exc}", file=sys.stderr)
        return EXIT_INPUT
    print("Legacy self-test completed. To verify visible typing, run --debug-input against Notepad or the RDP target.")
    return EXIT_OK


def self_test_input(win: WinKeyboard, opt: Options) -> int:
    if opt.legacy_keys:
        return self_test_legacy(win)
    return self_test_sendinput(win)


def send_debug_ascii(win: WinKeyboard, text: str, delay_ms: int) -> None:
    for ch in text:
        win.send_ascii_char(ch)
        sleep_ms(delay_ms)


def send_debug_unicode(win: WinKeyboard, text: str, delay_ms: int) -> None:
    for ch in text:
        win.send_unicode_char(ch)
        sleep_ms(delay_ms)


def send_debug_legacy_ascii(win: WinKeyboard, text: str, delay_ms: int) -> None:
    for ch in text:
        win.legacy_ascii_char(ch)
        sleep_ms(delay_ms)


def debug_input(win: WinKeyboard, opt: Options) -> int:
    print("Input debug mode.")
    print("It will type this visible marker into the focused target:")
    print("  ttdbg-legacy ttdbg-ascii ttdbg-unicode unicode-chinese=中")
    print("Open a safe text field, Notepad, or a scratch shell before the countdown ends.")

    rc, target = prepare_target_window(win, opt)
    if rc != EXIT_OK or target is None:
        return rc

    tests = [
        ("Legacy keybd_event ASCII", lambda: send_debug_legacy_ascii(win, "ttdbg-legacy ", opt.delay_ms)),
        ("SendInput no-visible F24 key", lambda: win.send_vk(VK_F24, "debug F24")),
        ("ASCII virtual keys", lambda: send_debug_ascii(win, "ttdbg-ascii ", opt.delay_ms)),
        ("Unicode ASCII", lambda: send_debug_unicode(win, "ttdbg-unicode ", opt.delay_ms)),
        ("Unicode Chinese", lambda: send_debug_unicode(win, "unicode-chinese=中", opt.delay_ms)),
    ]

    failed = False
    for name, fn in tests:
        print(f"Testing: {name}")
        try:
            fn()
        except RuntimeError as exc:
            failed = True
            print(f"FAILED: {name}: {exc}", file=sys.stderr)
            continue
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
        print("Next test: run --debug-input against local Notepad. If only legacy succeeds, use the default mode and avoid --sendinput/--ascii-keys.")
        return EXIT_INPUT

    print("Debug input completed. If all markers are visible in the target, all input modes work in this session.")
    print("If only ttdbg-legacy appears, use the default key mode or a complex transfer mode.")
    return EXIT_OK


def type_text(win: WinKeyboard, data: TextData, stats: TextStats, opt: Options, target: TargetWindow) -> int:
    line = 1
    col = 1
    typed_units = 0
    i = 0

    print()
    print("Typing started. Esc aborts. Space pauses or resumes.")
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
        event_delay_ms = opt.delay_ms
        try:
            if ch == "\r":
                if i + 1 < len(data.text) and data.text[i + 1] == "\n":
                    i += 1
                if opt.legacy_keys:
                    win.legacy_vk(VK_RETURN)
                elif opt.enter_mode == "unicode":
                    win.send_unicode_unit(ord("\r"), "Enter")
                else:
                    win.send_vk(VK_RETURN, "Enter")
                line += 1
                col = 1
                typed_units += 1
                print(f"\rProgress: line {min(line, stats.lines)}/{stats.lines}", end="", flush=True)
                event_delay_ms = opt.line_delay_ms
            elif ch == "\n":
                if opt.legacy_keys:
                    win.legacy_vk(VK_RETURN)
                elif opt.enter_mode == "unicode":
                    win.send_unicode_unit(ord("\r"), "Enter")
                else:
                    win.send_vk(VK_RETURN, "Enter")
                line += 1
                col = 1
                typed_units += 1
                print(f"\rProgress: line {min(line, stats.lines)}/{stats.lines}", end="", flush=True)
                event_delay_ms = opt.line_delay_ms
            elif ch == "\t":
                if opt.legacy_keys:
                    win.legacy_vk(VK_TAB)
                else:
                    win.send_vk(VK_TAB, "Tab")
                col += 1
                typed_units += 1
            elif opt.legacy_keys:
                win.legacy_ascii_char(ch)
                col += 1
                typed_units += 1
            elif opt.ascii_keys:
                win.send_ascii_char(ch)
                col += 1
                typed_units += 1
            else:
                win.send_unicode_char(ch)
                col += 1
                typed_units += 1
        except RuntimeError as exc:
            print(f"\nInput failed at line {line}, column {col}: {exc}", file=sys.stderr)
            return EXIT_INPUT

        rc, new_target = wait_typing_delay(win, opt, target, event_delay_ms)
        if rc != EXIT_OK:
            if rc == EXIT_ABORTED:
                print(f"\nAborted at line {line}, column {col} after {typed_units} character/event unit(s).")
            return rc
        if new_target is not None:
            target = new_target

        i += 1

    print(f"\rProgress: line {stats.lines}/{stats.lines}")
    print(f"Typing completed. Typed {typed_units} character/event unit(s).")
    return EXIT_OK


def hex_payload_from_bytes(raw: bytes, chunk_bytes: int) -> tuple[str, int, int]:
    encoded = raw.hex()
    line_chars = chunk_bytes * 2
    lines = [encoded[i : i + line_chars] for i in range(0, len(encoded), line_chars)]
    return "\n".join(lines) + "\n", len(raw), len(lines)


def hex_payload(text: str, chunk_bytes: int) -> tuple[str, int, int]:
    raw = text.encode("utf-8", errors="strict")
    return hex_payload_from_bytes(raw, chunk_bytes)


def normalized_zip_entry_name(remote_output: str) -> str:
    return remote_output_parts(remote_output)[1]


def zip_payload(raw: bytes, opt: Options) -> bytes:
    entry_name = normalized_zip_entry_name(opt.remote_output)
    buffer = io.BytesIO()
    info = zipfile.ZipInfo(entry_name)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.date_time = (1980, 1, 1, 0, 0, 0)
    with zipfile.ZipFile(buffer, "w") as archive:
        archive.writestr(info, raw)
    return buffer.getvalue()


def zip_directory_payload(data: TextData) -> bytes:
    if data.source_kind != "directory":
        raise RuntimeError("Internal error: directory ZIP requested for a non-directory source.")

    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, "w") as archive:
        for entry in data.directory_entries:
            info = zipfile.ZipInfo(entry.archive_name)
            info.date_time = (1980, 1, 1, 0, 0, 0)
            info.compress_type = zipfile.ZIP_STORED if entry.is_directory else zipfile.ZIP_DEFLATED
            info.external_attr = (0o40755 if entry.is_directory else 0o100644) << 16
            if entry.is_directory:
                archive.writestr(info, b"")
                continue

            if is_reparse_path(entry.path) or not entry.path.is_file():
                raise RuntimeError(f"Directory entry changed or became unsafe while archiving: {entry.path}")
            try:
                content = entry.path.read_bytes()
            except OSError as exc:
                raise RuntimeError(f"Cannot read directory file while archiving: {entry.path} ({exc})") from exc
            if len(content) != entry.byte_count:
                raise RuntimeError(f"Directory file changed size while archiving: {entry.path}")
            archive.writestr(info, content)
    return buffer.getvalue()


def generated_ascii_data(text: str, label: str) -> TextData:
    return TextData(path=Path(label), text=text, byte_count=len(text), encoding="generated ASCII command stream")


def quote_powershell_literal(value: str) -> str:
    if "'" in value:
        raise RuntimeError("Internal error: PowerShell literal contains a single quote.")
    return f"'{value}'"


def append_hex_writer_commands(commands: list[str], payload: str, destination: str) -> None:
    lines = [line for line in payload.splitlines() if line]
    for index, line in enumerate(lines):
        writer = "set-content" if index == 0 else "add-content"
        commands.append(
            f"{writer} -encoding ascii {quote_powershell_literal(destination)} {quote_powershell_literal(line)}"
        )


def build_hex_writer_commands(payload: str) -> list[str]:
    commands = ["powershell -noprofile"]
    append_hex_writer_commands(commands, payload, REMOTE_HEX)
    return commands


def append_remote_parent_command(commands: list[str], target: str) -> None:
    parent, _name = remote_output_parts(target)
    if parent != ".":
        commands.append(f"new-item -itemtype directory -force {quote_powershell_literal(parent)}")


def powershell_script_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def build_remote_helper(opt: Options, mode: str, directory_source: bool = False) -> bytes:
    parent, _name = remote_output_parts(opt.remote_output)
    parent_literal = powershell_script_literal(parent)
    target_literal = powershell_script_literal(opt.remote_output)
    create_parent = f"[system.io.directory]::createdirectory({parent_literal});"
    if mode == "cmd-hex":
        script = (
            create_parent
            + f"[system.io.file]::copy('{REMOTE_STAGE}',{target_literal},$true);"
            + f"certutil -hashfile {target_literal} sha256"
        )
    elif directory_source:
        script = (
            create_parent
            + f"certutil -hashfile '{REMOTE_ZIP}' sha256;"
            + f"expand-archive -force -literalpath '{REMOTE_ZIP}' -destinationpath {target_literal}"
        )
    else:
        script = (
            create_parent
            + f"expand-archive -force -literalpath '{REMOTE_ZIP}' -destinationpath {parent_literal};"
            + f"certutil -hashfile {target_literal} sha256"
        )

    batch_script = script.replace("^", "^^").replace("%", "%%")
    batch = (
        "@echo off\r\n"
        "setlocal disabledelayedexpansion\r\n"
        "chcp 65001 >nul\r\n"
        f'powershell -noprofile -noninteractive -command "{batch_script}"\r\n'
        "exit /b %errorlevel%\r\n"
    )
    return batch.encode("utf-8", errors="strict")


def append_remote_helper_commands(
    commands: list[str], opt: Options, mode: str, directory_source: bool = False
) -> None:
    helper = build_remote_helper(opt, mode, directory_source)
    helper_payload, _count, _lines = hex_payload_from_bytes(helper, opt.hex_chunk_bytes)
    append_hex_writer_commands(commands, helper_payload, REMOTE_HELPER_HEX)
    commands.append(
        f"certutil -f -decodehex {quote_powershell_literal(REMOTE_HELPER_HEX)} "
        f"{quote_powershell_literal(REMOTE_HELPER)}"
    )
    commands.append(f"cmd /d /c {REMOTE_HELPER}")


def append_cleanup_commands(commands: list[str], names: tuple[str, ...]) -> None:
    for name in names:
        commands.append(f"remove-item -force {quote_powershell_literal(name)}")


def build_cmd_hex_commands(payload: str, opt: Options) -> str:
    commands = build_hex_writer_commands(payload)
    helper = not is_shift_free_remote_output(opt.remote_output)
    decode_target = REMOTE_STAGE if helper else opt.remote_output
    if not helper:
        append_remote_parent_command(commands, opt.remote_output)
    if payload.strip():
        commands.append(
            f"certutil -f -decodehex {quote_powershell_literal(REMOTE_HEX)} "
            f"{quote_powershell_literal(decode_target)}"
        )
    else:
        commands.append(f"set-content -nonewline {quote_powershell_literal(REMOTE_HEX)} ''")
        commands.append(f"set-content -nonewline {quote_powershell_literal(decode_target)} ''")
    if helper:
        append_remote_helper_commands(commands, opt, "cmd-hex")
        append_cleanup_commands(
            commands, (REMOTE_HEX, REMOTE_STAGE, REMOTE_HELPER_HEX, REMOTE_HELPER)
        )
    else:
        commands.append(f"certutil -hashfile {quote_powershell_literal(opt.remote_output)} sha256")
        append_cleanup_commands(commands, (REMOTE_HEX,))
    commands.append("exit")
    return "\n".join(commands) + "\n"


def build_zip_hex_commands(payload: str, opt: Options, directory_source: bool = False) -> str:
    commands = build_hex_writer_commands(payload)
    helper = not is_shift_free_remote_output(opt.remote_output)
    if not helper:
        append_remote_parent_command(commands, opt.remote_output)
    commands.append(
        f"certutil -f -decodehex {quote_powershell_literal(REMOTE_HEX)} "
        f"{quote_powershell_literal(REMOTE_ZIP)}"
    )
    if helper:
        append_remote_helper_commands(commands, opt, "zip-hex", directory_source)
    elif directory_source:
        commands.append(f"certutil -hashfile {quote_powershell_literal(REMOTE_ZIP)} sha256")
        commands.append(
            f"expand-archive -force {quote_powershell_literal(REMOTE_ZIP)} "
            f"{quote_powershell_literal(opt.remote_output)}"
        )
    else:
        parent, _name = remote_output_parts(opt.remote_output)
        commands.append(
            f"expand-archive -force {quote_powershell_literal(REMOTE_ZIP)} "
            f"{quote_powershell_literal(parent)}"
        )
        commands.append(f"certutil -hashfile {quote_powershell_literal(opt.remote_output)} sha256")
    cleanup = (REMOTE_HEX, REMOTE_ZIP)
    if helper:
        cleanup += (REMOTE_HELPER_HEX, REMOTE_HELPER)
    append_cleanup_commands(commands, cleanup)
    commands.append("exit")
    return "\n".join(commands) + "\n"


def keyboard_opt_for_generated_ascii(opt: Options) -> Options:
    if opt.ascii_keys:
        return replace(
            opt,
            mode="simple",
            ascii_keys=True,
            legacy_keys=False,
            sendinput=False,
            commands_out=None,
            enter_mode="key",
            remote_output_set=False,
        )
    return replace(
        opt,
        mode="simple",
        ascii_keys=False,
        legacy_keys=True,
        sendinput=False,
        commands_out=None,
        enter_mode="key",
        remote_output_set=False,
    )


def type_generated_ascii(win: WinKeyboard, text: str, label: str, opt: Options, target: TargetWindow) -> int:
    generated = generated_ascii_data(text, label)
    stats = analyze_text(generated.text)
    key_opt = keyboard_opt_for_generated_ascii(opt)
    try:
        validate_text(generated, stats, key_opt)
    except RuntimeError as exc:
        print(exc, file=sys.stderr)
        return EXIT_CONTENT
    return type_text(win, generated, stats, key_opt, target)


def prepare_generated_commands(commands: str, opt: Options) -> int:
    try:
        validate_generated_command_stream(commands)
        if opt.commands_out is not None:
            opt.commands_out.write_text(commands, encoding="ascii", newline="\n")
            print(f"Generated commands written to: {opt.commands_out}")
    except (OSError, RuntimeError) as exc:
        print(f"Generated command validation/output failed: {exc}", file=sys.stderr)
        return EXIT_CONTENT
    return EXIT_OK


def send_enter_key(win: WinKeyboard, opt: Options) -> None:
    if opt.legacy_keys:
        win.legacy_vk(VK_RETURN)
    elif opt.enter_mode == "unicode":
        win.send_unicode_unit(ord("\r"), "Enter")
    else:
        win.send_vk(VK_RETURN, "Enter")


def run_cmd_hex_transfer(data: TextData, opt: Options) -> int:
    try:
        raw = output_bytes(data, opt)
        payload, byte_count, line_count = hex_payload_from_bytes(raw, opt.hex_chunk_bytes)
    except UnicodeEncodeError as exc:
        print(f"Input text could not be encoded as UTF-8: {exc}", file=sys.stderr)
        return EXIT_ENCODING
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return EXIT_ENCODING

    print("cmd-hex transfer mode.")
    print(f"Output bytes to transfer: {byte_count}")
    print(f"Remote output: {opt.remote_output}")
    print(f"Hex chunk: {opt.hex_chunk_bytes} bytes per generated line ({line_count} line(s))")
    print(f"Expected SHA-256: {hashlib.sha256(raw).hexdigest()}")
    if is_shift_free_remote_output(opt.remote_output):
        print(f"Remote temporary {REMOTE_HEX} is deleted after reconstruction.")
    else:
        print(
            f"Remote temporary {REMOTE_HEX}, {REMOTE_STAGE}, {REMOTE_HELPER_HEX}, "
            f"and {REMOTE_HELPER} are deleted after reconstruction."
        )
    print("Focus a remote cmd.exe or PowerShell prompt. This mode uses PowerShell Set-Content/Add-Content, not redirection or Ctrl+Z/F6.")

    commands = build_cmd_hex_commands(payload, opt)
    rc = prepare_generated_commands(commands, opt)
    if rc != EXIT_OK:
        return rc
    if opt.dry_run:
        print(f"Generated hex characters: {sum(len(line) for line in payload.splitlines())}")
        print(f"Generated command characters: {len(commands)}")
        print("Dry run passed. No typing was performed.")
        return EXIT_OK

    if os.name != "nt":
        print("Keyboard input must run on Windows for the Windows exe. Use GitHub Actions to build the exe, then run it on Windows.", file=sys.stderr)
        return EXIT_INPUT

    win = WinKeyboard()
    rc, target = prepare_target_window(win, opt)
    if rc != EXIT_OK or target is None:
        return rc

    key_opt = keyboard_opt_for_generated_ascii(opt)
    rc = type_generated_ascii(win, commands, "cmd-hex echo/certutil commands", key_opt, target)
    if rc != EXIT_OK:
        return rc

    print("cmd-hex transfer typing completed. Verify the remote output file before running it.")
    return EXIT_OK


def run_zip_hex_transfer(data: TextData, opt: Options) -> int:
    try:
        directory_source = data.source_kind == "directory"
        if directory_source:
            raw = None
            zipped = zip_directory_payload(data)
            raw_byte_count = data.byte_count
        else:
            raw = output_bytes(data, opt)
            zipped = zip_payload(raw, opt)
            raw_byte_count = len(raw)
        payload, zip_byte_count, line_count = hex_payload_from_bytes(zipped, opt.hex_chunk_bytes)
    except UnicodeEncodeError as exc:
        print(f"Input text could not be encoded as UTF-8: {exc}", file=sys.stderr)
        return EXIT_ENCODING
    except RuntimeError as exc:
        print(str(exc), file=sys.stderr)
        return EXIT_FILE

    ratio = (zip_byte_count / raw_byte_count) if raw_byte_count else 0.0
    print("zip-hex transfer mode.")
    print(f"Output bytes before zip: {raw_byte_count}")
    print(f"Zip bytes to transfer: {zip_byte_count} ({ratio:.2%} of output size)")
    print(f"Remote output: {opt.remote_output}")
    print(f"Hex chunk: {opt.hex_chunk_bytes} bytes per generated line ({line_count} line(s))")
    if directory_source:
        print(f"Expected archive SHA-256: {hashlib.sha256(zipped).hexdigest()}")
        print("The remote destination is merged/overwritten; unrelated existing files are not removed.")
    else:
        assert raw is not None
        print(f"Expected SHA-256: {hashlib.sha256(raw).hexdigest()}")
    if is_shift_free_remote_output(opt.remote_output):
        print(f"Remote temporary {REMOTE_HEX} and {REMOTE_ZIP} are deleted after extraction.")
    else:
        print(
            f"Remote temporary {REMOTE_HEX}, {REMOTE_ZIP}, {REMOTE_HELPER_HEX}, "
            f"and {REMOTE_HELPER} are deleted after extraction."
        )
    print("Focus a remote cmd.exe or PowerShell prompt. This mode decodes a zip, then runs Expand-Archive.")

    commands = build_zip_hex_commands(payload, opt, directory_source=directory_source)
    rc = prepare_generated_commands(commands, opt)
    if rc != EXIT_OK:
        return rc
    if opt.dry_run:
        print(f"Generated hex characters: {sum(len(line) for line in payload.splitlines())}")
        print(f"Generated command characters: {len(commands)}")
        print("Dry run passed. No typing was performed.")
        return EXIT_OK

    if os.name != "nt":
        print("Keyboard input must run on Windows for the Windows exe. Use the macOS binary on macOS.", file=sys.stderr)
        return EXIT_INPUT

    win = WinKeyboard()
    rc, target = prepare_target_window(win, opt)
    if rc != EXIT_OK or target is None:
        return rc

    key_opt = keyboard_opt_for_generated_ascii(opt)
    rc = type_generated_ascii(win, commands, "zip-hex expand-archive commands", key_opt, target)
    if rc != EXIT_OK:
        return rc

    print("zip-hex transfer typing completed. Verify the remote output file before running it.")
    return EXIT_OK


def run(argv: list[str]) -> int:
    if os.name == "nt":
        if hasattr(sys.stdout, "reconfigure"):
            sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        if hasattr(sys.stderr, "reconfigure"):
            sys.stderr.reconfigure(encoding="utf-8", errors="replace")
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
            rc = self_test_input(win, opt)
            if rc != EXIT_OK or not (opt.diagnose or opt.debug_input):
                return rc
        if opt.debug_input:
            return debug_input(win, opt)
        if opt.diagnose:
            rc, _target = prepare_target_window(win, opt)
            return rc

    try:
        data = read_text_source(opt)
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

    if opt.mode == "cmd-hex":
        return run_cmd_hex_transfer(data, opt)
    if opt.mode == "zip-hex":
        return run_zip_hex_transfer(data, opt)

    if opt.dry_run:
        print("Dry run passed. No typing was performed.")
        return EXIT_OK

    if os.name != "nt":
        print("Keyboard input and Windows exe packaging must run on Windows. Use GitHub Actions or a Windows machine.", file=sys.stderr)
        return EXIT_INPUT

    win = WinKeyboard()
    rc, target = prepare_target_window(win, opt)
    if rc != EXIT_OK or target is None:
        return rc
    return type_text(win, data, stats, opt, target)


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
