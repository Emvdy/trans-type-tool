#import <ApplicationServices/ApplicationServices.h>
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

#define PROGRAM_NAME "trans_type_mac"

static constexpr int EXIT_OK = 0;
static constexpr int EXIT_ARGS = 2;
static constexpr int EXIT_FILE = 3;
static constexpr int EXIT_ENCODING = 4;
static constexpr int EXIT_CONTENT = 5;
static constexpr int EXIT_ABORTED = 6;
static constexpr int EXIT_INPUT = 7;

static constexpr int DEFAULT_DELAY_MS = 20;
static constexpr int DEFAULT_LINE_DELAY_MS = 100;
static constexpr int DEFAULT_START_DELAY_SEC = 5;
static constexpr int DEFAULT_MAX_BYTES = 1024 * 1024;
static constexpr int ABSOLUTE_MAX_BYTES = 100 * 1024 * 1024;
static constexpr int DEFAULT_HEX_CHUNK_BYTES = 240;
static constexpr int MAX_HEX_CHUNK_BYTES = 2048;
static constexpr size_t MAX_DIRECTORY_ENTRIES = 10000;
static constexpr const char *REMOTE_HEX = "tt.hex";
static constexpr const char *REMOTE_ZIP = "tt.zip";
static constexpr const char *REMOTE_STAGE = "tt.out";
static constexpr const char *REMOTE_HELPER_HEX = "tt.cmd.hex";
static constexpr const char *REMOTE_HELPER = "tt.cmd";

enum class SourceKind {
    Clipboard,
    File,
};

enum class EnterMode {
    Key,
    Unicode,
};

enum class InputMode {
    Auto,
    Keys,
    Unicode,
};

enum class TransferMode {
    Simple,
    CmdHex,
    ZipHex,
};

enum class OutputEncoding {
    Utf8,
    Utf8Bom,
    Preserve,
};

enum class DataKind {
    Text,
    FileBytes,
    Directory,
};

struct Options {
    int delay_ms = DEFAULT_DELAY_MS;
    int line_delay_ms = DEFAULT_LINE_DELAY_MS;
    int start_delay_sec = DEFAULT_START_DELAY_SEC;
    int max_bytes = DEFAULT_MAX_BYTES;
    bool ascii_only = false;
    bool dry_run = false;
    bool diagnose = false;
    bool self_test = false;
    bool debug_input = false;
    bool no_focus_check = false;
    bool request_accessibility = false;
    SourceKind source = SourceKind::Clipboard;
    EnterMode enter_mode = EnterMode::Key;
    InputMode input_mode = InputMode::Auto;
    TransferMode transfer_mode = TransferMode::Simple;
    OutputEncoding output_encoding = OutputEncoding::Utf8;
    std::filesystem::path file_path;
    std::filesystem::path commands_out;
    std::string remote_output = ".\\trans.txt";
    bool remote_output_set = false;
    int hex_chunk_bytes = DEFAULT_HEX_CHUNK_BYTES;
};

struct TextData {
    std::u16string text;
    size_t byte_count = 0;
    std::string encoding;
    std::string source_label;
    std::vector<unsigned char> raw_bytes;
    DataKind kind = DataKind::Text;
    std::filesystem::path source_path;
    size_t file_count = 0;
    size_t directory_count = 0;
};

struct TextStats {
    size_t lines = 1;
    size_t scalar_count = 0;
    size_t non_ascii_count = 0;
    size_t control_count = 0;
    size_t surrogate_error_count = 0;
    size_t first_non_ascii = 0;
    size_t first_control = 0;
    size_t first_surrogate_error = 0;
};

struct FrontApp {
    pid_t pid = 0;
    std::string name;
    CGWindowID window_id = 0;
    std::string window_title;
};

static bool simple_ascii_key_supported(char16_t ch);

static void print_help_option(const char *option, const char *description) {
    printf("  %-25s%s\n", option, description);
}

static void print_usage() {
    puts(PROGRAM_NAME " - 通过模拟键盘输入文本或文件传输命令");
    puts("Type text or file-transfer commands into the foreground macOS window.");
    puts("");
    puts("用法 / usage:");
    puts("  " PROGRAM_NAME " [options]");
    puts("");
    puts("选项 / options:");
    print_help_option("--source SOURCE", "来源：clipboard、file(trans.txt)或本地路径 / source (default: clipboard)");
    print_help_option("--mode MODE", "传输模式 / simple, cmd-hex, or zip-hex (default: simple)");
    print_help_option("--remote-output TARGET", "远端名称或路径 / name or relative/absolute path (default: ./basename)");
    print_help_option("--output-encoding E", "输出编码 / utf8, utf8-bom, or preserve (default: utf8)");
    print_help_option("--commands-out PATH", "导出复杂模式命令 / export command stream (default: unset)");
    print_help_option("--hex-chunk-bytes N", "每行 hex 原始字节数 / bytes per hex line (default: 240)");
    print_help_option("--delay-ms N", "每个字符延迟 / delay per character (0..5000, default: 20)");
    print_help_option("--line-delay-ms N", "每行延迟 / delay after each line (0..5000, default: 100)");
    print_help_option("--start-delay-sec N", "开始前倒计时 / countdown before typing (default: 5)");
    print_help_option("--max-bytes N", "文件字节上限 / source byte limit (default: 1048576)");
    print_help_option("--ascii-only", "拒绝非 ASCII 文本 / reject non-ASCII text (default: off)");
    print_help_option("--input-mode MODE", "按键方式 / auto, keys, or unicode (default: auto)");
    print_help_option("--enter-mode MODE", "回车方式 / key or unicode (default: key)");
    print_help_option("--no-focus-check", "关闭前台窗口检查 / disable focus check (default: check on)");
    print_help_option("--request-accessibility", "请求辅助功能权限 / request permission (default: off)");
    print_help_option("--dry-run", "只验证，不输入 / validate without typing (default: off)");
    print_help_option("--diagnose", "显示来源、窗口和权限 / report status (default: off)");
    print_help_option("--self-test", "发送无害 F20 测试键 / harmless F20 test (default: off)");
    print_help_option("--debug-input", "可见输入诊断 / visible diagnostics (default: off)");
    print_help_option("-h, --help", "显示帮助并退出 / show help and exit");
    puts("");
    puts("使用方法和示例 / usage and examples:");
    puts("  simple 文本/text:");
    puts("    " PROGRAM_NAME " --mode simple [--source clipboard|file|PATH]");
    puts("  cmd-hex 文本/text:");
    puts("    " PROGRAM_NAME " --mode cmd-hex --source SOURCE [--remote-output TARGET]");
    puts("      [--output-encoding utf8|utf8-bom]");
    puts("  cmd-hex 单文件/raw file:");
    puts("    " PROGRAM_NAME " --mode cmd-hex --source PATH --output-encoding preserve");
    puts("      [--remote-output TARGET]");
    puts("  zip-hex 文本/compressed text:");
    puts("    " PROGRAM_NAME " --mode zip-hex --source SOURCE [--remote-output TARGET]");
    puts("      [--output-encoding utf8|utf8-bom]");
    puts("  zip-hex 单文件/raw file:");
    puts("    " PROGRAM_NAME " --mode zip-hex --source PATH --output-encoding preserve");
    puts("      [--remote-output TARGET]");
    puts("  zip-hex 目录/folder:");
    puts("    " PROGRAM_NAME " --mode zip-hex --source FOLDER [--remote-output TARGET]");
    puts("  只导出命令/export only:");
    puts("    " PROGRAM_NAME " --mode cmd-hex --dry-run --commands-out commands.txt");
    puts("");
    puts("运行控制 / runtime controls:");
    puts("  Ctrl-C  立即中止 / abort immediately");
    puts("  Esc    中止 / abort");
    puts("  Space  暂停或继续 / pause or resume");
}

static bool parse_int_range(const char *value, int min_value, int max_value, int *out) {
    if (value == nullptr || *value == '\0') {
        return false;
    }
    char *end = nullptr;
    long parsed = strtol(value, &end, 10);
    if (*end != '\0' || parsed < min_value || parsed > max_value) {
        return false;
    }
    *out = static_cast<int>(parsed);
    return true;
}

static int get_option_value(int *i, int argc, char **argv, const char *name, const char **value) {
    size_t len = strlen(name);
    if (strncmp(argv[*i], name, len) == 0 && argv[*i][len] == '=') {
        *value = argv[*i] + len + 1;
        return 1;
    }
    if (strcmp(argv[*i], name) == 0) {
        if (*i + 1 >= argc) {
            fprintf(stderr, "Missing value for %s\n", name);
            return 0;
        }
        ++(*i);
        *value = argv[*i];
        return 1;
    }
    return -1;
}

static bool parse_args(int argc, char **argv, Options *opt) {
    int source_option_kind = 0;
    for (int i = 1; i < argc; ++i) {
        const char *value = nullptr;
        int matched = get_option_value(&i, argc, argv, "--delay-ms", &value);
        if (matched == 1) {
            if (!parse_int_range(value, 0, 5000, &opt->delay_ms)) {
                fprintf(stderr, "Invalid --delay-ms value: %s\n", value);
                return false;
            }
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--line-delay-ms", &value);
        if (matched == 1) {
            if (!parse_int_range(value, 0, 5000, &opt->line_delay_ms)) {
                fprintf(stderr, "Invalid --line-delay-ms value: %s\n", value);
                return false;
            }
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--start-delay-sec", &value);
        if (matched == 1) {
            if (!parse_int_range(value, 0, 3600, &opt->start_delay_sec)) {
                fprintf(stderr, "Invalid --start-delay-sec value: %s\n", value);
                return false;
            }
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--max-bytes", &value);
        if (matched == 1) {
            if (!parse_int_range(value, 1, ABSOLUTE_MAX_BYTES, &opt->max_bytes)) {
                fprintf(stderr, "Invalid --max-bytes value: %s\n", value);
                return false;
            }
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--hex-chunk-bytes", &value);
        if (matched == 1) {
            if (!parse_int_range(value, 1, MAX_HEX_CHUNK_BYTES, &opt->hex_chunk_bytes)) {
                fprintf(stderr, "Invalid --hex-chunk-bytes value: %s\n", value);
                return false;
            }
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--source", &value);
        if (matched == 1) {
            if (source_option_kind != 0) {
                fprintf(stderr, "--source may only be specified once.\n");
                return false;
            }
            if (strcmp(value, "clipboard") == 0) {
                opt->source = SourceKind::Clipboard;
                source_option_kind = 2;
            } else if (strcmp(value, "file") == 0) {
                opt->source = SourceKind::File;
                source_option_kind = 1;
            } else {
                opt->source = SourceKind::File;
                opt->file_path = std::filesystem::u8path(value);
                source_option_kind = 3;
            }
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--enter-mode", &value);
        if (matched == 1) {
            if (strcmp(value, "key") == 0) {
                opt->enter_mode = EnterMode::Key;
            } else if (strcmp(value, "unicode") == 0) {
                opt->enter_mode = EnterMode::Unicode;
            } else {
                fprintf(stderr, "Invalid --enter-mode value: %s\n", value);
                return false;
            }
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--mode", &value);
        if (matched == 1) {
            if (strcmp(value, "simple") == 0) {
                opt->transfer_mode = TransferMode::Simple;
            } else if (strcmp(value, "cmd-hex") == 0 || strcmp(value, "complex") == 0 || strcmp(value, "cmd") == 0) {
                opt->transfer_mode = TransferMode::CmdHex;
            } else if (strcmp(value, "zip-hex") == 0 || strcmp(value, "zip") == 0) {
                opt->transfer_mode = TransferMode::ZipHex;
            } else {
                fprintf(stderr, "Invalid --mode value: %s\n", value);
                return false;
            }
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--remote-output", &value);
        if (matched == 1) {
            opt->remote_output = value;
            opt->remote_output_set = true;
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--output-encoding", &value);
        if (matched == 1) {
            if (strcmp(value, "utf8") == 0) {
                opt->output_encoding = OutputEncoding::Utf8;
            } else if (strcmp(value, "utf8-bom") == 0) {
                opt->output_encoding = OutputEncoding::Utf8Bom;
            } else if (strcmp(value, "preserve") == 0) {
                opt->output_encoding = OutputEncoding::Preserve;
            } else {
                fprintf(stderr, "Invalid --output-encoding value: %s\n", value);
                return false;
            }
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--commands-out", &value);
        if (matched == 1) {
            opt->commands_out = value;
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--input-mode", &value);
        if (matched == 1) {
            if (strcmp(value, "auto") == 0) {
                opt->input_mode = InputMode::Auto;
            } else if (strcmp(value, "keys") == 0) {
                opt->input_mode = InputMode::Keys;
            } else if (strcmp(value, "unicode") == 0) {
                opt->input_mode = InputMode::Unicode;
            } else {
                fprintf(stderr, "Invalid --input-mode value: %s\n", value);
                return false;
            }
            continue;
        }
        if (matched == 0) {
            return false;
        }

        if (strcmp(argv[i], "--ascii-only") == 0) {
            opt->ascii_only = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            opt->dry_run = true;
        } else if (strcmp(argv[i], "--diagnose") == 0) {
            opt->diagnose = true;
        } else if (strcmp(argv[i], "--self-test") == 0) {
            opt->self_test = true;
        } else if (strcmp(argv[i], "--debug-input") == 0) {
            opt->debug_input = true;
        } else if (strcmp(argv[i], "--no-focus-check") == 0) {
            opt->no_focus_check = true;
        } else if (strcmp(argv[i], "--request-accessibility") == 0) {
            opt->request_accessibility = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            exit(EXIT_OK);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return false;
        }
    }
    if (opt->output_encoding == OutputEncoding::Preserve && opt->transfer_mode == TransferMode::Simple) {
        fprintf(stderr, "--output-encoding preserve is only valid with --mode cmd-hex or --mode zip-hex.\n");
        return false;
    }
    if (opt->output_encoding == OutputEncoding::Preserve && opt->source == SourceKind::Clipboard) {
        fprintf(stderr, "--output-encoding preserve requires a file source, not clipboard text.\n");
        return false;
    }
    if (opt->transfer_mode == TransferMode::Simple && opt->remote_output_set) {
        fprintf(stderr, "--remote-output is only valid with cmd-hex or zip-hex.\n");
        return false;
    }
    if (opt->transfer_mode == TransferMode::Simple && !opt->commands_out.empty()) {
        fprintf(stderr, "--commands-out is only valid with --mode cmd-hex or --mode zip-hex.\n");
        return false;
    }
    return true;
}

static std::filesystem::path executable_dir() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::filesystem::current_path();
    }
    std::error_code ec;
    auto path = std::filesystem::weakly_canonical(buffer.data(), ec);
    if (ec) {
        path = buffer.data();
    }
    return path.parent_path();
}

static std::u16string nsstring_to_u16(NSString *value) {
    NSUInteger len = [value length];
    std::vector<unichar> chars(len);
    if (len > 0) {
        [value getCharacters:chars.data() range:NSMakeRange(0, len)];
    }
    std::u16string out;
    out.resize(len);
    for (NSUInteger i = 0; i < len; ++i) {
        out[i] = static_cast<char16_t>(chars[i]);
    }
    return out;
}

static std::string nsstring_to_utf8(NSString *value) {
    const char *utf8 = [value UTF8String];
    return utf8 ? std::string(utf8) : std::string();
}

static void strip_bom(std::u16string *text) {
    if (!text->empty() && (*text)[0] == 0xFEFF) {
        text->erase(text->begin());
    }
}

static bool decode_data(NSData *data, TextData *out, std::string *error) {
    if ([data length] > static_cast<NSUInteger>(ABSOLUTE_MAX_BYTES)) {
        *error = "Input is larger than the absolute maximum size.";
        return false;
    }

    NSString *decoded = nil;
    std::string encoding;
    const unsigned char *bytes = static_cast<const unsigned char *>([data bytes]);
    NSUInteger len = [data length];

    if (len >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        decoded = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        encoding = "UTF-8 BOM";
    } else if (len >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        decoded = [[NSString alloc] initWithData:data encoding:NSUTF16LittleEndianStringEncoding];
        encoding = "UTF-16 LE BOM";
    } else if (len >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        decoded = [[NSString alloc] initWithData:data encoding:NSUTF16BigEndianStringEncoding];
        encoding = "UTF-16 BE BOM";
    } else {
        decoded = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
        encoding = "UTF-8";
        if (decoded == nil) {
            decoded = [[NSString alloc] initWithData:data encoding:NSWindowsCP1252StringEncoding];
            encoding = "Windows-1252 fallback";
        }
    }

    if (decoded == nil) {
        *error = "Input could not be decoded as UTF-8, UTF-16 with BOM, or Windows-1252.";
        return false;
    }

    out->text = nsstring_to_u16(decoded);
    strip_bom(&out->text);
    out->byte_count = [data length];
    out->encoding = encoding;
    return true;
}

static bool scan_directory_source(const std::filesystem::path &path, const Options &opt,
                                  TextData *out, std::string *error) {
    std::error_code ec;
    auto root_status = std::filesystem::symlink_status(path, ec);
    if (ec || std::filesystem::is_symlink(root_status)) {
        *error = "Directory source cannot be a symbolic link.";
        return false;
    }

    size_t entries = 0;
    uintmax_t total_bytes = 0;
    std::filesystem::recursive_directory_iterator iterator(path, ec);
    std::filesystem::recursive_directory_iterator end;
    if (ec) {
        *error = "Cannot enumerate source directory: " + path.string();
        return false;
    }
    while (iterator != end) {
        const auto entry_path = iterator->path();
        auto entry_status = iterator->symlink_status(ec);
        if (ec) {
            *error = "Cannot inspect directory entry: " + entry_path.string();
            return false;
        }
        if (std::filesystem::is_symlink(entry_status)) {
            *error = "Directory source contains a symbolic link: " + entry_path.string();
            return false;
        }
        if (std::filesystem::is_directory(entry_status)) {
            ++out->directory_count;
        } else if (std::filesystem::is_regular_file(entry_status)) {
            uintmax_t size = iterator->file_size(ec);
            if (ec) {
                *error = "Cannot determine directory file size: " + entry_path.string();
                return false;
            }
            total_bytes += size;
            if (total_bytes > static_cast<uintmax_t>(opt.max_bytes)) {
                *error = "Directory file data is larger than --max-bytes.";
                return false;
            }
            ++out->file_count;
        } else {
            *error = "Directory source contains an unsupported special file: " + entry_path.string();
            return false;
        }
        if (++entries > MAX_DIRECTORY_ENTRIES) {
            *error = "Directory contains more than 10000 entries.";
            return false;
        }
        iterator.increment(ec);
        if (ec) {
            *error = "Cannot continue enumerating source directory: " + path.string();
            return false;
        }
    }

    out->kind = DataKind::Directory;
    out->source_path = path;
    out->byte_count = static_cast<size_t>(total_bytes);
    out->encoding = "directory tree (file bytes preserved)";
    out->source_label = "Directory: " + path.string();
    return true;
}

static bool read_file_source(const Options &opt, TextData *out, std::string *error) {
    std::filesystem::path path = opt.file_path.empty() ? executable_dir() / "trans.txt" : opt.file_path;
    std::error_code status_error;
    auto link_status = std::filesystem::symlink_status(path, status_error);
    if (status_error) {
        *error = "Cannot inspect input path: " + path.string();
        return false;
    }
    if (std::filesystem::is_symlink(link_status)) {
        *error = "Input path cannot be a symbolic link.";
        return false;
    }
    if (std::filesystem::is_directory(link_status)) {
        if (opt.transfer_mode != TransferMode::ZipHex) {
            *error = "Directory sources are only supported with --mode zip-hex.";
            return false;
        }
        return scan_directory_source(path, opt, out, error);
    }
    if (!std::filesystem::is_regular_file(link_status)) {
        *error = "Input path must be a regular file, or a real directory in zip-hex mode.";
        return false;
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        *error = "Cannot open input file: " + path.string();
        return false;
    }
    file.seekg(0, std::ios::end);
    std::streamoff size = file.tellg();
    if (size < 0) {
        *error = "Cannot determine input file size: " + path.string();
        return false;
    }
    if (size > opt.max_bytes) {
        *error = "Input file is larger than --max-bytes.";
        return false;
    }
    file.seekg(0, std::ios::beg);
    std::vector<char> bytes(static_cast<size_t>(size));
    if (!bytes.empty()) {
        file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    if (!file && !bytes.empty()) {
        *error = "Cannot read input file: " + path.string();
        return false;
    }

    bool preserve_file = opt.transfer_mode != TransferMode::Simple &&
                         opt.output_encoding == OutputEncoding::Preserve;
    if (preserve_file) {
        out->raw_bytes.assign(bytes.begin(), bytes.end());
        out->byte_count = bytes.size();
        out->encoding = "raw file bytes (preserved)";
        out->source_label = "File: " + path.string();
        out->source_path = path;
        out->kind = DataKind::FileBytes;
        return true;
    }

    NSData *data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    if (!decode_data(data, out, error)) {
        return false;
    }
    out->raw_bytes.assign(bytes.begin(), bytes.end());
    out->source_label = "File: " + path.string();
    out->source_path = path;
    return true;
}

static bool read_clipboard_source(const Options &opt, TextData *out, std::string *error) {
    NSPasteboard *pasteboard = [NSPasteboard generalPasteboard];
    NSString *text = [pasteboard stringForType:NSPasteboardTypeString];
    if (text == nil) {
        *error = "The macOS clipboard does not currently contain text.";
        return false;
    }

    NSData *utf8 = [text dataUsingEncoding:NSUTF8StringEncoding];
    NSUInteger byte_count = [utf8 length];
    if (byte_count > static_cast<NSUInteger>(opt.max_bytes)) {
        *error = "Clipboard text is larger than --max-bytes.";
        return false;
    }

    out->text = nsstring_to_u16(text);
    strip_bom(&out->text);
    out->byte_count = byte_count;
    out->encoding = "macOS clipboard Unicode string";
    out->source_label = "Clipboard";
    return true;
}

static bool read_text_source(const Options &opt, TextData *out, std::string *error) {
    if (opt.source == SourceKind::Clipboard) {
        return read_clipboard_source(opt, out, error);
    }
    return read_file_source(opt, out, error);
}

static bool remote_separator(char ch) {
    return ch == '/' || ch == '\\';
}

static std::string collapse_remote_separators(const std::string &path) {
    bool unc = path.size() >= 2 && remote_separator(path[0]) && remote_separator(path[1]);
    std::string out;
    out.reserve(path.size());
    for (char ch : path) {
        char converted = remote_separator(ch) ? '\\' : ch;
        if (converted == '\\' && !out.empty() && out.back() == '\\') {
            continue;
        }
        out.push_back(converted);
    }
    if (unc && !out.empty() && out.front() == '\\') {
        out.insert(out.begin(), '\\');
    }
    return out;
}

static bool normalize_remote_output(const std::string *value, const std::string &source_name,
                                    std::string *out) {
    if (source_name.empty()) {
        return false;
    }
    if (value == nullptr) {
        *out = ".\\" + source_name;
        return true;
    }
    if (value->empty()) {
        return false;
    }

    bool directory_target = remote_separator(value->back()) || *value == "." || *value == "..";
    *out = collapse_remote_separators(*value);
    if (!directory_target) {
        if (out->find('\\') == std::string::npos && out->find(':') == std::string::npos) {
            *out = ".\\" + *out;
        }
        return !out->empty();
    }
    while (!out->empty() && out->back() == '\\') {
        out->pop_back();
    }
    if (out->empty()) {
        *out = "\\" + source_name;
    } else {
        *out += "\\" + source_name;
    }
    return true;
}

static std::pair<std::string, std::string> remote_output_parts(const std::string &target) {
    size_t split = target.rfind('\\');
    if (split == std::string::npos) {
        return {".", target};
    }
    std::string name = target.substr(split + 1);
    if (split == 0) {
        return {"\\", name};
    }
    if (split == 2 && target.size() >= 3 && target[1] == ':') {
        return {target.substr(0, 3), name};
    }
    return {target.substr(0, split), name};
}

static std::string ascii_lower(std::string value) {
    for (char &ch : value) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    return value;
}

static bool remote_component_is_reserved(const std::string &component) {
    std::string base = ascii_lower(component.substr(0, component.find('.')));
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") {
        return true;
    }
    return base.size() == 4 && (base.compare(0, 3, "com") == 0 || base.compare(0, 3, "lpt") == 0) &&
           base[3] >= '1' && base[3] <= '9';
}

static std::vector<std::string> remote_components(const std::string &target, size_t start) {
    std::vector<std::string> components;
    for (;;) {
        size_t end = target.find('\\', start);
        components.push_back(target.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return components;
}

static std::string remote_output_error(const std::string &target) {
    if (target.empty()) {
        return "--remote-output must not be empty";
    }
    NSString *decoded = [[NSString alloc] initWithBytes:target.data()
                                                 length:target.size()
                                               encoding:NSUTF8StringEncoding];
    if (decoded == nil) {
        return "--remote-output contains invalid Unicode";
    }
    if ([decoded length] > 240) {
        return "--remote-output is longer than 240 UTF-16 code units";
    }
    for (unsigned char ch : target) {
        if (ch < 32) {
            return "--remote-output contains a control character";
        }
    }

    bool unc = target.size() >= 2 && target[0] == '\\' && target[1] == '\\';
    bool ascii_drive = !target.empty() &&
                       ((target[0] >= 'a' && target[0] <= 'z') ||
                        (target[0] >= 'A' && target[0] <= 'Z'));
    bool drive_absolute = target.size() >= 3 && ascii_drive && target[1] == ':' && target[2] == '\\';
    if (target.size() >= 2 && target[1] == ':' && !drive_absolute) {
        return "--remote-output drive paths must be absolute, for example c:/work/file.txt";
    }
    size_t colon_start = drive_absolute ? 3 : 0;
    if (target.find(':', colon_start) != std::string::npos) {
        return "--remote-output contains ':' outside a drive prefix";
    }

    size_t component_start = unc ? 2 : (drive_absolute ? 3 : (!target.empty() && target[0] == '\\' ? 1 : 0));
    std::vector<std::string> components = remote_components(target, component_start);
    if (unc) {
        if (components.size() < 3 || components[0].empty() || components[1].empty()) {
            return "--remote-output UNC paths must include server, share, and output name";
        }
        for (const std::string &component : components) {
            if (component == "." || component == "..") {
                return "--remote-output UNC paths cannot contain '.' or '..' segments";
            }
        }
    }

    for (const std::string &component : components) {
        if (component == "." || component == "..") {
            continue;
        }
        if (component.empty()) {
            return "--remote-output contains an empty path component";
        }
        if (component.back() == '.' || component.back() == ' ') {
            return "--remote-output components cannot end with a dot or space";
        }
        if (component.find_first_of("<>\"|?*") != std::string::npos) {
            return "--remote-output contains a Windows-invalid path character";
        }
        if (remote_component_is_reserved(component)) {
            return "--remote-output contains a reserved Windows device name";
        }
        std::string folded = ascii_lower(component);
        if (folded == REMOTE_HEX || folded == REMOTE_ZIP || folded == REMOTE_STAGE ||
            folded == REMOTE_HELPER_HEX || folded == REMOTE_HELPER) {
            return "--remote-output uses a reserved temporary name";
        }
    }

    std::string name = remote_output_parts(target).second;
    if (name.empty() || name == "." || name == "..") {
        return "--remote-output must end with a file or directory name";
    }
    return {};
}

static bool remote_output_is_shift_free(const std::string &target) {
    for (unsigned char ch : target) {
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
            ch == '.' || ch == '-' || ch == '\\') {
            continue;
        }
        return false;
    }
    return true;
}

static bool apply_default_remote_output(Options *opt, const TextData &data, std::string *error) {
    std::string source_name = opt->source == SourceKind::Clipboard
                                  ? "trans.txt"
                                  : data.source_path.filename().u8string();
    if (source_name.empty()) {
        *error = "Cannot derive --remote-output from the source path; specify --remote-output TARGET.";
        return false;
    }
    std::string requested = opt->remote_output;
    if (!normalize_remote_output(opt->remote_output_set ? &requested : nullptr, source_name,
                                 &opt->remote_output)) {
        *error = "--remote-output is empty or cannot be normalized.";
        return false;
    }
    if (opt->transfer_mode != TransferMode::Simple) {
        *error = remote_output_error(opt->remote_output);
        if (!error->empty()) {
            return false;
        }
    }
    return true;
}

static std::vector<unsigned char> text_to_utf8_bytes(const std::u16string &text) {
    NSString *value = [[NSString alloc] initWithCharacters:reinterpret_cast<const unichar *>(text.data()) length:text.size()];
    NSData *data = [value dataUsingEncoding:NSUTF8StringEncoding];
    std::vector<unsigned char> out([data length]);
    if (!out.empty()) {
        memcpy(out.data(), [data bytes], out.size());
    }
    return out;
}

static const char *output_encoding_name(OutputEncoding encoding) {
    switch (encoding) {
        case OutputEncoding::Utf8Bom:
            return "utf8-bom";
        case OutputEncoding::Preserve:
            return "preserve";
        case OutputEncoding::Utf8:
            return "utf8";
    }
    return "utf8";
}

static bool make_output_bytes(const TextData &data, const Options &opt,
                              std::vector<unsigned char> *out, std::string *error) {
    if (opt.output_encoding == OutputEncoding::Preserve) {
        if (data.kind != DataKind::FileBytes) {
            *error = "--output-encoding preserve requires file input.";
            return false;
        }
        *out = data.raw_bytes;
        return true;
    }
    *out = text_to_utf8_bytes(data.text);
    if (out->empty()) {
        *error = "Input text encoded to empty UTF-8 data.";
        return false;
    }
    if (opt.output_encoding == OutputEncoding::Utf8Bom) {
        out->insert(out->begin(), {0xEF, 0xBB, 0xBF});
    }
    return true;
}

static std::string sha256_hex(const std::vector<unsigned char> &bytes) {
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    static const char hex[] = "0123456789abcdef";
    CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest);
    std::string out(CC_SHA256_DIGEST_LENGTH * 2, '0');
    for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    return out;
}

static std::string bytes_to_plain_hex(const std::vector<unsigned char> &bytes, int chunk_bytes) {
    std::string out;
    char chunk[3];
    for (size_t i = 0; i < bytes.size(); ++i) {
        snprintf(chunk, sizeof(chunk), "%02x", static_cast<unsigned int>(bytes[i]));
        out += chunk;
        if ((i + 1) % static_cast<size_t>(chunk_bytes) == 0) {
            out += '\n';
        }
    }
    if (out.empty() || out.back() != '\n') {
        out += '\n';
    }
    return out;
}

static size_t count_hex_lines(const std::string &hex_text) {
    size_t lines = 0;
    bool has_data = false;
    for (char ch : hex_text) {
        if (ch == '\n') {
            if (has_data) {
                ++lines;
                has_data = false;
            }
        } else {
            has_data = true;
        }
    }
    return lines + (has_data ? 1 : 0);
}

static std::string normalized_zip_entry_name(const std::string &remote_output) {
    return remote_output_parts(remote_output).second;
}

static std::filesystem::path create_temp_directory() {
    const char *base = getenv("TMPDIR");
    std::string templ = (base && *base) ? base : "/tmp";
    if (!templ.empty() && templ.back() != '/') {
        templ += '/';
    }
    templ += "trans-type-zip-XXXXXX";
    std::vector<char> buffer(templ.begin(), templ.end());
    buffer.push_back('\0');
    char *made = mkdtemp(buffer.data());
    if (made == nullptr) {
        return {};
    }
    return std::filesystem::path(made);
}

static bool read_binary_file(const std::filesystem::path &path, std::vector<unsigned char> *out, std::string *error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        *error = "Could not read generated zip file.";
        return false;
    }
    input.seekg(0, std::ios::end);
    std::streamoff size = input.tellg();
    if (size < 0) {
        *error = "Could not determine generated zip file size.";
        return false;
    }
    input.seekg(0, std::ios::beg);
    out->resize(static_cast<size_t>(size));
    if (!out->empty()) {
        input.read(reinterpret_cast<char *>(out->data()), static_cast<std::streamsize>(out->size()));
        if (!input) {
            *error = "Could not read all generated zip bytes.";
            return false;
        }
    }
    return true;
}

static uint16_t zip_u16(const std::vector<unsigned char> &bytes, size_t offset) {
    return static_cast<uint16_t>(bytes[offset]) |
           static_cast<uint16_t>(bytes[offset + 1] << 8);
}

static uint32_t zip_u32(const std::vector<unsigned char> &bytes, size_t offset) {
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 3]) << 24);
}

static void zip_set_u16(std::vector<unsigned char> *bytes, size_t offset, uint16_t value) {
    (*bytes)[offset] = static_cast<unsigned char>(value & 0xff);
    (*bytes)[offset + 1] = static_cast<unsigned char>((value >> 8) & 0xff);
}

static bool mark_zip_filenames_utf8(std::vector<unsigned char> *bytes, std::string *error) {
    if (bytes->size() < 22) {
        *error = "Generated directory ZIP is too short.";
        return false;
    }
    size_t search_start = bytes->size() > 65557 ? bytes->size() - 65557 : 0;
    size_t eocd = bytes->size() - 22;
    while (true) {
        if (zip_u32(*bytes, eocd) == 0x06054b50) {
            break;
        }
        if (eocd == search_start) {
            *error = "Generated directory ZIP has no end-of-central-directory record.";
            return false;
        }
        --eocd;
    }

    uint16_t entry_count = zip_u16(*bytes, eocd + 10);
    size_t central = zip_u32(*bytes, eocd + 16);
    for (uint16_t index = 0; index < entry_count; ++index) {
        if (central + 46 > bytes->size() || zip_u32(*bytes, central) != 0x02014b50) {
            *error = "Generated directory ZIP has an invalid central-directory entry.";
            return false;
        }
        uint16_t flags = zip_u16(*bytes, central + 8);
        zip_set_u16(bytes, central + 8, static_cast<uint16_t>(flags | 0x0800));
        size_t local = zip_u32(*bytes, central + 42);
        if (local + 30 > bytes->size() || zip_u32(*bytes, local) != 0x04034b50) {
            *error = "Generated directory ZIP has an invalid local entry.";
            return false;
        }
        uint16_t local_flags = zip_u16(*bytes, local + 6);
        zip_set_u16(bytes, local + 6, static_cast<uint16_t>(local_flags | 0x0800));
        central += 46 + zip_u16(*bytes, central + 28) + zip_u16(*bytes, central + 30) +
                   zip_u16(*bytes, central + 32);
    }
    return true;
}

static bool run_zip_tool(const std::filesystem::path &cwd,
                         const std::filesystem::path &zip_path,
                         const std::string &entry_name,
                         std::string *error) {
    @autoreleasepool {
        NSTask *task = [[NSTask alloc] init];
        task.executableURL = [NSURL fileURLWithPath:@"/usr/bin/zip"];
        task.currentDirectoryURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:cwd.string().c_str()] isDirectory:YES];
        task.arguments = @[
            @"-q",
            @"-r",
            [NSString stringWithUTF8String:zip_path.string().c_str()],
            @"--",
            [NSString stringWithUTF8String:entry_name.c_str()]
        ];

        NSPipe *pipe = [NSPipe pipe];
        task.standardOutput = pipe;
        task.standardError = pipe;

        NSError *launch_error = nil;
        if (![task launchAndReturnError:&launch_error]) {
            NSString *message = launch_error ? [launch_error localizedDescription] : @"unknown launch error";
            *error = std::string("/usr/bin/zip failed to start: ") + [message UTF8String];
            return false;
        }
        [task waitUntilExit];
        if (task.terminationStatus != 0) {
            NSData *data = [[pipe fileHandleForReading] readDataToEndOfFile];
            NSString *output = [[NSString alloc] initWithData:data encoding:NSUTF8StringEncoding];
            std::string details = output ? [output UTF8String] : "";
            *error = "/usr/bin/zip failed while creating the transfer archive.";
            if (!details.empty()) {
                *error += " ";
                *error += details;
            }
            return false;
        }
    }
    return true;
}

static bool make_zip_payload(const std::vector<unsigned char> &bytes,
                             const std::string &remote_output,
                             std::vector<unsigned char> *zip_bytes,
                             std::string *error) {
    std::filesystem::path temp_root;
    try {
        temp_root = create_temp_directory();
        if (temp_root.empty()) {
            *error = "Could not create a temporary directory for zip compression.";
            return false;
        }

        std::string entry_name = normalized_zip_entry_name(remote_output);
        std::filesystem::path entry_path = temp_root / std::filesystem::path(entry_name);
        if (entry_path.has_parent_path()) {
            std::filesystem::create_directories(entry_path.parent_path());
        }

        {
            std::ofstream output(entry_path, std::ios::binary);
            if (!output) {
                *error = "Could not create temporary input file for zip compression.";
                std::error_code ignored;
                std::filesystem::remove_all(temp_root, ignored);
                return false;
            }
            if (!bytes.empty()) {
                output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            }
        }

        std::filesystem::path zip_path = temp_root / "tt.zip";
        if (!run_zip_tool(temp_root, zip_path, entry_name, error)) {
            std::error_code ignored;
            std::filesystem::remove_all(temp_root, ignored);
            return false;
        }
        bool ok = read_binary_file(zip_path, zip_bytes, error);
        if (ok) {
            ok = mark_zip_filenames_utf8(zip_bytes, error);
        }
        std::error_code ignored;
        std::filesystem::remove_all(temp_root, ignored);
        return ok;
    } catch (const std::exception &ex) {
        if (!temp_root.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(temp_root, ignored);
        }
        *error = std::string("Zip compression failed: ") + ex.what();
        return false;
    }
}

static bool make_directory_zip_payload(const TextData &data, const Options &opt,
                                       std::vector<unsigned char> *zip_bytes,
                                       std::string *error) {
    TextData current;
    if (!scan_directory_source(data.source_path, opt, &current, error)) {
        return false;
    }
    if (current.byte_count != data.byte_count || current.file_count != data.file_count ||
        current.directory_count != data.directory_count) {
        *error = "Source directory changed after it was scanned; retry the transfer.";
        return false;
    }

    if (current.file_count == 0 && current.directory_count == 0) {
        static const unsigned char empty_zip[] = {
            0x50, 0x4b, 0x05, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        };
        zip_bytes->assign(std::begin(empty_zip), std::end(empty_zip));
        return true;
    }

    std::filesystem::path temp_root;
    try {
        temp_root = create_temp_directory();
        if (temp_root.empty()) {
            *error = "Could not create a temporary directory for folder compression.";
            return false;
        }
        std::filesystem::path zip_path = temp_root / "tt.zip";
        if (!run_zip_tool(data.source_path, zip_path, ".", error)) {
            std::error_code ignored;
            std::filesystem::remove_all(temp_root, ignored);
            return false;
        }
        bool ok = read_binary_file(zip_path, zip_bytes, error);
        if (ok) {
            ok = mark_zip_filenames_utf8(zip_bytes, error);
        }
        std::error_code ignored;
        std::filesystem::remove_all(temp_root, ignored);
        return ok;
    } catch (const std::exception &ex) {
        if (!temp_root.empty()) {
            std::error_code ignored;
            std::filesystem::remove_all(temp_root, ignored);
        }
        *error = std::string("Directory ZIP creation failed: ") + ex.what();
        return false;
    }
}

static TextData text_data_from_ascii(const std::string &text, const std::string &label) {
    TextData data;
    data.text.reserve(text.size());
    for (unsigned char ch : text) {
        data.text.push_back(static_cast<char16_t>(ch));
    }
    data.byte_count = text.size();
    data.encoding = "generated ASCII command stream";
    data.source_label = label;
    return data;
}

static std::string quote_powershell_literal(const std::string &value) {
    return "'" + value + "'";
}

static void append_hex_writer_commands(std::string *commands, const std::string &hex_text,
                                       const std::string &destination) {
    size_t start = 0;
    bool first_line = true;
    while (start < hex_text.size()) {
        size_t end = hex_text.find('\n', start);
        if (end == std::string::npos) {
            end = hex_text.size();
        }
        if (end > start) {
            *commands += first_line ? "set-content" : "add-content";
            *commands += " -encoding ascii ";
            *commands += quote_powershell_literal(destination);
            *commands += " '";
            commands->append(hex_text, start, end - start);
            *commands += "'\n";
            first_line = false;
        }
        start = end + 1;
    }
}

static std::string build_hex_writer_commands(const std::string &hex_text) {
    std::string commands = "powershell -noprofile\n";
    append_hex_writer_commands(&commands, hex_text, REMOTE_HEX);
    return commands;
}

static void append_remote_parent_command(std::string *commands, const std::string &target) {
    std::string parent = remote_output_parts(target).first;
    if (parent != ".") {
        *commands += "new-item -itemtype directory -force " + quote_powershell_literal(parent) + "\n";
    }
}

static std::string powershell_script_literal(const std::string &value) {
    std::string literal = "'";
    for (char ch : value) {
        if (ch == '\'') {
            literal += '\'';
        }
        literal += ch;
    }
    literal += '\'';
    return literal;
}

static std::vector<unsigned char> build_remote_helper(const Options &opt,
                                                       TransferMode transfer_mode,
                                                       bool directory_source = false) {
    std::string parent = remote_output_parts(opt.remote_output).first;
    std::string parent_literal = powershell_script_literal(parent);
    std::string target_literal = powershell_script_literal(opt.remote_output);
    std::string script = "[system.io.directory]::createdirectory(" + parent_literal + ");";
    if (transfer_mode == TransferMode::CmdHex) {
        script += "[system.io.file]::copy('" + std::string(REMOTE_STAGE) + "'," +
                  target_literal + ",$true);certutil -hashfile " + target_literal + " sha256";
    } else if (directory_source) {
        script += "certutil -hashfile '" + std::string(REMOTE_ZIP) + "' sha256;";
        script += "expand-archive -force -literalpath '" + std::string(REMOTE_ZIP) +
                  "' -destinationpath " + target_literal;
    } else {
        script += "expand-archive -force -literalpath '" + std::string(REMOTE_ZIP) +
                  "' -destinationpath " + parent_literal + ";";
        script += "certutil -hashfile " + target_literal + " sha256";
    }

    std::string escaped;
    escaped.reserve(script.size());
    for (char ch : script) {
        if (ch == '^' || ch == '%') {
            escaped += ch;
        }
        escaped += ch;
    }
    std::string batch = "@echo off\r\n"
                        "setlocal disabledelayedexpansion\r\n"
                        "chcp 65001 >nul\r\n"
                        "powershell -noprofile -noninteractive -command \"" +
                        escaped + "\"\r\nexit /b %errorlevel%\r\n";
    return {batch.begin(), batch.end()};
}

static void append_remote_helper_commands(std::string *commands, const Options &opt,
                                          TransferMode transfer_mode,
                                          bool directory_source = false) {
    std::vector<unsigned char> helper = build_remote_helper(opt, transfer_mode, directory_source);
    std::string helper_hex = bytes_to_plain_hex(helper, opt.hex_chunk_bytes);
    append_hex_writer_commands(commands, helper_hex, REMOTE_HELPER_HEX);
    *commands += "certutil -f -decodehex " + quote_powershell_literal(REMOTE_HELPER_HEX) + " " +
                 quote_powershell_literal(REMOTE_HELPER) + "\n";
    *commands += "cmd /d /c " + std::string(REMOTE_HELPER) + "\n";
}

static void append_cleanup_command(std::string *commands, const std::string &name) {
    *commands += "remove-item -force " + quote_powershell_literal(name) + "\n";
}

static std::string build_cmd_hex_commands(const std::string &hex_text, const Options &opt) {
    std::string commands = build_hex_writer_commands(hex_text);
    bool helper = !remote_output_is_shift_free(opt.remote_output);
    std::string decode_target = helper ? REMOTE_STAGE : opt.remote_output;
    if (!helper) {
        append_remote_parent_command(&commands, opt.remote_output);
    }
    if (count_hex_lines(hex_text) > 0) {
        commands += "certutil -f -decodehex " + quote_powershell_literal(REMOTE_HEX) + " " +
                    quote_powershell_literal(decode_target) + "\n";
    } else {
        commands += "set-content -nonewline " + quote_powershell_literal(REMOTE_HEX) + " ''\n";
        commands += "set-content -nonewline " + quote_powershell_literal(decode_target) + " ''\n";
    }
    if (helper) {
        append_remote_helper_commands(&commands, opt, TransferMode::CmdHex);
        append_cleanup_command(&commands, REMOTE_HEX);
        append_cleanup_command(&commands, REMOTE_STAGE);
        append_cleanup_command(&commands, REMOTE_HELPER_HEX);
        append_cleanup_command(&commands, REMOTE_HELPER);
    } else {
        commands += "certutil -hashfile " + quote_powershell_literal(opt.remote_output) + " sha256\n";
        append_cleanup_command(&commands, REMOTE_HEX);
    }
    commands += "exit\n";
    return commands;
}

static std::string build_zip_hex_commands(const std::string &hex_text, const Options &opt,
                                          bool directory_source = false) {
    std::string commands = build_hex_writer_commands(hex_text);
    bool helper = !remote_output_is_shift_free(opt.remote_output);
    if (!helper) {
        append_remote_parent_command(&commands, opt.remote_output);
    }
    commands += "certutil -f -decodehex " + quote_powershell_literal(REMOTE_HEX) + " " +
                quote_powershell_literal(REMOTE_ZIP) + "\n";
    if (helper) {
        append_remote_helper_commands(&commands, opt, TransferMode::ZipHex, directory_source);
    } else if (directory_source) {
        commands += "certutil -hashfile " + quote_powershell_literal(REMOTE_ZIP) + " sha256\n";
        commands += "expand-archive -force " + quote_powershell_literal(REMOTE_ZIP) + " " +
                    quote_powershell_literal(opt.remote_output) + "\n";
    } else {
        std::string parent = remote_output_parts(opt.remote_output).first;
        commands += "expand-archive -force " + quote_powershell_literal(REMOTE_ZIP) + " " +
                    quote_powershell_literal(parent) + "\n";
        commands += "certutil -hashfile " + quote_powershell_literal(opt.remote_output) + " sha256\n";
    }
    append_cleanup_command(&commands, REMOTE_HEX);
    append_cleanup_command(&commands, REMOTE_ZIP);
    if (helper) {
        append_cleanup_command(&commands, REMOTE_HELPER_HEX);
        append_cleanup_command(&commands, REMOTE_HELPER);
    }
    commands += "exit\n";
    return commands;
}

static bool generated_command_char_allowed(unsigned char ch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
        return true;
    }
    return ch == ' ' || ch == '\r' || ch == '\n' || ch == '-' || ch == '.' ||
           ch == '/' || ch == '\\' || ch == '\'';
}

static int prepare_generated_commands(const std::string &commands, const Options &opt) {
    size_t line = 1;
    size_t col = 1;
    for (unsigned char ch : commands) {
        if (!generated_command_char_allowed(ch)) {
            fprintf(stderr, "Generated command stream contains forbidden character U+%04X at line %zu, column %zu.\n",
                    static_cast<unsigned int>(ch), line, col);
            return EXIT_CONTENT;
        }
        if (ch == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
    }
    if (!opt.commands_out.empty()) {
        std::ofstream output(opt.commands_out, std::ios::binary | std::ios::trunc);
        if (!output) {
            fprintf(stderr, "Cannot create --commands-out file: %s\n", opt.commands_out.string().c_str());
            return EXIT_FILE;
        }
        output.write(commands.data(), static_cast<std::streamsize>(commands.size()));
        if (!output) {
            fprintf(stderr, "Cannot write --commands-out file: %s\n", opt.commands_out.string().c_str());
            return EXIT_FILE;
        }
        printf("Generated commands written to: %s\n", opt.commands_out.string().c_str());
    }
    return EXIT_OK;
}

static bool is_high_surrogate(char16_t ch) {
    return ch >= 0xD800 && ch <= 0xDBFF;
}

static bool is_low_surrogate(char16_t ch) {
    return ch >= 0xDC00 && ch <= 0xDFFF;
}

static bool next_scalar(const std::u16string &text, size_t index, uint32_t *scalar, size_t *units, bool *invalid) {
    *invalid = false;
    char16_t ch = text[index];
    if (is_high_surrogate(ch)) {
        if (index + 1 < text.size() && is_low_surrogate(text[index + 1])) {
            *scalar = 0x10000 + (((static_cast<uint32_t>(ch) - 0xD800) << 10) | (static_cast<uint32_t>(text[index + 1]) - 0xDC00));
            *units = 2;
            return true;
        }
        *scalar = ch;
        *units = 1;
        *invalid = true;
        return true;
    }
    if (is_low_surrogate(ch)) {
        *scalar = ch;
        *units = 1;
        *invalid = true;
        return true;
    }
    *scalar = ch;
    *units = 1;
    return true;
}

static TextStats analyze_text(const std::u16string &text) {
    TextStats stats;
    if (text.empty()) {
        stats.lines = 0;
        return stats;
    }

    for (size_t i = 0; i < text.size();) {
        uint32_t scalar = 0;
        size_t units = 1;
        bool invalid = false;
        next_scalar(text, i, &scalar, &units, &invalid);

        if (invalid) {
            if (stats.surrogate_error_count == 0) {
                stats.first_surrogate_error = i;
            }
            ++stats.surrogate_error_count;
        }
        if (scalar > 0x7F) {
            if (stats.non_ascii_count == 0) {
                stats.first_non_ascii = i;
            }
            ++stats.non_ascii_count;
        }
        if (scalar < 0x20 && scalar != '\t' && scalar != '\n' && scalar != '\r') {
            if (stats.control_count == 0) {
                stats.first_control = i;
            }
            ++stats.control_count;
        }
        if (scalar == '\n') {
            ++stats.lines;
        } else if (scalar == '\r') {
            if (!(i + 1 < text.size() && text[i + 1] == '\n')) {
                ++stats.lines;
            }
        }

        ++stats.scalar_count;
        i += units;
    }
    return stats;
}

static void line_col_for_index(const std::u16string &text, size_t wanted, size_t *line, size_t *col) {
    *line = 1;
    *col = 1;
    for (size_t i = 0; i < text.size() && i < wanted;) {
        uint32_t scalar = 0;
        size_t units = 1;
        bool invalid = false;
        next_scalar(text, i, &scalar, &units, &invalid);
        if (scalar == '\r') {
            if (i + 1 < text.size() && text[i + 1] == '\n') {
                i += 2;
            } else {
                i += units;
            }
            ++(*line);
            *col = 1;
            continue;
        }
        if (scalar == '\n') {
            ++(*line);
            *col = 1;
        } else {
            ++(*col);
        }
        i += units;
    }
}

static int validate_text(const TextData &data, const TextStats &stats, const Options &opt) {
    if (data.kind == DataKind::Directory && opt.transfer_mode != TransferMode::ZipHex) {
        fprintf(stderr, "Directory sources are only supported with --mode zip-hex.\n");
        return EXIT_ARGS;
    }
    if (data.kind == DataKind::FileBytes &&
        (opt.transfer_mode == TransferMode::Simple || opt.output_encoding != OutputEncoding::Preserve)) {
        fprintf(stderr, "Raw file bytes require cmd-hex or zip-hex with --output-encoding preserve.\n");
        return EXIT_ARGS;
    }

    if (data.kind == DataKind::Text && data.text.empty()) {
        fprintf(stderr, "Input text is empty.\n");
        return EXIT_CONTENT;
    }
    if (data.kind == DataKind::Text && stats.control_count > 0) {
        size_t line = 0;
        size_t col = 0;
        line_col_for_index(data.text, stats.first_control, &line, &col);
        fprintf(stderr, "Input contains unsupported control characters. First one at line %zu, column %zu.\n", line, col);
        fprintf(stderr, "Allowed control characters are tab and newlines only.\n");
        return EXIT_CONTENT;
    }
    if (data.kind == DataKind::Text && stats.surrogate_error_count > 0) {
        size_t line = 0;
        size_t col = 0;
        line_col_for_index(data.text, stats.first_surrogate_error, &line, &col);
        fprintf(stderr, "Input contains invalid UTF-16 surrogate data. First issue at line %zu, column %zu.\n", line, col);
        return EXIT_CONTENT;
    }
    if (data.kind == DataKind::Text && opt.ascii_only && stats.non_ascii_count > 0) {
        size_t line = 0;
        size_t col = 0;
        line_col_for_index(data.text, stats.first_non_ascii, &line, &col);
        fprintf(stderr, "--ascii-only is enabled, but input has non-ASCII characters. First one at line %zu, column %zu.\n", line, col);
        return EXIT_CONTENT;
    }
    if (opt.transfer_mode == TransferMode::CmdHex || opt.transfer_mode == TransferMode::ZipHex) {
        if (opt.output_encoding == OutputEncoding::Preserve && data.kind != DataKind::FileBytes &&
            data.kind != DataKind::Directory) {
            fprintf(stderr, "--output-encoding preserve requires file input; clipboard text has no original bytes.\n");
            return EXIT_ARGS;
        }
        std::string path_error = remote_output_error(opt.remote_output);
        if (!path_error.empty()) {
            fprintf(stderr, "%s.\n", path_error.c_str());
            return EXIT_ARGS;
        }
        return EXIT_OK;
    }
    if (!opt.commands_out.empty()) {
        fprintf(stderr, "--commands-out is only valid with --mode cmd-hex or --mode zip-hex.\n");
        return EXIT_ARGS;
    }
    if (opt.remote_output_set) {
        fprintf(stderr, "--remote-output is only valid with cmd-hex or zip-hex.\n");
        return EXIT_ARGS;
    }
    if (opt.debug_input && opt.input_mode == InputMode::Unicode) {
        return EXIT_OK;
    }
    for (size_t i = 0; i < data.text.size();) {
        uint32_t scalar = 0;
        size_t units = 1;
        bool invalid = false;
        next_scalar(data.text, i, &scalar, &units, &invalid);
        bool allowed_control = scalar == '\t' || scalar == '\n' || scalar == '\r';
        if (!allowed_control && (invalid || units != 1 || !simple_ascii_key_supported(data.text[i]))) {
            size_t line = 0;
            size_t col = 0;
            line_col_for_index(data.text, i, &line, &col);
            fprintf(stderr, "Simple mode rejects the character at line %zu, column %zu because it is uppercase, Unicode, or requires a modifier.\n",
                    line, col);
            fprintf(stderr, "Use --mode cmd-hex or --mode zip-hex.\n");
            return EXIT_CONTENT;
        }
        i += units;
    }
    return EXIT_OK;
}

static InputMode effective_input_mode(const Options &opt) {
    if (opt.input_mode != InputMode::Auto) {
        return opt.input_mode;
    }
    return InputMode::Keys;
}

static const char *input_mode_name(InputMode mode) {
    switch (mode) {
        case InputMode::Auto:
            return "Auto";
        case InputMode::Keys:
            return "Simple ASCII virtual keys";
        case InputMode::Unicode:
            return "Unicode CGEvent payloads";
    }
    return "Unknown";
}

static FrontApp frontmost_app() {
    FrontApp app;
    @autoreleasepool {
        NSRunningApplication *running = [[NSWorkspace sharedWorkspace] frontmostApplication];
        if (running != nil) {
            app.pid = [running processIdentifier];
            NSString *name = [running localizedName];
            app.name = name ? nsstring_to_utf8(name) : "";

            CFArrayRef copied = CGWindowListCopyWindowInfo(
                kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
                kCGNullWindowID);
            NSArray *windows = CFBridgingRelease(copied);
            for (NSDictionary *info in windows) {
                NSNumber *owner_pid = info[(__bridge NSString *)kCGWindowOwnerPID];
                NSNumber *layer = info[(__bridge NSString *)kCGWindowLayer];
                if ([owner_pid intValue] != app.pid || [layer intValue] != 0) {
                    continue;
                }
                NSNumber *number = info[(__bridge NSString *)kCGWindowNumber];
                NSString *title = info[(__bridge NSString *)kCGWindowName];
                app.window_id = static_cast<CGWindowID>([number unsignedIntValue]);
                app.window_title = title ? nsstring_to_utf8(title) : "";
                break;
            }
        }
    }
    return app;
}

static void print_front_app(const FrontApp &app) {
    if (app.pid == 0) {
        puts("Target app: <unknown>");
    } else {
        printf("Target app: \"%s\" (pid %d, window %u)", app.name.c_str(), app.pid,
               static_cast<unsigned int>(app.window_id));
        if (!app.window_title.empty()) {
            printf(" \"%s\"", app.window_title.c_str());
        }
        printf("\n");
    }
}

static bool accessibility_trusted(bool prompt) {
    NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @(prompt)};
    return AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
}

static void print_summary(const TextData &data, const TextStats &stats, const Options &opt) {
    InputMode mode = effective_input_mode(opt);
    printf("Source: %s\n", data.source_label.c_str());
    printf("Encoding: %s\n", data.encoding.c_str());
    printf("Bytes: %zu\n", data.byte_count);
    if (data.kind == DataKind::Directory) {
        printf("Directory entries: %zu file(s), %zu subdirectory(s)\n", data.file_count, data.directory_count);
    } else if (data.kind == DataKind::FileBytes) {
        printf("Content: arbitrary regular file bytes\n");
    } else {
        printf("Characters: %zu\n", stats.scalar_count);
        printf("UTF-16 units: %zu\n", data.text.size());
        printf("Lines: %zu\n", stats.lines);
        printf("Non-ASCII characters: %zu\n", stats.non_ascii_count);
    }
    printf("Delay: %d ms per character, %d ms per line\n", opt.delay_ms, opt.line_delay_ms);
    if (opt.transfer_mode == TransferMode::CmdHex) {
        printf("Mode: cmd-hex transfer through remote cmd/certutil\n");
        printf("Output encoding: %s\n", output_encoding_name(opt.output_encoding));
        printf("Remote output: %s\n", opt.remote_output.c_str());
        printf("Path transport: %s\n", remote_output_is_shift_free(opt.remote_output) ? "direct" : "hex helper");
        printf("Hex chunk: %d bytes per generated line\n", opt.hex_chunk_bytes);
    } else if (opt.transfer_mode == TransferMode::ZipHex) {
        printf("Mode: zip-hex transfer through remote PowerShell/certutil/Expand-Archive\n");
        if (data.kind == DataKind::Directory) {
            printf("Output encoding: directory file bytes preserved\n");
            printf("Remote destination directory: %s\n", opt.remote_output.c_str());
        } else {
            printf("Output encoding: %s\n", output_encoding_name(opt.output_encoding));
            printf("Remote output: %s\n", opt.remote_output.c_str());
        }
        printf("Path transport: %s\n", remote_output_is_shift_free(opt.remote_output) ? "direct" : "hex helper");
        printf("Hex chunk: %d bytes per generated line\n", opt.hex_chunk_bytes);
    } else if (opt.input_mode == InputMode::Auto) {
        printf("Input mode: Auto -> %s\n", input_mode_name(mode));
    } else {
        printf("Input mode: %s\n", input_mode_name(mode));
    }
    if (opt.transfer_mode == TransferMode::Simple && mode == InputMode::Unicode) {
        printf("RDP note: some RDP clients ignore Unicode CGEvent payloads and may type repeated 'a'. Use --input-mode keys for ASCII text.\n");
    }
    if (opt.transfer_mode == TransferMode::CmdHex || opt.transfer_mode == TransferMode::ZipHex) {
        printf("Enter mode: physical Return key (forced for complex mode)\n");
    } else {
        printf("Enter mode: %s\n", opt.enter_mode == EnterMode::Unicode ? "Unicode CR" : "Return key");
    }
    printf("Focus check: %s\n", opt.no_focus_check ? "off" : "on");
    printf("Accessibility check: required before typing\n");
}

static void sleep_ms(int ms) {
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

struct KeyStroke {
    CGKeyCode keycode = 0;
};

static bool simple_ascii_key_supported(char16_t ch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
        return true;
    }
    switch (ch) {
        case ' ':
        case '`':
        case '-':
        case '=':
        case '[':
        case ']':
        case '\\':
        case ';':
        case '\'':
        case ',':
        case '.':
        case '/':
            return true;
        default:
            return false;
    }
}

static bool ascii_keystroke(char16_t ch, KeyStroke *stroke) {
    if (ch >= 'a' && ch <= 'z') {
        static const CGKeyCode keys[] = {
            kVK_ANSI_A, kVK_ANSI_B, kVK_ANSI_C, kVK_ANSI_D, kVK_ANSI_E, kVK_ANSI_F,
            kVK_ANSI_G, kVK_ANSI_H, kVK_ANSI_I, kVK_ANSI_J, kVK_ANSI_K, kVK_ANSI_L,
            kVK_ANSI_M, kVK_ANSI_N, kVK_ANSI_O, kVK_ANSI_P, kVK_ANSI_Q, kVK_ANSI_R,
            kVK_ANSI_S, kVK_ANSI_T, kVK_ANSI_U, kVK_ANSI_V, kVK_ANSI_W, kVK_ANSI_X,
            kVK_ANSI_Y, kVK_ANSI_Z,
        };
        stroke->keycode = keys[ch - 'a'];
        return true;
    }
    if (ch >= '0' && ch <= '9') {
        static const CGKeyCode keys[] = {
            kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
            kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9,
        };
        stroke->keycode = keys[ch - '0'];
        return true;
    }

    switch (ch) {
        case ' ': stroke->keycode = kVK_Space; return true;
        case '`': stroke->keycode = kVK_ANSI_Grave; return true;
        case '-': stroke->keycode = kVK_ANSI_Minus; return true;
        case '=': stroke->keycode = kVK_ANSI_Equal; return true;
        case '[': stroke->keycode = kVK_ANSI_LeftBracket; return true;
        case ']': stroke->keycode = kVK_ANSI_RightBracket; return true;
        case '\\': stroke->keycode = kVK_ANSI_Backslash; return true;
        case ';': stroke->keycode = kVK_ANSI_Semicolon; return true;
        case '\'': stroke->keycode = kVK_ANSI_Quote; return true;
        case ',': stroke->keycode = kVK_ANSI_Comma; return true;
        case '.': stroke->keycode = kVK_ANSI_Period; return true;
        case '/': stroke->keycode = kVK_ANSI_Slash; return true;
        default:
            return false;
    }
}

static bool escape_pressed() {
    return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, kVK_Escape);
}

static bool space_pressed() {
    return CGEventSourceKeyState(kCGEventSourceStateHIDSystemState, kVK_Space);
}

static std::string keyboard_state_error() {
    CGEventFlags flags = CGEventSourceFlagsState(kCGEventSourceStateCombinedSessionState);
    if ((flags & kCGEventFlagMaskAlphaShift) != 0) {
        return "Caps Lock is on. Turn it off before typing.";
    }
    if ((flags & (kCGEventFlagMaskShift | kCGEventFlagMaskControl |
                  kCGEventFlagMaskAlternate | kCGEventFlagMaskCommand)) != 0) {
        return "A modifier key is held. Release Shift, Control, Option, and Command before typing.";
    }
    return {};
}

class MacKeyboard {
public:
    MacKeyboard() {
        source_ = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
        if (source_) {
            CGEventSourceSetLocalEventsSuppressionInterval(source_, 0);
        }
    }

    ~MacKeyboard() {
        if (source_) {
            CFRelease(source_);
        }
    }

    bool ok() const {
        return source_ != nullptr;
    }

    bool send_key(CGKeyCode keycode) {
        if (!source_) {
            return false;
        }
        CGEventRef down = CGEventCreateKeyboardEvent(source_, keycode, true);
        CGEventRef up = CGEventCreateKeyboardEvent(source_, keycode, false);
        if (!down || !up) {
            if (down) {
                CFRelease(down);
            }
            if (up) {
                CFRelease(up);
            }
            return false;
        }
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(down);
        CFRelease(up);
        return true;
    }

    bool send_ascii_char(char16_t ch) {
        KeyStroke stroke;
        if (!ascii_keystroke(ch, &stroke)) {
            return false;
        }
        return send_key(stroke.keycode);
    }

    bool send_unicode_units(const char16_t *units, size_t len) {
        if (!source_ || len == 0) {
            return false;
        }
        CGEventRef down = CGEventCreateKeyboardEvent(source_, 0, true);
        CGEventRef up = CGEventCreateKeyboardEvent(source_, 0, false);
        if (!down || !up) {
            if (down) {
                CFRelease(down);
            }
            if (up) {
                CFRelease(up);
            }
            return false;
        }
        CGEventKeyboardSetUnicodeString(down, static_cast<UniCharCount>(len), reinterpret_cast<const UniChar *>(units));
        CGEventKeyboardSetUnicodeString(up, static_cast<UniCharCount>(len), reinterpret_cast<const UniChar *>(units));
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
        CFRelease(down);
        CFRelease(up);
        return true;
    }

private:
    CGEventSourceRef source_ = nullptr;
};

static bool target_changed(const FrontApp &target, FrontApp *current_out = nullptr) {
    FrontApp current = frontmost_app();
    if (current_out != nullptr) {
        *current_out = current;
    }
    bool app_changed = target.pid != 0 && current.pid != target.pid;
    bool window_changed = target.window_id != 0 && current.window_id != target.window_id;
    return app_changed || window_changed;
}

static bool target_app_changed(const FrontApp &target) {
    @autoreleasepool {
        NSRunningApplication *running = [[NSWorkspace sharedWorkspace] frontmostApplication];
        return target.pid != 0 && (running == nil || [running processIdentifier] != target.pid);
    }
}

static int wait_for_space_release(const Options &opt, const FrontApp &target) {
    while (space_pressed()) {
        if (escape_pressed()) {
            return EXIT_ABORTED;
        }
        if (!opt.no_focus_check && target_app_changed(target)) {
            fprintf(stderr, "\nForeground app changed while handling Space; aborting.\n");
            return EXIT_ABORTED;
        }
        sleep_ms(10);
    }
    return EXIT_OK;
}

static int pause_until_space(MacKeyboard &keyboard, const Options &opt, const FrontApp &target) {
    int rc = wait_for_space_release(opt, target);
    if (rc != EXIT_OK) {
        return rc;
    }
    if (!opt.no_focus_check && target_changed(target)) {
        fprintf(stderr, "\nForeground target changed while handling Space; aborting.\n");
        return EXIT_ABORTED;
    }
    if (!keyboard.send_key(kVK_Delete)) {
        fprintf(stderr, "\nCould not erase the pause Space from the target.\n");
        return EXIT_INPUT;
    }

    puts("\nPaused. Press Space again to continue; Esc aborts.");
    for (;;) {
        if (escape_pressed()) {
            return EXIT_ABORTED;
        }
        if (!opt.no_focus_check && target_app_changed(target)) {
            fprintf(stderr, "\nForeground app changed while paused; aborting.\n");
            return EXIT_ABORTED;
        }
        if (space_pressed()) {
            rc = wait_for_space_release(opt, target);
            if (rc != EXIT_OK) {
                return rc;
            }
            if (!opt.no_focus_check && target_changed(target)) {
                fprintf(stderr, "\nForeground target changed while paused; aborting.\n");
                return EXIT_ABORTED;
            }
            if (!keyboard.send_key(kVK_Delete)) {
                fprintf(stderr, "\nCould not erase the resume Space from the target.\n");
                return EXIT_INPUT;
            }
            puts("Resuming.");
            return EXIT_OK;
        }
        sleep_ms(10);
    }
}

static int check_runtime_controls(MacKeyboard &keyboard, const Options &opt,
                                  const FrontApp &target, bool check_focus) {
    if (escape_pressed()) {
        return EXIT_ABORTED;
    }
    if (check_focus && !opt.no_focus_check) {
        FrontApp current;
        if (target_changed(target, &current)) {
            fprintf(stderr, "\nForeground target changed from app \"%s\" window %u to app \"%s\" window %u. "
                    "Aborting to avoid typing into the wrong target.\n",
                    target.name.c_str(), static_cast<unsigned int>(target.window_id),
                    current.name.c_str(), static_cast<unsigned int>(current.window_id));
            return EXIT_ABORTED;
        }
    }
    if (space_pressed()) {
        return pause_until_space(keyboard, opt, target);
    }
    std::string state_error = keyboard_state_error();
    if (!state_error.empty()) {
        fprintf(stderr, "\n%s\n", state_error.c_str());
        return EXIT_ABORTED;
    }
    return EXIT_OK;
}

static int wait_typing_delay(MacKeyboard &keyboard, const Options &opt,
                             const FrontApp &target, int delay_ms) {
    int remaining = delay_ms;
    while (remaining > 0) {
        int slice = remaining > 10 ? 10 : remaining;
        int rc = check_runtime_controls(keyboard, opt, target, false);
        if (rc != EXIT_OK) {
            return rc;
        }
        sleep_ms(slice);
        remaining -= slice;
    }
    return EXIT_OK;
}

static int run_countdown(int seconds) {
    for (int remaining = seconds; remaining > 0; --remaining) {
        printf("\rStarting in %d second(s)...", remaining);
        fflush(stdout);
        for (int i = 0; i < 10; ++i) {
            if (escape_pressed()) {
                printf("\nAborted before typing.\n");
                return EXIT_ABORTED;
            }
            sleep_ms(100);
        }
    }
    if (seconds > 0) {
        printf("\rStarting now.              \n");
    } else {
        puts("Starting now.");
    }
    return EXIT_OK;
}

static int prepare_target(const Options &opt, FrontApp *target) {
    if (!accessibility_trusted(opt.request_accessibility)) {
        fprintf(stderr, "macOS Accessibility permission is not enabled for this terminal or binary.\n");
        fprintf(stderr, "Enable it in System Settings > Privacy & Security > Accessibility, then run again.\n");
        fprintf(stderr, "You can run with --request-accessibility to ask macOS to show the permission prompt.\n");
        return EXIT_INPUT;
    }

    puts("Focus the target window now.");
    puts("Ctrl-C or Esc aborts. Space pauses or resumes.");
    int rc = run_countdown(opt.start_delay_sec);
    if (rc != EXIT_OK) {
        return rc;
    }
    std::string state_error = keyboard_state_error();
    if (!state_error.empty()) {
        fprintf(stderr, "%s\n", state_error.c_str());
        return EXIT_INPUT;
    }
    *target = frontmost_app();
    print_front_app(*target);
    return EXIT_OK;
}

static int type_text(MacKeyboard &keyboard, const TextData &data, const TextStats &stats, const Options &opt, const FrontApp &initial_target) {
    size_t line = 1;
    size_t col = 1;
    size_t typed = 0;
    FrontApp target = initial_target;
    InputMode mode = effective_input_mode(opt);

    puts("");
    puts("Typing started. Ctrl-C or Esc aborts. Space pauses or resumes.");
    printf("Progress: line %zu/%zu\n", line, stats.lines);

    for (size_t i = 0; i < data.text.size();) {
        int rc = check_runtime_controls(keyboard, opt, target, true);
        if (rc != EXIT_OK) {
            if (rc == EXIT_ABORTED) {
                printf("\nAborted at line %zu, column %zu after %zu character/event unit(s).\n", line, col, typed);
            }
            return rc;
        }

        uint32_t scalar = 0;
        size_t units = 1;
        bool invalid = false;
        next_scalar(data.text, i, &scalar, &units, &invalid);
        bool ok = false;
        int event_delay_ms = opt.delay_ms;

        if (scalar == '\r') {
            if (i + 1 < data.text.size() && data.text[i + 1] == '\n') {
                units = 2;
            }
            if (opt.enter_mode == EnterMode::Unicode) {
                char16_t cr = '\r';
                ok = keyboard.send_unicode_units(&cr, 1);
            } else {
                ok = keyboard.send_key(kVK_Return);
            }
            ++line;
            col = 1;
            printf("\rProgress: line %zu/%zu", line <= stats.lines ? line : stats.lines, stats.lines);
            fflush(stdout);
            event_delay_ms = opt.line_delay_ms;
        } else if (scalar == '\n') {
            if (opt.enter_mode == EnterMode::Unicode) {
                char16_t cr = '\r';
                ok = keyboard.send_unicode_units(&cr, 1);
            } else {
                ok = keyboard.send_key(kVK_Return);
            }
            ++line;
            col = 1;
            printf("\rProgress: line %zu/%zu", line <= stats.lines ? line : stats.lines, stats.lines);
            fflush(stdout);
            event_delay_ms = opt.line_delay_ms;
        } else if (scalar == '\t') {
            ok = keyboard.send_key(kVK_Tab);
            ++col;
        } else {
            if (mode == InputMode::Keys) {
                ok = keyboard.send_ascii_char(data.text[i]);
            } else {
                ok = keyboard.send_unicode_units(&data.text[i], units);
            }
            ++col;
        }

        if (!ok) {
            fprintf(stderr, "\nInput failed at line %zu, column %zu.\n", line, col);
            return EXIT_INPUT;
        }
        rc = wait_typing_delay(keyboard, opt, target, event_delay_ms);
        if (rc != EXIT_OK) {
            if (rc == EXIT_ABORTED) {
                printf("\nAborted at line %zu, column %zu after %zu character/event unit(s).\n", line, col, typed + 1);
            }
            return rc;
        }
        ++typed;
        i += units;
    }

    printf("\rProgress: line %zu/%zu\n", stats.lines, stats.lines);
    printf("Typing completed. Typed %zu character/event unit(s).\n", typed);
    return EXIT_OK;
}

static int run_self_test(const Options &opt) {
    printf("Accessibility trusted: %s\n", accessibility_trusted(opt.request_accessibility) ? "yes" : "no");
    print_front_app(frontmost_app());
    fflush(stdout);
    if (!accessibility_trusted(opt.request_accessibility)) {
        fprintf(stderr, "Self-test failed before input: Accessibility permission is not enabled.\n");
        return EXIT_INPUT;
    }
    MacKeyboard keyboard;
    if (!keyboard.ok()) {
        fprintf(stderr, "Self-test failed: cannot create CGEventSource.\n");
        return EXIT_INPUT;
    }
    if (!keyboard.send_key(kVK_F20)) {
        fprintf(stderr, "Self-test failed: cannot post F20 key event.\n");
        return EXIT_INPUT;
    }
    puts("Self-test completed: posted a harmless F20 key event.");
    puts("Note: macOS CGEventPost has no per-event accepted count. Use --debug-input for visible verification.");
    return EXIT_OK;
}

static int run_debug_input(const Options &opt) {
    MacKeyboard keyboard;
    if (!keyboard.ok()) {
        fprintf(stderr, "Cannot create CGEventSource.\n");
        return EXIT_INPUT;
    }
    FrontApp target;
    int rc = prepare_target(opt, &target);
    if (rc != EXIT_OK) {
        return rc;
    }

    TextData keys_data;
    NSString *keys_marker = @"ttdbg-mac-keys abc xyz 0123456789\n";
    keys_data.text = nsstring_to_u16(keys_marker);
    keys_data.byte_count = [[keys_marker dataUsingEncoding:NSUTF8StringEncoding] length];
    keys_data.encoding = "built-in ASCII debug marker";
    keys_data.source_label = "Debug marker: ASCII keys";
    TextStats keys_stats = analyze_text(keys_data.text);

    Options keys_opt = opt;
    keys_opt.input_mode = InputMode::Keys;
    print_summary(keys_data, keys_stats, keys_opt);
    rc = validate_text(keys_data, keys_stats, keys_opt);
    if (rc != EXIT_OK) {
        return rc;
    }
    rc = type_text(keyboard, keys_data, keys_stats, keys_opt, target);
    if (rc != EXIT_OK) {
        return rc;
    }

    TextData unicode_data;
    NSString *unicode_marker = @"ttdbg-mac-unicode unicode=中 check=✓\n";
    unicode_data.text = nsstring_to_u16(unicode_marker);
    unicode_data.byte_count = [[unicode_marker dataUsingEncoding:NSUTF8StringEncoding] length];
    unicode_data.encoding = "built-in Unicode debug marker";
    unicode_data.source_label = "Debug marker: Unicode payload";
    TextStats unicode_stats = analyze_text(unicode_data.text);

    Options unicode_opt = opt;
    unicode_opt.input_mode = InputMode::Unicode;
    print_summary(unicode_data, unicode_stats, unicode_opt);
    rc = validate_text(unicode_data, unicode_stats, unicode_opt);
    if (rc != EXIT_OK) {
        return rc;
    }
    return type_text(keyboard, unicode_data, unicode_stats, unicode_opt, target);
}

static int type_generated_ascii(MacKeyboard &keyboard, const std::string &text, const std::string &label, const Options &base_opt, const FrontApp &target) {
    TextData data = text_data_from_ascii(text, label);
    TextStats stats = analyze_text(data.text);
    Options opt = base_opt;
    opt.transfer_mode = TransferMode::Simple;
    opt.input_mode = InputMode::Keys;
    opt.enter_mode = EnterMode::Key;
    opt.commands_out.clear();
    opt.remote_output_set = false;
    int rc = validate_text(data, stats, opt);
    if (rc != EXIT_OK) {
        return rc;
    }
    return type_text(keyboard, data, stats, opt, target);
}

static int run_cmd_hex_transfer(const TextData &data, const Options &opt) {
    std::vector<unsigned char> bytes;
    std::string error;
    if (!make_output_bytes(data, opt, &bytes, &error)) {
        fprintf(stderr, "%s\n", error.c_str());
        return EXIT_ENCODING;
    }

    std::string hex_text = bytes_to_plain_hex(bytes, opt.hex_chunk_bytes);
    size_t hex_line_count = count_hex_lines(hex_text);
    printf("cmd-hex transfer mode.\n");
    printf("Output bytes to transfer: %zu\n", bytes.size());
    printf("Remote output: %s\n", opt.remote_output.c_str());
    printf("Hex chunk: %d bytes per generated line (%zu line(s))\n", opt.hex_chunk_bytes, hex_line_count);
    printf("Expected SHA-256: %s\n", sha256_hex(bytes).c_str());
    if (remote_output_is_shift_free(opt.remote_output)) {
        printf("Remote temporary %s is deleted after reconstruction.\n", REMOTE_HEX);
    } else {
        printf("Remote temporary %s, %s, %s, and %s are deleted after reconstruction.\n",
               REMOTE_HEX, REMOTE_STAGE, REMOTE_HELPER_HEX, REMOTE_HELPER);
    }
    printf("Focus a remote cmd.exe or PowerShell prompt. This mode uses PowerShell Set-Content/Add-Content, not redirection or Ctrl+Z/F6.\n");
    fflush(stdout);

    std::string commands = build_cmd_hex_commands(hex_text, opt);
    int rc = prepare_generated_commands(commands, opt);
    if (rc != EXIT_OK) {
        return rc;
    }
    if (opt.dry_run) {
        printf("Generated hex characters: %zu\n", hex_text.size() - hex_line_count);
        printf("Generated command characters: %zu\n", commands.size());
        puts("Dry run passed. No typing was performed.");
        return EXIT_OK;
    }

    MacKeyboard keyboard;
    if (!keyboard.ok()) {
        fprintf(stderr, "Cannot create CGEventSource.\n");
        return EXIT_INPUT;
    }

    FrontApp target;
    rc = prepare_target(opt, &target);
    if (rc != EXIT_OK) {
        return rc;
    }

    rc = type_generated_ascii(keyboard, commands, "cmd-hex echo/certutil commands", opt, target);
    if (rc != EXIT_OK) {
        return rc;
    }

    puts("cmd-hex transfer typing completed. Verify the remote output file before running it.");
    return EXIT_OK;
}

static int run_zip_hex_transfer(const TextData &data, const Options &opt) {
    std::vector<unsigned char> zip_bytes;
    std::vector<unsigned char> bytes;
    std::string error;
    bool directory_source = data.kind == DataKind::Directory;
    if (directory_source) {
        if (!make_directory_zip_payload(data, opt, &zip_bytes, &error)) {
            fprintf(stderr, "%s\n", error.c_str());
            return EXIT_FILE;
        }
    } else {
        if (!make_output_bytes(data, opt, &bytes, &error)) {
            fprintf(stderr, "%s\n", error.c_str());
            return EXIT_ENCODING;
        }
        if (!make_zip_payload(bytes, opt.remote_output, &zip_bytes, &error)) {
            fprintf(stderr, "%s\n", error.c_str());
            return EXIT_FILE;
        }
    }

    std::string hex_text = bytes_to_plain_hex(zip_bytes, opt.hex_chunk_bytes);
    size_t hex_line_count = count_hex_lines(hex_text);
    size_t source_bytes = directory_source ? data.byte_count : bytes.size();
    double ratio = source_bytes == 0 ? 0.0 :
        (static_cast<double>(zip_bytes.size()) / static_cast<double>(source_bytes)) * 100.0;
    printf("zip-hex transfer mode.\n");
    printf("Output bytes before zip: %zu\n", source_bytes);
    printf("Zip bytes to transfer: %zu (%.2f%% of output size)\n", zip_bytes.size(), ratio);
    printf("Remote output: %s\n", opt.remote_output.c_str());
    printf("Hex chunk: %d bytes per generated line (%zu line(s))\n", opt.hex_chunk_bytes, hex_line_count);
    if (directory_source) {
        printf("Expected archive SHA-256: %s\n", sha256_hex(zip_bytes).c_str());
        printf("The remote destination is merged/overwritten; unrelated existing files are not removed.\n");
    } else {
        printf("Expected SHA-256: %s\n", sha256_hex(bytes).c_str());
    }
    if (remote_output_is_shift_free(opt.remote_output)) {
        printf("Remote temporary %s and %s are deleted after extraction.\n", REMOTE_HEX, REMOTE_ZIP);
    } else {
        printf("Remote temporary %s, %s, %s, and %s are deleted after extraction.\n",
               REMOTE_HEX, REMOTE_ZIP, REMOTE_HELPER_HEX, REMOTE_HELPER);
    }
    printf("Focus a remote cmd.exe or PowerShell prompt. This mode decodes a zip, then runs Expand-Archive.\n");
    fflush(stdout);

    std::string commands = build_zip_hex_commands(hex_text, opt, directory_source);
    int rc = prepare_generated_commands(commands, opt);
    if (rc != EXIT_OK) {
        return rc;
    }
    if (opt.dry_run) {
        printf("Generated hex characters: %zu\n", hex_text.size() - hex_line_count);
        printf("Generated command characters: %zu\n", commands.size());
        puts("Dry run passed. No typing was performed.");
        return EXIT_OK;
    }

    MacKeyboard keyboard;
    if (!keyboard.ok()) {
        fprintf(stderr, "Cannot create CGEventSource.\n");
        return EXIT_INPUT;
    }

    FrontApp target;
    rc = prepare_target(opt, &target);
    if (rc != EXIT_OK) {
        return rc;
    }

    rc = type_generated_ascii(keyboard, commands, "zip-hex expand-archive commands", opt, target);
    if (rc != EXIT_OK) {
        return rc;
    }

    puts("zip-hex transfer typing completed. Verify the remote output file before running it.");
    return EXIT_OK;
}

static int run_diagnose(const Options &opt) {
    printf("Accessibility trusted: %s\n", accessibility_trusted(opt.request_accessibility) ? "yes" : "no");
    print_front_app(frontmost_app());

    TextData data;
    std::string error;
    if (read_text_source(opt, &data, &error)) {
        Options resolved = opt;
        if (!apply_default_remote_output(&resolved, data, &error)) {
            fprintf(stderr, "Source read failed: %s\n", error.c_str());
            return EXIT_ARGS;
        }
        TextStats stats = analyze_text(data.text);
        print_summary(data, stats, resolved);
    } else {
        fprintf(stderr, "Source read failed: %s\n", error.c_str());
        return opt.source == SourceKind::Clipboard ? EXIT_CONTENT : EXIT_FILE;
    }
    return EXIT_OK;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        Options opt;
        if (!parse_args(argc, argv, &opt)) {
            puts("");
            print_usage();
            return EXIT_ARGS;
        }

        if (opt.self_test) {
            return run_self_test(opt);
        }

        if (opt.debug_input) {
            return run_debug_input(opt);
        }

        if (opt.diagnose) {
            return run_diagnose(opt);
        }

        TextData data;
        std::string error;
        if (!read_text_source(opt, &data, &error)) {
            fprintf(stderr, "%s\n", error.c_str());
            return opt.source == SourceKind::Clipboard ? EXIT_CONTENT : EXIT_FILE;
        }
        if (!apply_default_remote_output(&opt, data, &error)) {
            fprintf(stderr, "%s\n", error.c_str());
            return EXIT_ARGS;
        }

        TextStats stats = analyze_text(data.text);
        print_summary(data, stats, opt);

        int rc = validate_text(data, stats, opt);
        if (rc != EXIT_OK) {
            return rc;
        }

        if (opt.transfer_mode == TransferMode::CmdHex) {
            return run_cmd_hex_transfer(data, opt);
        }
        if (opt.transfer_mode == TransferMode::ZipHex) {
            return run_zip_hex_transfer(data, opt);
        }

        if (opt.dry_run) {
            puts("Dry run passed. No typing was performed.");
            return EXIT_OK;
        }

        MacKeyboard keyboard;
        if (!keyboard.ok()) {
            fprintf(stderr, "Cannot create CGEventSource.\n");
            return EXIT_INPUT;
        }

        FrontApp target;
        rc = prepare_target(opt, &target);
        if (rc != EXIT_OK) {
            return rc;
        }
        return type_text(keyboard, data, stats, opt, target);
    }
}
