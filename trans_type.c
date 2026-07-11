#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WINVER
#define WINVER 0x0A00
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <conio.h>
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define DEFAULT_DELAY_MS 20
#define DEFAULT_LINE_DELAY_MS 100
#define DEFAULT_START_DELAY_SEC 5
#define DEFAULT_MAX_BYTES (1024 * 1024)
#define ABSOLUTE_MAX_BYTES (100 * 1024 * 1024)
#define DEFAULT_HEX_CHUNK_BYTES 240
#define MAX_HEX_CHUNK_BYTES 2048
#define MAX_DIRECTORY_ENTRIES 10000
#define REMOTE_HEX "tt.hex"
#define REMOTE_ZIP "tt.zip"
#define REMOTE_STAGE "tt.out"
#define REMOTE_HELPER_HEX "tt.cmd.hex"
#define REMOTE_HELPER "tt.cmd"
#define MAX_REMOTE_OUTPUT_UTF8 1024

#ifndef PROGRAM_NAME
#define PROGRAM_NAME "trans_type.exe"
#endif

enum ExitCode {
    EXIT_OK = 0,
    EXIT_ARGS = 2,
    EXIT_FILE = 3,
    EXIT_ENCODING = 4,
    EXIT_CONTENT = 5,
    EXIT_ABORTED = 6,
    EXIT_INPUT = 7
};

enum SourceKind {
    SOURCE_FILE = 0,
    SOURCE_CLIPBOARD = 1
};

enum TransferMode {
    TRANSFER_SIMPLE = 0,
    TRANSFER_CMD_HEX = 1,
    TRANSFER_ZIP_HEX = 2
};

enum OutputEncoding {
    OUTPUT_UTF8 = 0,
    OUTPUT_UTF8_BOM = 1,
    OUTPUT_PRESERVE = 2
};

enum DataKind {
    DATA_TEXT = 0,
    DATA_FILE_BYTES = 1,
    DATA_DIRECTORY = 2
};

typedef struct Options {
    int delay_ms;
    int line_delay_ms;
    int start_delay_sec;
    int source;
    int transfer_mode;
    int output_encoding;
    int ascii_only;
    int ascii_keys;
    int legacy_keys;
    int no_focus_check;
    int dry_run;
    int self_test;
    int enter_unicode;
    int max_bytes;
    int hex_chunk_bytes;
    int has_file_path;
    int has_commands_out;
    wchar_t file_path[32768];
    wchar_t commands_out[32768];
    char remote_output[MAX_REMOTE_OUTPUT_UTF8];
    int remote_output_set;
} Options;

typedef struct TextData {
    wchar_t *text;
    int chars;
    int bytes;
    const char *encoding;
    unsigned char *raw_bytes;
    int raw_count;
    int kind;
    int file_count;
    int directory_count;
    wchar_t source_path[32768];
    wchar_t source_label[32768];
} TextData;

typedef struct TextStats {
    int lines;
    int non_ascii_count;
    int control_count;
    int surrogate_error_count;
    int first_non_ascii;
    int first_control;
    int first_surrogate_error;
} TextStats;

typedef struct TargetWindow {
    HWND hwnd;
    DWORD pid;
    wchar_t title[512];
} TargetWindow;

static void init_options(Options *opt) {
    opt->delay_ms = DEFAULT_DELAY_MS;
    opt->line_delay_ms = DEFAULT_LINE_DELAY_MS;
    opt->start_delay_sec = DEFAULT_START_DELAY_SEC;
    opt->source = SOURCE_FILE;
    opt->transfer_mode = TRANSFER_SIMPLE;
    opt->output_encoding = OUTPUT_UTF8;
    opt->ascii_only = 0;
    opt->ascii_keys = 0;
    opt->legacy_keys = 1;
    opt->no_focus_check = 0;
    opt->dry_run = 0;
    opt->self_test = 0;
    opt->enter_unicode = 0;
    opt->max_bytes = DEFAULT_MAX_BYTES;
    opt->hex_chunk_bytes = DEFAULT_HEX_CHUNK_BYTES;
    opt->has_file_path = 0;
    opt->has_commands_out = 0;
    opt->file_path[0] = L'\0';
    opt->commands_out[0] = L'\0';
    strcpy(opt->remote_output, ".\\trans.txt");
    opt->remote_output_set = 0;
}

static void print_help_option(const char *option, const char *description) {
    printf("  %-25s%s\n", option, description);
}

static void print_usage(void) {
    puts(PROGRAM_NAME " - 通过模拟键盘输入文本或文件传输命令");
    puts("Type text or file-transfer commands into the foreground Windows window.");
    puts("");
    puts("用法 / usage:");
    puts("  " PROGRAM_NAME " [options]");
    puts("");
    puts("选项 / options:");
    print_help_option("--source SOURCE", "来源：clipboard、file(trans.txt)或本地路径 / source (default: file)");
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
    print_help_option("--ascii-keys", "使用 SendInput 虚拟键 / use SendInput virtual keys (default: off)");
    print_help_option("--legacy-keys", "使用 keybd_event ASCII 键 / use legacy keys (default)");
    print_help_option("--sendinput", "诊断用 Unicode SendInput / diagnostic transport (default: off)");
    print_help_option("--enter-mode MODE", "回车方式 / key or unicode (default: key)");
    print_help_option("--no-focus-check", "关闭前台窗口检查 / disable focus check (default: check on)");
    print_help_option("--dry-run", "只验证，不输入 / validate without typing (default: off)");
    print_help_option("--self-test", "发送无害 F24 测试键 / harmless F24 test (default: off)");
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
    puts("  Esc    中止 / abort");
    puts("  Enter  倒计时期间立即开始 / start immediately during countdown");
}

static int parse_int_range(const char *s, int min_value, int max_value, int *out) {
    char *end = NULL;
    long value;

    if (s == NULL || *s == '\0') {
        return 0;
    }

    value = strtol(s, &end, 10);
    if (*end != '\0' || value < min_value || value > max_value) {
        return 0;
    }

    *out = (int)value;
    return 1;
}

static int get_option_value(int *i, int argc, char **argv, const char *name, const char **value) {
    size_t name_len = strlen(name);

    if (strncmp(argv[*i], name, name_len) == 0 && argv[*i][name_len] == '=') {
        *value = argv[*i] + name_len + 1;
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

static int arg_to_wide_path(const char *value, wchar_t *out, size_t out_len) {
    int needed;

    if (value == NULL || *value == '\0' || out_len == 0) {
        return 0;
    }

    needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value, -1, out, (int)out_len);
    if (needed > 0) {
        return 1;
    }

    needed = MultiByteToWideChar(CP_ACP, 0, value, -1, out, (int)out_len);
    return needed > 0;
}

static char *wide_to_utf8_alloc(const wchar_t *value);

static int copy_remote_argument(char *dest, size_t dest_len, const char *value, const char *name) {
    wchar_t wide[MAX_REMOTE_OUTPUT_UTF8];
    char *utf8;
    size_t len;

    if (value == NULL || *value == '\0') {
        fprintf(stderr, "%s must not be empty.\n", name);
        return 0;
    }
    if (!arg_to_wide_path(value, wide, sizeof(wide) / sizeof(wide[0]))) {
        fprintf(stderr, "%s is not valid text.\n", name);
        return 0;
    }
    utf8 = wide_to_utf8_alloc(wide);
    if (utf8 == NULL) {
        fprintf(stderr, "%s cannot be converted to UTF-8.\n", name);
        return 0;
    }
    len = strlen(utf8);
    if (len >= dest_len) {
        fprintf(stderr, "%s is too long.\n", name);
        free(utf8);
        return 0;
    }
    memcpy(dest, utf8, len + 1);
    free(utf8);
    return 1;
}

static int parse_args(int argc, char **argv, Options *opt) {
    int i;
    int source_option_kind = 0;

    for (i = 1; i < argc; ++i) {
        const char *value = NULL;
        int matched;

        matched = get_option_value(&i, argc, argv, "--delay-ms", &value);
        if (matched == 1) {
            if (!parse_int_range(value, 0, 5000, &opt->delay_ms)) {
                fprintf(stderr, "Invalid --delay-ms value: %s\n", value);
                return 0;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--line-delay-ms", &value);
        if (matched == 1) {
            if (!parse_int_range(value, 0, 5000, &opt->line_delay_ms)) {
                fprintf(stderr, "Invalid --line-delay-ms value: %s\n", value);
                return 0;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--start-delay-sec", &value);
        if (matched == 1) {
            if (!parse_int_range(value, 0, 3600, &opt->start_delay_sec)) {
                fprintf(stderr, "Invalid --start-delay-sec value: %s\n", value);
                return 0;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--max-bytes", &value);
        if (matched == 1) {
            if (!parse_int_range(value, 1, ABSOLUTE_MAX_BYTES, &opt->max_bytes)) {
                fprintf(stderr, "Invalid --max-bytes value: %s\n", value);
                return 0;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--hex-chunk-bytes", &value);
        if (matched == 1) {
            if (!parse_int_range(value, 1, MAX_HEX_CHUNK_BYTES, &opt->hex_chunk_bytes)) {
                fprintf(stderr, "Invalid --hex-chunk-bytes value: %s\n", value);
                return 0;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--source", &value);
        if (matched == 1) {
            if (source_option_kind != 0) {
                fprintf(stderr, "--source may only be specified once.\n");
                return 0;
            }
            if (strcmp(value, "file") == 0) {
                opt->source = SOURCE_FILE;
                source_option_kind = 1;
            } else if (strcmp(value, "clipboard") == 0) {
                opt->source = SOURCE_CLIPBOARD;
                source_option_kind = 2;
            } else {
                opt->source = SOURCE_FILE;
                opt->has_file_path = 1;
                if (!arg_to_wide_path(value, opt->file_path, sizeof(opt->file_path) / sizeof(opt->file_path[0]))) {
                    fprintf(stderr, "Invalid --source path: %s\n", value);
                    return 0;
                }
                source_option_kind = 3;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--mode", &value);
        if (matched == 1) {
            if (strcmp(value, "simple") == 0) {
                opt->transfer_mode = TRANSFER_SIMPLE;
            } else if (strcmp(value, "cmd-hex") == 0 || strcmp(value, "complex") == 0 || strcmp(value, "cmd") == 0) {
                opt->transfer_mode = TRANSFER_CMD_HEX;
            } else if (strcmp(value, "zip-hex") == 0 || strcmp(value, "zip") == 0) {
                opt->transfer_mode = TRANSFER_ZIP_HEX;
            } else {
                fprintf(stderr, "Invalid --mode value: %s\n", value);
                return 0;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--remote-output", &value);
        if (matched == 1) {
            if (!copy_remote_argument(opt->remote_output, sizeof(opt->remote_output), value, "--remote-output")) {
                return 0;
            }
            opt->remote_output_set = 1;
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--output-encoding", &value);
        if (matched == 1) {
            if (strcmp(value, "utf8") == 0) {
                opt->output_encoding = OUTPUT_UTF8;
            } else if (strcmp(value, "utf8-bom") == 0) {
                opt->output_encoding = OUTPUT_UTF8_BOM;
            } else if (strcmp(value, "preserve") == 0) {
                opt->output_encoding = OUTPUT_PRESERVE;
            } else {
                fprintf(stderr, "Invalid --output-encoding value: %s\n", value);
                return 0;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--commands-out", &value);
        if (matched == 1) {
            opt->has_commands_out = 1;
            if (!arg_to_wide_path(value, opt->commands_out, sizeof(opt->commands_out) / sizeof(opt->commands_out[0]))) {
                fprintf(stderr, "Invalid --commands-out path: %s\n", value);
                return 0;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--enter-mode", &value);
        if (matched == 1) {
            if (strcmp(value, "key") == 0) {
                opt->enter_unicode = 0;
            } else if (strcmp(value, "unicode") == 0) {
                opt->enter_unicode = 1;
            } else {
                fprintf(stderr, "Invalid --enter-mode value: %s\n", value);
                return 0;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        if (strcmp(argv[i], "--ascii-only") == 0) {
            opt->ascii_only = 1;
        } else if (strcmp(argv[i], "--ascii-keys") == 0) {
            opt->ascii_keys = 1;
            opt->legacy_keys = 0;
        } else if (strcmp(argv[i], "--legacy-keys") == 0) {
            opt->legacy_keys = 1;
            opt->ascii_keys = 0;
        } else if (strcmp(argv[i], "--sendinput") == 0) {
            opt->legacy_keys = 0;
            opt->ascii_keys = 0;
        } else if (strcmp(argv[i], "--no-focus-check") == 0) {
            opt->no_focus_check = 1;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            opt->dry_run = 1;
        } else if (strcmp(argv[i], "--self-test") == 0) {
            opt->self_test = 1;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage();
            exit(EXIT_OK);
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 0;
        }
    }

    if (opt->output_encoding == OUTPUT_PRESERVE && opt->transfer_mode == TRANSFER_SIMPLE) {
        fprintf(stderr, "--output-encoding preserve is only valid with --mode cmd-hex or --mode zip-hex.\n");
        return 0;
    }
    if (opt->output_encoding == OUTPUT_PRESERVE && opt->source == SOURCE_CLIPBOARD) {
        fprintf(stderr, "--output-encoding preserve requires a file source, not clipboard text.\n");
        return 0;
    }
    if (opt->transfer_mode == TRANSFER_SIMPLE && opt->remote_output_set) {
        fprintf(stderr, "--remote-output is only valid with cmd-hex or zip-hex.\n");
        return 0;
    }
    if (opt->transfer_mode == TRANSFER_SIMPLE && opt->has_commands_out) {
        fprintf(stderr, "--commands-out is only valid with --mode cmd-hex or --mode zip-hex.\n");
        return 0;
    }
    return 1;
}

static void print_windows_error_a(const char *prefix, DWORD err) {
    char *message = NULL;
    DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;

    FormatMessageA(flags, NULL, err, 0, (LPSTR)&message, 0, NULL);
    if (message != NULL) {
        fprintf(stderr, "%s: %s (Windows error %lu)\n", prefix, message, (unsigned long)err);
        LocalFree(message);
    } else {
        fprintf(stderr, "%s: Windows error %lu\n", prefix, (unsigned long)err);
    }
}

static char *wide_to_utf8_alloc(const wchar_t *value) {
    int needed;
    char *utf8;

    if (value == NULL) {
        return NULL;
    }
    needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        return NULL;
    }
    utf8 = (char *)malloc((size_t)needed);
    if (utf8 == NULL) {
        return NULL;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, utf8, needed, NULL, NULL) != needed) {
        free(utf8);
        return NULL;
    }
    return utf8;
}

static void print_wide_line(FILE *stream, const char *prefix, const wchar_t *value) {
    char *utf8 = wide_to_utf8_alloc(value);
    if (utf8 != NULL) {
        fprintf(stream, "%s%s\n", prefix, utf8);
        free(utf8);
    } else {
        fprintf(stream, "%s<unprintable Unicode text>\n", prefix);
    }
}

static int source_basename_utf8(const Options *opt, const TextData *data, char *out, size_t out_len) {
    const wchar_t *start;
    const wchar_t *end;
    const wchar_t *cursor;
    int chars;
    int needed;

    if (opt->source == SOURCE_CLIPBOARD) {
        if (out_len < sizeof("trans.txt")) {
            return 0;
        }
        strcpy(out, "trans.txt");
        return 1;
    }

    start = data->source_path;
    end = start + wcslen(start);
    while (end > start && (end[-1] == L'\\' || end[-1] == L'/')) {
        --end;
    }
    cursor = end;
    while (cursor > start && cursor[-1] != L'\\' && cursor[-1] != L'/') {
        --cursor;
    }
    chars = (int)(end - cursor);
    if (chars <= 0) {
        fprintf(stderr, "Cannot derive --remote-output from the source path; specify --remote-output TARGET.\n");
        return 0;
    }
    needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, cursor, chars, NULL, 0, NULL, NULL);
    if (needed <= 0 || needed >= (int)out_len) {
        fprintf(stderr, "The source basename cannot be used as --remote-output.\n");
        return 0;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, cursor, chars,
                            out, needed, NULL, NULL) != needed) {
        fprintf(stderr, "The source basename cannot be converted to UTF-8.\n");
        return 0;
    }
    out[needed] = '\0';
    return 1;
}

static int is_path_separator(char ch) {
    return ch == '/' || ch == '\\';
}

static int normalize_remote_output_value(const char *value, const char *source_name,
                                         char *out, size_t out_len) {
    size_t value_len;
    size_t read_pos = 0;
    size_t write_pos = 0;
    int directory_target;
    int is_unc;

    if (value == NULL) {
        int written = snprintf(out, out_len, ".\\%s", source_name);
        return written >= 0 && (size_t)written < out_len;
    }
    value_len = strlen(value);
    if (value_len == 0) {
        return 0;
    }
    directory_target = is_path_separator(value[value_len - 1]) ||
                       strcmp(value, ".") == 0 || strcmp(value, "..") == 0;
    is_unc = value_len >= 2 && is_path_separator(value[0]) && is_path_separator(value[1]);

    if (is_unc) {
        if (out_len < 3) {
            return 0;
        }
        out[write_pos++] = '\\';
        out[write_pos++] = '\\';
        while (read_pos < value_len && is_path_separator(value[read_pos])) {
            ++read_pos;
        }
    }
    while (read_pos < value_len) {
        char ch = is_path_separator(value[read_pos]) ? '\\' : value[read_pos];
        ++read_pos;
        if (ch == '\\' && write_pos > 0 && out[write_pos - 1] == '\\') {
            continue;
        }
        if (write_pos + 1 >= out_len) {
            return 0;
        }
        out[write_pos++] = ch;
    }
    out[write_pos] = '\0';

    if (directory_target) {
        while (write_pos > 0 && out[write_pos - 1] == '\\') {
            out[--write_pos] = '\0';
        }
        if (write_pos == 0) {
            int written = snprintf(out, out_len, "\\%s", source_name);
            return written >= 0 && (size_t)written < out_len;
        }
        if (write_pos + 1 + strlen(source_name) + 1 > out_len) {
            return 0;
        }
        out[write_pos++] = '\\';
        strcpy(out + write_pos, source_name);
    } else if (strchr(out, '\\') == NULL && strchr(out, ':') == NULL) {
        if (write_pos + 3 > out_len) {
            return 0;
        }
        memmove(out + 2, out, write_pos + 1);
        out[0] = '.';
        out[1] = '\\';
    }
    return 1;
}

static int apply_default_remote_output(Options *opt, const TextData *data) {
    char source_name[MAX_REMOTE_OUTPUT_UTF8];
    char original[MAX_REMOTE_OUTPUT_UTF8];

    if (!source_basename_utf8(opt, data, source_name, sizeof(source_name))) {
        return 0;
    }
    if (opt->remote_output_set) {
        strcpy(original, opt->remote_output);
        if (!normalize_remote_output_value(original, source_name,
                                           opt->remote_output, sizeof(opt->remote_output))) {
            fprintf(stderr, "--remote-output is empty or cannot be normalized.\n");
            return 0;
        }
    } else if (!normalize_remote_output_value(NULL, source_name,
                                               opt->remote_output, sizeof(opt->remote_output))) {
        fprintf(stderr, "The default remote output is too long.\n");
        return 0;
    }
    return 1;
}

static int build_trans_path(wchar_t *out, DWORD out_len) {
    DWORD len;
    wchar_t *slash;

    len = GetModuleFileNameW(NULL, out, out_len);
    if (len == 0 || len >= out_len) {
        return 0;
    }

    slash = wcsrchr(out, L'\\');
    if (slash == NULL) {
        slash = wcsrchr(out, L'/');
    }
    if (slash == NULL) {
        return 0;
    }

    *(slash + 1) = L'\0';
    if (wcslen(out) + wcslen(L"trans.txt") + 1 > out_len) {
        return 0;
    }

    wcscat(out, L"trans.txt");
    return 1;
}

static int resolve_source_path(const Options *opt, wchar_t *out, DWORD out_len) {
    if (opt->has_file_path) {
        if (wcslen(opt->file_path) + 1 > out_len) {
            return 0;
        }
        wcscpy(out, opt->file_path);
        return 1;
    }
    return build_trans_path(out, out_len);
}

static int decode_with_codepage(const unsigned char *bytes, int byte_count, UINT codepage, DWORD flags, wchar_t **out_text, int *out_chars) {
    int needed;
    wchar_t *text;

    needed = MultiByteToWideChar(codepage, flags, (LPCCH)bytes, byte_count, NULL, 0);
    if (needed <= 0) {
        return 0;
    }

    text = (wchar_t *)calloc((size_t)needed + 1, sizeof(wchar_t));
    if (text == NULL) {
        return 0;
    }

    if (MultiByteToWideChar(codepage, flags, (LPCCH)bytes, byte_count, text, needed) != needed) {
        free(text);
        return 0;
    }

    text[needed] = L'\0';
    *out_text = text;
    *out_chars = needed;
    return 1;
}

static int decode_utf16le(const unsigned char *bytes, int byte_count, wchar_t **out_text, int *out_chars) {
    int i;
    int chars;
    wchar_t *text;

    if ((byte_count % 2) != 0) {
        return 0;
    }

    chars = byte_count / 2;
    text = (wchar_t *)calloc((size_t)chars + 1, sizeof(wchar_t));
    if (text == NULL) {
        return 0;
    }

    for (i = 0; i < chars; ++i) {
        text[i] = (wchar_t)(bytes[i * 2] | ((unsigned int)bytes[i * 2 + 1] << 8));
    }
    text[chars] = L'\0';

    *out_text = text;
    *out_chars = chars;
    return 1;
}

static int decode_utf16be(const unsigned char *bytes, int byte_count, wchar_t **out_text, int *out_chars) {
    int i;
    int chars;
    wchar_t *text;

    if ((byte_count % 2) != 0) {
        return 0;
    }

    chars = byte_count / 2;
    text = (wchar_t *)calloc((size_t)chars + 1, sizeof(wchar_t));
    if (text == NULL) {
        return 0;
    }

    for (i = 0; i < chars; ++i) {
        text[i] = (wchar_t)(((unsigned int)bytes[i * 2] << 8) | bytes[i * 2 + 1]);
    }
    text[chars] = L'\0';

    *out_text = text;
    *out_chars = chars;
    return 1;
}

static int scan_directory_recursive(const wchar_t *path, const Options *opt, TextData *data, int depth) {
    wchar_t *pattern = NULL;
    wchar_t *child = NULL;
    WIN32_FIND_DATAW found;
    HANDLE search = INVALID_HANDLE_VALUE;
    size_t path_len;
    int result = 0;

    if (depth > 128) {
        fprintf(stderr, "Directory nesting exceeds the supported depth of 128.\n");
        return 0;
    }
    path_len = wcslen(path);
    if (path_len + 3 >= 32768) {
        fprintf(stderr, "Directory path is too long while scanning.\n");
        return 0;
    }
    pattern = (wchar_t *)calloc(path_len + 3, sizeof(wchar_t));
    if (pattern == NULL) {
        fprintf(stderr, "Not enough memory to scan the source directory.\n");
        return 0;
    }
    wcscpy(pattern, path);
    if (path_len > 0 && path[path_len - 1] != L'\\' && path[path_len - 1] != L'/') {
        wcscat(pattern, L"\\");
    }
    wcscat(pattern, L"*");

    search = FindFirstFileW(pattern, &found);
    if (search == INVALID_HANDLE_VALUE) {
        print_windows_error_a("Cannot enumerate source directory", GetLastError());
        print_wide_line(stderr, "Path: ", path);
        goto cleanup;
    }

    do {
        ULONGLONG file_size;
        size_t child_len;
        if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) {
            continue;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
            fprintf(stderr, "Directory source contains a symbolic link, junction, or reparse point.\n");
            print_wide_line(stderr, "Entry: ", found.cFileName);
            goto cleanup;
        }
        child_len = path_len + wcslen(found.cFileName) + 2;
        if (child_len >= 32768) {
            fprintf(stderr, "Directory entry path is too long.\n");
            goto cleanup;
        }
        child = (wchar_t *)calloc(child_len, sizeof(wchar_t));
        if (child == NULL) {
            fprintf(stderr, "Not enough memory to inspect a directory entry.\n");
            goto cleanup;
        }
        _snwprintf(child, child_len,
                   (path_len > 0 && (path[path_len - 1] == L'\\' || path[path_len - 1] == L'/'))
                       ? L"%ls%ls" : L"%ls\\%ls",
                   path, found.cFileName);
        child[child_len - 1] = L'\0';
        if (child[0] == L'\0') {
            fprintf(stderr, "Could not build a directory entry path.\n");
            goto cleanup;
        }
        if (data->file_count + data->directory_count >= MAX_DIRECTORY_ENTRIES) {
            fprintf(stderr, "Directory contains more than %d entries.\n", MAX_DIRECTORY_ENTRIES);
            goto cleanup;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ++data->directory_count;
            if (!scan_directory_recursive(child, opt, data, depth + 1)) {
                goto cleanup;
            }
            free(child);
            child = NULL;
            continue;
        }
        if (found.dwFileAttributes & FILE_ATTRIBUTE_DEVICE) {
            fprintf(stderr, "Directory source contains an unsupported device entry.\n");
            print_wide_line(stderr, "Entry: ", child);
            goto cleanup;
        }
        file_size = ((ULONGLONG)found.nFileSizeHigh << 32) | found.nFileSizeLow;
        if (file_size > (ULONGLONG)opt->max_bytes ||
            (ULONGLONG)data->bytes + file_size > (ULONGLONG)opt->max_bytes) {
            fprintf(stderr, "Directory file data is larger than --max-bytes.\n");
            goto cleanup;
        }
        data->bytes += (int)file_size;
        ++data->file_count;
        free(child);
        child = NULL;
    } while (FindNextFileW(search, &found));

    if (GetLastError() != ERROR_NO_MORE_FILES) {
        print_windows_error_a("Cannot continue enumerating source directory", GetLastError());
        goto cleanup;
    }
    result = 1;

cleanup:
    free(child);
    free(pattern);
    if (search != INVALID_HANDLE_VALUE) {
        FindClose(search);
    }
    return result;
}

static int read_trans_file(const wchar_t *path, const Options *opt, TextData *data) {
    HANDLE file;
    LARGE_INTEGER size;
    DWORD read_bytes;
    unsigned char *bytes;
    int ok;
    DWORD attributes;
    int preserve_file;

    memset(data, 0, sizeof(*data));
    _snwprintf(data->source_label, (sizeof(data->source_label) / sizeof(data->source_label[0])) - 1, L"File: %ls", path);
    data->source_label[(sizeof(data->source_label) / sizeof(data->source_label[0])) - 1] = L'\0';
    if (wcslen(path) + 1 > sizeof(data->source_path) / sizeof(data->source_path[0])) {
        fprintf(stderr, "Input path is too long.\n");
        return EXIT_FILE;
    }
    wcscpy(data->source_path, path);

    attributes = GetFileAttributesW(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        print_windows_error_a("Cannot inspect input path", GetLastError());
        print_wide_line(stderr, "Path: ", path);
        return EXIT_FILE;
    }
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        fprintf(stderr, "Input path cannot be a symbolic link, junction, or other reparse point.\n");
        return EXIT_FILE;
    }
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) {
        if (opt->transfer_mode != TRANSFER_ZIP_HEX) {
            fprintf(stderr, "Directory sources are only supported with --mode zip-hex.\n");
            return EXIT_ARGS;
        }
        data->kind = DATA_DIRECTORY;
        data->encoding = "directory tree (file bytes preserved)";
        _snwprintf(data->source_label, (sizeof(data->source_label) / sizeof(data->source_label[0])) - 1,
                   L"Directory: %ls", path);
        if (!scan_directory_recursive(path, opt, data, 0)) {
            return EXIT_FILE;
        }
        return EXIT_OK;
    }

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        print_windows_error_a("Cannot open input file", GetLastError());
        print_wide_line(stderr, "Path: ", path);
        return EXIT_FILE;
    }

    if (!GetFileSizeEx(file, &size)) {
        print_windows_error_a("Cannot get input file size", GetLastError());
        CloseHandle(file);
        return EXIT_FILE;
    }

    preserve_file = opt->transfer_mode != TRANSFER_SIMPLE && opt->output_encoding == OUTPUT_PRESERVE;
    if (size.QuadPart == 0 && !preserve_file) {
        fprintf(stderr, "Input file is empty.\n");
        CloseHandle(file);
        return EXIT_FILE;
    }

    if (size.QuadPart > opt->max_bytes) {
        fprintf(stderr, "Input file is too large: %lld bytes. Limit: %d bytes. Use --max-bytes to override.\n",
                (long long)size.QuadPart, opt->max_bytes);
        CloseHandle(file);
        return EXIT_FILE;
    }

    if (size.QuadPart > INT_MAX) {
        fprintf(stderr, "Input file is too large for this build.\n");
        CloseHandle(file);
        return EXIT_FILE;
    }

    bytes = (unsigned char *)malloc(size.QuadPart > 0 ? (size_t)size.QuadPart : 1U);
    if (bytes == NULL) {
        fprintf(stderr, "Not enough memory to read input file.\n");
        CloseHandle(file);
        return EXIT_FILE;
    }

    ok = ReadFile(file, bytes, (DWORD)size.QuadPart, &read_bytes, NULL);
    CloseHandle(file);

    if (!ok || read_bytes != (DWORD)size.QuadPart) {
        print_windows_error_a("Cannot read input file", GetLastError());
        free(bytes);
        return EXIT_FILE;
    }

    data->bytes = (int)size.QuadPart;

    if (preserve_file) {
        data->kind = DATA_FILE_BYTES;
        data->encoding = "raw file bytes (preserved)";
        data->raw_bytes = bytes;
        data->raw_count = data->bytes;
        return EXIT_OK;
    }

    if (data->bytes >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        if (!decode_with_codepage(bytes + 3, data->bytes - 3, CP_UTF8, MB_ERR_INVALID_CHARS, &data->text, &data->chars)) {
            fprintf(stderr, "Input file has a UTF-8 BOM but could not be decoded as UTF-8.\n");
            free(bytes);
            return EXIT_ENCODING;
        }
        data->encoding = "UTF-8 BOM";
    } else if (data->bytes >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        if (!decode_utf16le(bytes + 2, data->bytes - 2, &data->text, &data->chars)) {
            fprintf(stderr, "Input file has a UTF-16 LE BOM but could not be decoded.\n");
            free(bytes);
            return EXIT_ENCODING;
        }
        data->encoding = "UTF-16 LE BOM";
    } else if (data->bytes >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        if (!decode_utf16be(bytes + 2, data->bytes - 2, &data->text, &data->chars)) {
            fprintf(stderr, "Input file has a UTF-16 BE BOM but could not be decoded.\n");
            free(bytes);
            return EXIT_ENCODING;
        }
        data->encoding = "UTF-16 BE BOM";
    } else if (decode_with_codepage(bytes, data->bytes, CP_UTF8, MB_ERR_INVALID_CHARS, &data->text, &data->chars)) {
        data->encoding = "UTF-8";
    } else if (decode_with_codepage(bytes, data->bytes, CP_ACP, 0, &data->text, &data->chars)) {
        data->encoding = "ANSI code page";
    } else {
        fprintf(stderr, "Input file could not be decoded as UTF-8 or the Windows ANSI code page.\n");
        free(bytes);
        return EXIT_ENCODING;
    }

    data->raw_bytes = bytes;
    data->raw_count = data->bytes;
    data->kind = DATA_TEXT;
    return EXIT_OK;
}

static int read_clipboard_source(const Options *opt, TextData *data) {
    HANDLE handle;
    const wchar_t *clip;
    size_t chars;

    memset(data, 0, sizeof(*data));
    wcscpy(data->source_label, L"Clipboard");

    if (!OpenClipboard(NULL)) {
        print_windows_error_a("Cannot open clipboard", GetLastError());
        return EXIT_FILE;
    }

    handle = GetClipboardData(CF_UNICODETEXT);
    if (handle == NULL) {
        CloseClipboard();
        fprintf(stderr, "Clipboard does not contain Unicode text.\n");
        return EXIT_CONTENT;
    }

    clip = (const wchar_t *)GlobalLock(handle);
    if (clip == NULL) {
        CloseClipboard();
        print_windows_error_a("Cannot lock clipboard text", GetLastError());
        return EXIT_FILE;
    }

    chars = wcslen(clip);
    if (chars == 0) {
        GlobalUnlock(handle);
        CloseClipboard();
        fprintf(stderr, "Clipboard text is empty.\n");
        return EXIT_CONTENT;
    }
    if (chars > (size_t)INT_MAX) {
        GlobalUnlock(handle);
        CloseClipboard();
        fprintf(stderr, "Clipboard text is too large for this build.\n");
        return EXIT_CONTENT;
    }
    if (chars * sizeof(wchar_t) > (size_t)opt->max_bytes) {
        GlobalUnlock(handle);
        CloseClipboard();
        fprintf(stderr, "Clipboard text is larger than --max-bytes.\n");
        return EXIT_CONTENT;
    }

    data->text = (wchar_t *)calloc(chars + 1, sizeof(wchar_t));
    if (data->text == NULL) {
        GlobalUnlock(handle);
        CloseClipboard();
        fprintf(stderr, "Not enough memory to read clipboard text.\n");
        return EXIT_FILE;
    }
    memcpy(data->text, clip, chars * sizeof(wchar_t));
    data->text[chars] = L'\0';
    data->chars = (int)chars;
    data->bytes = (int)(chars * sizeof(wchar_t));
    data->encoding = "Windows clipboard Unicode string";

    GlobalUnlock(handle);
    CloseClipboard();
    return EXIT_OK;
}

static int read_text_source(const Options *opt, TextData *data, wchar_t *path, DWORD path_len) {
    if (opt->source == SOURCE_CLIPBOARD) {
        path[0] = L'\0';
        return read_clipboard_source(opt, data);
    }
    if (!resolve_source_path(opt, path, path_len)) {
        fprintf(stderr, "Cannot resolve input file path.\n");
        return EXIT_FILE;
    }
    return read_trans_file(path, opt, data);
}

static void free_text_data(TextData *data) {
    free(data->text);
    free(data->raw_bytes);
    data->text = NULL;
    data->raw_bytes = NULL;
}

static void index_to_line_col(const wchar_t *text, int chars, int index, int *line, int *col) {
    int i;

    *line = 1;
    *col = 1;
    for (i = 0; i < chars && i < index; ++i) {
        if (text[i] == L'\r') {
            ++(*line);
            *col = 1;
            if (i + 1 < chars && i + 1 < index && text[i + 1] == L'\n') {
                ++i;
            }
        } else if (text[i] == L'\n') {
            ++(*line);
            *col = 1;
        } else {
            ++(*col);
        }
    }
}

static TextStats analyze_text(const wchar_t *text, int chars) {
    int i;
    TextStats stats;

    memset(&stats, 0, sizeof(stats));
    stats.lines = 1;
    stats.first_non_ascii = -1;
    stats.first_control = -1;
    stats.first_surrogate_error = -1;

    for (i = 0; i < chars; ++i) {
        wchar_t ch = text[i];

        if (ch == L'\r') {
            ++stats.lines;
            if (i + 1 < chars && text[i + 1] == L'\n') {
                ++i;
            }
            continue;
        }

        if (ch == L'\n') {
            ++stats.lines;
            continue;
        }

        if (ch > 0x7F) {
            ++stats.non_ascii_count;
            if (stats.first_non_ascii < 0) {
                stats.first_non_ascii = i;
            }
        }

        if ((ch < 32 && ch != L'\t') || ch == 127) {
            ++stats.control_count;
            if (stats.first_control < 0) {
                stats.first_control = i;
            }
        }

        if (ch >= 0xD800 && ch <= 0xDBFF) {
            if (i + 1 >= chars || text[i + 1] < 0xDC00 || text[i + 1] > 0xDFFF) {
                ++stats.surrogate_error_count;
                if (stats.first_surrogate_error < 0) {
                    stats.first_surrogate_error = i;
                }
            }
        } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            if (i == 0 || text[i - 1] < 0xD800 || text[i - 1] > 0xDBFF) {
                ++stats.surrogate_error_count;
                if (stats.first_surrogate_error < 0) {
                    stats.first_surrogate_error = i;
                }
            }
        }
    }

    return stats;
}

static int remote_component_is_reserved(const char *component, size_t len) {
    const char *dot = (const char *)memchr(component, '.', len);
    size_t base_len = dot != NULL ? (size_t)(dot - component) : len;
    if (base_len == 3 && (_strnicmp(component, "con", 3) == 0 ||
                          _strnicmp(component, "prn", 3) == 0 ||
                          _strnicmp(component, "aux", 3) == 0 ||
                          _strnicmp(component, "nul", 3) == 0)) {
        return 1;
    }
    return base_len == 4 &&
           (_strnicmp(component, "com", 3) == 0 || _strnicmp(component, "lpt", 3) == 0) &&
           component[3] >= '1' && component[3] <= '9';
}

static int remote_component_valid(const char *component, size_t len) {
    size_t i;
    if (len == 0) {
        return 0;
    }
    if ((len == 1 && component[0] == '.') ||
        (len == 2 && component[0] == '.' && component[1] == '.')) {
        return 1;
    }
    if (component[len - 1] == '.' || component[len - 1] == ' ') {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        unsigned char ch = (unsigned char)component[i];
        if (ch < 32 || ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*') {
            return 0;
        }
    }
    return !remote_component_is_reserved(component, len);
}

static int remote_component_is_temporary(const char *component, size_t len) {
    return (len == strlen(REMOTE_HEX) && _strnicmp(component, REMOTE_HEX, len) == 0) ||
           (len == strlen(REMOTE_ZIP) && _strnicmp(component, REMOTE_ZIP, len) == 0) ||
           (len == strlen(REMOTE_STAGE) && _strnicmp(component, REMOTE_STAGE, len) == 0) ||
           (len == strlen(REMOTE_HELPER_HEX) && _strnicmp(component, REMOTE_HELPER_HEX, len) == 0) ||
           (len == strlen(REMOTE_HELPER) && _strnicmp(component, REMOTE_HELPER, len) == 0);
}

static const char *remote_output_name_ptr(const char *target) {
    const char *slash = strrchr(target, '\\');
    return slash != NULL ? slash + 1 : target;
}

static int remote_output_parts(const char *target, char *parent, size_t parent_len,
                               char *name, size_t name_len) {
    const char *slash = strrchr(target, '\\');
    size_t prefix_len;
    const char *name_start;
    if (slash == NULL) {
        if (parent_len < 2) {
            return 0;
        }
        strcpy(parent, ".");
        name_start = target;
    } else {
        prefix_len = (size_t)(slash - target);
        name_start = slash + 1;
        if (prefix_len == 0) {
            prefix_len = 1;
        } else if (prefix_len == 2 && target[1] == ':') {
            prefix_len = 3;
        }
        if (prefix_len + 1 > parent_len) {
            return 0;
        }
        memcpy(parent, target, prefix_len);
        parent[prefix_len] = '\0';
    }
    if (strlen(name_start) + 1 > name_len) {
        return 0;
    }
    strcpy(name, name_start);
    return 1;
}

static int remote_output_valid(const char *target) {
    const char *cursor;
    const char *component;
    const char *name;
    size_t target_len;
    int wide_units;
    int is_unc;
    int drive_absolute;
    int component_count = 0;

    if (target == NULL || *target == '\0') {
        fprintf(stderr, "--remote-output must not be empty.\n");
        return 0;
    }
    target_len = strlen(target);
    wide_units = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target, -1, NULL, 0);
    if (wide_units <= 0) {
        fprintf(stderr, "--remote-output contains invalid Unicode.\n");
        return 0;
    }
    if (wide_units - 1 > 240) {
        fprintf(stderr, "--remote-output is longer than 240 UTF-16 code units.\n");
        return 0;
    }

    is_unc = target_len >= 2 && target[0] == '\\' && target[1] == '\\';
    drive_absolute = target_len >= 3 && isalpha((unsigned char)target[0]) &&
                     target[1] == ':' && target[2] == '\\';
    if (target_len >= 2 && target[1] == ':' && !drive_absolute) {
        fprintf(stderr, "--remote-output drive paths must be absolute, for example c:/work/file.txt.\n");
        return 0;
    }
    cursor = target + (is_unc ? 2 : (drive_absolute ? 3 : (target[0] == '\\' ? 1 : 0)));
    if (strchr(cursor, ':') != NULL) {
        fprintf(stderr, "--remote-output contains ':' outside a drive prefix.\n");
        return 0;
    }

    component = cursor;
    for (;;) {
        const char *end = strchr(component, '\\');
        size_t len = end != NULL ? (size_t)(end - component) : strlen(component);
        if (!remote_component_valid(component, len)) {
            fprintf(stderr, "--remote-output contains an invalid, empty, trailing-dot, or reserved path component.\n");
            return 0;
        }
        if (remote_component_is_temporary(component, len)) {
            fprintf(stderr, "--remote-output uses a reserved temporary name.\n");
            return 0;
        }
        if (is_unc && ((len == 1 && component[0] == '.') ||
                       (len == 2 && component[0] == '.' && component[1] == '.'))) {
            fprintf(stderr, "--remote-output UNC paths cannot contain '.' or '..' segments.\n");
            return 0;
        }
        ++component_count;
        if (end == NULL) {
            break;
        }
        component = end + 1;
    }
    if (is_unc && component_count < 3) {
        fprintf(stderr, "--remote-output UNC paths must include server, share, and output name.\n");
        return 0;
    }

    name = remote_output_name_ptr(target);
    if (*name == '\0' || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        fprintf(stderr, "--remote-output must end with a file or directory name.\n");
        return 0;
    }
    return 1;
}

static int remote_output_is_shift_free(const char *target) {
    const unsigned char *p = (const unsigned char *)target;
    while (*p != '\0') {
        if ((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
            *p == '.' || *p == '-' || *p == '\\') {
            ++p;
            continue;
        }
        return 0;
    }
    return 1;
}

static const char *output_encoding_name(int encoding) {
    if (encoding == OUTPUT_UTF8_BOM) {
        return "utf8-bom";
    }
    if (encoding == OUTPUT_PRESERVE) {
        return "preserve";
    }
    return "utf8";
}

static int make_output_bytes(const TextData *data, const Options *opt, unsigned char **out_bytes, int *out_count) {
    int needed;
    unsigned char *bytes;

    if (opt->output_encoding == OUTPUT_PRESERVE) {
        if (data->kind != DATA_FILE_BYTES || data->raw_bytes == NULL || data->raw_count < 0) {
            fprintf(stderr, "--output-encoding preserve requires file input.\n");
            return 0;
        }
        bytes = (unsigned char *)malloc(data->raw_count > 0 ? (size_t)data->raw_count : 1U);
        if (bytes == NULL) {
            return 0;
        }
        memcpy(bytes, data->raw_bytes, (size_t)data->raw_count);
        *out_bytes = bytes;
        *out_count = data->raw_count;
        return 1;
    }

    needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data->text, data->chars, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        return 0;
    }
    bytes = (unsigned char *)malloc((size_t)needed + (opt->output_encoding == OUTPUT_UTF8_BOM ? 3U : 0U));
    if (bytes == NULL) {
        return 0;
    }
    if (opt->output_encoding == OUTPUT_UTF8_BOM) {
        bytes[0] = 0xEF;
        bytes[1] = 0xBB;
        bytes[2] = 0xBF;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data->text, data->chars,
                            (LPSTR)(bytes + (opt->output_encoding == OUTPUT_UTF8_BOM ? 3 : 0)),
                            needed, NULL, NULL) != needed) {
        free(bytes);
        return 0;
    }

    *out_bytes = bytes;
    *out_count = needed + (opt->output_encoding == OUTPUT_UTF8_BOM ? 3 : 0);
    return 1;
}

static int sha256_bytes(const unsigned char *bytes, int count, unsigned char digest[32]) {
    BCRYPT_ALG_HANDLE algorithm = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR object = NULL;
    DWORD object_size = 0;
    DWORD result_size = 0;
    NTSTATUS status;
    int ok = 0;

    status = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    if (status < 0) {
        goto cleanup;
    }
    status = BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
                               sizeof(object_size), &result_size, 0);
    if (status < 0 || object_size == 0) {
        goto cleanup;
    }
    object = (PUCHAR)malloc(object_size);
    if (object == NULL) {
        goto cleanup;
    }
    status = BCryptCreateHash(algorithm, &hash, object, object_size, NULL, 0, 0);
    if (status < 0 || BCryptHashData(hash, (PUCHAR)bytes, (ULONG)count, 0) < 0 ||
        BCryptFinishHash(hash, digest, 32, 0) < 0) {
        goto cleanup;
    }
    ok = 1;

cleanup:
    if (hash != NULL) {
        BCryptDestroyHash(hash);
    }
    if (algorithm != NULL) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
    }
    free(object);
    return ok;
}

static void digest_to_hex(const unsigned char digest[32], char out[65]) {
    static const char hex[] = "0123456789abcdef";
    int i;
    for (i = 0; i < 32; ++i) {
        out[i * 2] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0x0F];
    }
    out[64] = '\0';
}

static char *bytes_to_plain_hex(const unsigned char *bytes, int count, int chunk_bytes) {
    size_t line_breaks = (size_t)(count / chunk_bytes) + 2;
    size_t out_len = (size_t)count * 2 + line_breaks + 1;
    char *out = (char *)malloc(out_len);
    size_t pos = 0;
    int i;

    if (out == NULL) {
        return NULL;
    }

    for (i = 0; i < count; ++i) {
        int written = snprintf(out + pos, out_len - pos, "%02x", (unsigned int)bytes[i]);
        if (written != 2) {
            free(out);
            return NULL;
        }
        pos += 2;
        if ((i + 1) % chunk_bytes == 0) {
            out[pos++] = '\n';
        }
    }
    if (pos == 0 || out[pos - 1] != '\n') {
        out[pos++] = '\n';
    }
    out[pos] = '\0';
    return out;
}

static size_t count_hex_lines(const char *hex_text) {
    size_t lines = 0;
    int has_data = 0;
    const char *p;

    for (p = hex_text; *p != '\0'; ++p) {
        if (*p == '\n') {
            if (has_data) {
                ++lines;
                has_data = 0;
            }
        } else {
            has_data = 1;
        }
    }
    return lines + (has_data ? 1U : 0U);
}

static int text_data_from_ascii(const char *text, const char *label, TextData *data) {
    size_t len = strlen(text);
    size_t i;

    memset(data, 0, sizeof(*data));
    data->text = (wchar_t *)calloc(len + 1, sizeof(wchar_t));
    if (data->text == NULL) {
        return 0;
    }
    for (i = 0; i < len; ++i) {
        data->text[i] = (wchar_t)(unsigned char)text[i];
    }
    data->text[len] = L'\0';
    data->chars = (int)len;
    data->bytes = (int)len;
    data->encoding = "generated ASCII command stream";
    MultiByteToWideChar(CP_UTF8, 0, label, -1, data->source_label, (int)(sizeof(data->source_label) / sizeof(data->source_label[0])));
    return 1;
}

typedef struct CommandBuffer {
    char *data;
    size_t cap;
    size_t pos;
} CommandBuffer;

static int command_buffer_init(CommandBuffer *buffer, size_t cap) {
    buffer->data = (char *)malloc(cap);
    if (buffer->data == NULL) {
        return 0;
    }
    buffer->cap = cap;
    buffer->pos = 0;
    buffer->data[0] = '\0';
    return 1;
}

static int command_buffer_appendf(CommandBuffer *buffer, const char *format, ...) {
    va_list args;
    int written;
    va_start(args, format);
    written = vsnprintf(buffer->data + buffer->pos, buffer->cap - buffer->pos, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= buffer->cap - buffer->pos) {
        return 0;
    }
    buffer->pos += (size_t)written;
    return 1;
}

static int command_buffer_append_hex(CommandBuffer *buffer, const char *hex_text,
                                     const char *destination) {
    size_t hex_len = strlen(hex_text);
    size_t start = 0;
    int first_line = 1;
    while (start < hex_len) {
        size_t end = start;
        while (end < hex_len && hex_text[end] != '\n') {
            ++end;
        }
        if (end > start) {
            if (!command_buffer_appendf(buffer, "%s -encoding ascii '%s' '%.*s'\n",
                                        first_line ? "set-content" : "add-content",
                                        destination, (int)(end - start), hex_text + start)) {
                return 0;
            }
            first_line = 0;
        }
        start = end + 1;
    }
    return 1;
}

static char *powershell_script_literal_alloc(const char *value) {
    size_t len = strlen(value);
    char *literal = (char *)malloc(len * 2 + 3);
    size_t read_pos;
    size_t write_pos = 0;
    if (literal == NULL) {
        return NULL;
    }
    literal[write_pos++] = '\'';
    for (read_pos = 0; read_pos < len; ++read_pos) {
        if (value[read_pos] == '\'') {
            literal[write_pos++] = '\'';
        }
        literal[write_pos++] = value[read_pos];
    }
    literal[write_pos++] = '\'';
    literal[write_pos] = '\0';
    return literal;
}

static int build_remote_helper(const Options *opt, int transfer_mode, int directory_source,
                               unsigned char **out_bytes, int *out_count) {
    char parent[MAX_REMOTE_OUTPUT_UTF8];
    char name[MAX_REMOTE_OUTPUT_UTF8];
    char *parent_literal = NULL;
    char *target_literal = NULL;
    char *script = NULL;
    char *escaped = NULL;
    char *batch = NULL;
    const char *prefix = "@echo off\r\nsetlocal disabledelayedexpansion\r\nchcp 65001 >nul\r\n"
                         "powershell -noprofile -noninteractive -command \"";
    const char *suffix = "\"\r\nexit /b %errorlevel%\r\n";
    size_t script_cap;
    size_t escaped_len = 0;
    size_t batch_len;
    size_t i;
    int written;
    int ok = 0;

    if (!remote_output_parts(opt->remote_output, parent, sizeof(parent), name, sizeof(name))) {
        return 0;
    }
    parent_literal = powershell_script_literal_alloc(parent);
    target_literal = powershell_script_literal_alloc(opt->remote_output);
    if (parent_literal == NULL || target_literal == NULL) {
        goto cleanup;
    }
    script_cap = strlen(parent_literal) + strlen(target_literal) * 2 + 1024;
    script = (char *)malloc(script_cap);
    if (script == NULL) {
        goto cleanup;
    }
    if (transfer_mode == TRANSFER_CMD_HEX) {
        written = snprintf(script, script_cap,
                           "[system.io.directory]::createdirectory(%s);"
                           "[system.io.file]::copy('%s',%s,$true);"
                           "certutil -hashfile %s sha256",
                           parent_literal, REMOTE_STAGE, target_literal, target_literal);
    } else if (directory_source) {
        written = snprintf(script, script_cap,
                           "[system.io.directory]::createdirectory(%s);"
                           "certutil -hashfile '%s' sha256;"
                           "expand-archive -force -literalpath '%s' -destinationpath %s",
                           parent_literal, REMOTE_ZIP, REMOTE_ZIP, target_literal);
    } else {
        written = snprintf(script, script_cap,
                           "[system.io.directory]::createdirectory(%s);"
                           "expand-archive -force -literalpath '%s' -destinationpath %s;"
                           "certutil -hashfile %s sha256",
                           parent_literal, REMOTE_ZIP, parent_literal, target_literal);
    }
    if (written < 0 || (size_t)written >= script_cap) {
        goto cleanup;
    }
    for (i = 0; script[i] != '\0'; ++i) {
        escaped_len += (script[i] == '^' || script[i] == '%') ? 2U : 1U;
    }
    escaped = (char *)malloc(escaped_len + 1);
    if (escaped == NULL) {
        goto cleanup;
    }
    escaped_len = 0;
    for (i = 0; script[i] != '\0'; ++i) {
        if (script[i] == '^' || script[i] == '%') {
            escaped[escaped_len++] = script[i];
        }
        escaped[escaped_len++] = script[i];
    }
    escaped[escaped_len] = '\0';
    batch_len = strlen(prefix) + escaped_len + strlen(suffix);
    if (batch_len > INT_MAX) {
        goto cleanup;
    }
    batch = (char *)malloc(batch_len + 1);
    if (batch == NULL) {
        goto cleanup;
    }
    written = snprintf(batch, batch_len + 1, "%s%s%s", prefix, escaped, suffix);
    if (written < 0 || (size_t)written != batch_len) {
        goto cleanup;
    }
    *out_bytes = (unsigned char *)batch;
    *out_count = (int)batch_len;
    batch = NULL;
    ok = 1;

cleanup:
    free(parent_literal);
    free(target_literal);
    free(script);
    free(escaped);
    free(batch);
    return ok;
}

static int command_buffer_append_parent(CommandBuffer *buffer, const Options *opt) {
    char parent[MAX_REMOTE_OUTPUT_UTF8];
    char name[MAX_REMOTE_OUTPUT_UTF8];
    if (!remote_output_parts(opt->remote_output, parent, sizeof(parent), name, sizeof(name))) {
        return 0;
    }
    if (strcmp(parent, ".") == 0) {
        return 1;
    }
    return command_buffer_appendf(buffer, "new-item -itemtype directory -force '%s'\n", parent);
}

static int command_buffer_append_cleanup(CommandBuffer *buffer, const char *name) {
    return command_buffer_appendf(buffer, "remove-item -force '%s'\n", name);
}

static char *build_cmd_hex_commands(const char *hex_text, const Options *opt) {
    int helper = !remote_output_is_shift_free(opt->remote_output);
    unsigned char *helper_bytes = NULL;
    int helper_count = 0;
    char *helper_hex = NULL;
    size_t line_count = count_hex_lines(hex_text);
    size_t helper_lines = 0;
    size_t cap;
    CommandBuffer buffer;
    int has_payload = line_count > 0;
    int ok = 0;

    if (helper) {
        if (!build_remote_helper(opt, TRANSFER_CMD_HEX, 0, &helper_bytes, &helper_count)) {
            return NULL;
        }
        helper_hex = bytes_to_plain_hex(helper_bytes, helper_count, opt->hex_chunk_bytes);
        free(helper_bytes);
        if (helper_hex == NULL) {
            return NULL;
        }
        helper_lines = count_hex_lines(helper_hex);
    }
    cap = strlen(hex_text) + (helper_hex != NULL ? strlen(helper_hex) : 0) +
          (line_count + helper_lines) * 128 + strlen(opt->remote_output) * 4 + 8192;
    if (!command_buffer_init(&buffer, cap)) {
        free(helper_hex);
        return NULL;
    }
    if (!command_buffer_appendf(&buffer, "powershell -noprofile\n") ||
        !command_buffer_append_hex(&buffer, hex_text, REMOTE_HEX)) {
        goto cleanup;
    }
    if (!helper && !command_buffer_append_parent(&buffer, opt)) {
        goto cleanup;
    }
    if (has_payload) {
        if (!command_buffer_appendf(&buffer, "certutil -f -decodehex '%s' '%s'\n",
                                    REMOTE_HEX, helper ? REMOTE_STAGE : opt->remote_output)) {
            goto cleanup;
        }
    } else if (!command_buffer_appendf(&buffer,
                                       "set-content -nonewline '%s' ''\n"
                                       "set-content -nonewline '%s' ''\n",
                                       REMOTE_HEX, helper ? REMOTE_STAGE : opt->remote_output)) {
        goto cleanup;
    }
    if (helper) {
        if (!command_buffer_append_hex(&buffer, helper_hex, REMOTE_HELPER_HEX) ||
            !command_buffer_appendf(&buffer, "certutil -f -decodehex '%s' '%s'\n",
                                    REMOTE_HELPER_HEX, REMOTE_HELPER) ||
            !command_buffer_appendf(&buffer, "cmd /d /c %s\n", REMOTE_HELPER) ||
            !command_buffer_append_cleanup(&buffer, REMOTE_HEX) ||
            !command_buffer_append_cleanup(&buffer, REMOTE_STAGE) ||
            !command_buffer_append_cleanup(&buffer, REMOTE_HELPER_HEX) ||
            !command_buffer_append_cleanup(&buffer, REMOTE_HELPER)) {
            goto cleanup;
        }
    } else if (!command_buffer_appendf(&buffer, "certutil -hashfile '%s' sha256\n",
                                       opt->remote_output) ||
               !command_buffer_append_cleanup(&buffer, REMOTE_HEX)) {
        goto cleanup;
    }
    if (!command_buffer_appendf(&buffer, "exit\n")) {
        goto cleanup;
    }
    ok = 1;

cleanup:
    free(helper_hex);
    if (!ok) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

static char *build_zip_hex_commands(const char *hex_text, const Options *opt, int directory_source) {
    int helper = !remote_output_is_shift_free(opt->remote_output);
    unsigned char *helper_bytes = NULL;
    int helper_count = 0;
    char *helper_hex = NULL;
    size_t line_count = count_hex_lines(hex_text);
    size_t helper_lines = 0;
    size_t cap;
    CommandBuffer buffer;
    char parent[MAX_REMOTE_OUTPUT_UTF8];
    char name[MAX_REMOTE_OUTPUT_UTF8];
    int ok = 0;

    if (!remote_output_parts(opt->remote_output, parent, sizeof(parent), name, sizeof(name))) {
        return NULL;
    }
    if (helper) {
        if (!build_remote_helper(opt, TRANSFER_ZIP_HEX, directory_source,
                                 &helper_bytes, &helper_count)) {
            return NULL;
        }
        helper_hex = bytes_to_plain_hex(helper_bytes, helper_count, opt->hex_chunk_bytes);
        free(helper_bytes);
        if (helper_hex == NULL) {
            return NULL;
        }
        helper_lines = count_hex_lines(helper_hex);
    }
    cap = strlen(hex_text) + (helper_hex != NULL ? strlen(helper_hex) : 0) +
          (line_count + helper_lines) * 128 + strlen(opt->remote_output) * 4 + 8192;
    if (!command_buffer_init(&buffer, cap)) {
        free(helper_hex);
        return NULL;
    }
    if (!command_buffer_appendf(&buffer, "powershell -noprofile\n") ||
        !command_buffer_append_hex(&buffer, hex_text, REMOTE_HEX) ||
        (!helper && !command_buffer_append_parent(&buffer, opt)) ||
        !command_buffer_appendf(&buffer, "certutil -f -decodehex '%s' '%s'\n",
                                REMOTE_HEX, REMOTE_ZIP)) {
        goto cleanup;
    }
    if (helper) {
        if (!command_buffer_append_hex(&buffer, helper_hex, REMOTE_HELPER_HEX) ||
            !command_buffer_appendf(&buffer, "certutil -f -decodehex '%s' '%s'\n",
                                    REMOTE_HELPER_HEX, REMOTE_HELPER) ||
            !command_buffer_appendf(&buffer, "cmd /d /c %s\n", REMOTE_HELPER)) {
            goto cleanup;
        }
    } else if (directory_source) {
        if (!command_buffer_appendf(&buffer,
                                    "certutil -hashfile '%s' sha256\n"
                                    "expand-archive -force '%s' '%s'\n",
                                    REMOTE_ZIP, REMOTE_ZIP, opt->remote_output)) {
            goto cleanup;
        }
    } else if (!command_buffer_appendf(&buffer,
                                       "expand-archive -force '%s' '%s'\n"
                                       "certutil -hashfile '%s' sha256\n",
                                       REMOTE_ZIP, parent, opt->remote_output)) {
        goto cleanup;
    }
    if (!command_buffer_append_cleanup(&buffer, REMOTE_HEX) ||
        !command_buffer_append_cleanup(&buffer, REMOTE_ZIP) ||
        (helper && (!command_buffer_append_cleanup(&buffer, REMOTE_HELPER_HEX) ||
                    !command_buffer_append_cleanup(&buffer, REMOTE_HELPER))) ||
        !command_buffer_appendf(&buffer, "exit\n")) {
        goto cleanup;
    }
    ok = 1;

cleanup:
    free(helper_hex);
    if (!ok) {
        free(buffer.data);
        return NULL;
    }
    return buffer.data;
}

static int generated_command_char_allowed(unsigned char ch) {
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
        return 1;
    }
    return ch == ' ' || ch == '\r' || ch == '\n' || ch == '-' || ch == '.' ||
           ch == '/' || ch == '\\' || ch == '\'';
}

static int validate_generated_commands(const char *commands) {
    int line = 1;
    int col = 1;
    const unsigned char *p = (const unsigned char *)commands;
    while (*p != '\0') {
        if (!generated_command_char_allowed(*p)) {
            fprintf(stderr, "Generated command stream contains forbidden character U+%04X at line %d, column %d.\n",
                    (unsigned int)*p, line, col);
            return EXIT_CONTENT;
        }
        if (*p == '\n') {
            ++line;
            col = 1;
        } else {
            ++col;
        }
        ++p;
    }
    return EXIT_OK;
}

static int write_commands_file(const wchar_t *path, const char *commands) {
    HANDLE file;
    DWORD written = 0;
    size_t len = strlen(commands);
    if (len > MAXDWORD) {
        fprintf(stderr, "Generated command stream is too large to write.\n");
        return EXIT_FILE;
    }
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        print_windows_error_a("Cannot create --commands-out file", GetLastError());
        return EXIT_FILE;
    }
    if (!WriteFile(file, commands, (DWORD)len, &written, NULL) || written != (DWORD)len) {
        print_windows_error_a("Cannot write --commands-out file", GetLastError());
        CloseHandle(file);
        return EXIT_FILE;
    }
    CloseHandle(file);
    return EXIT_OK;
}

static int prepare_generated_commands(const char *commands, const Options *opt) {
    int rc = validate_generated_commands(commands);
    if (rc != EXIT_OK) {
        return rc;
    }
    if (opt->has_commands_out) {
        rc = write_commands_file(opt->commands_out, commands);
        if (rc != EXIT_OK) {
            return rc;
        }
        print_wide_line(stdout, "Generated commands written to: ", opt->commands_out);
    }
    return EXIT_OK;
}

static int ascii_to_wide(const char *src, wchar_t *dest, size_t dest_len) {
    size_t i;

    if (dest_len == 0) {
        return 0;
    }
    for (i = 0; src[i] != '\0' && i + 1 < dest_len; ++i) {
        unsigned char ch = (unsigned char)src[i];
        if (ch > 0x7F) {
            return 0;
        }
        dest[i] = (wchar_t)ch;
    }
    if (src[i] != '\0') {
        return 0;
    }
    dest[i] = L'\0';
    return 1;
}

static int read_binary_file_w(const wchar_t *path, unsigned char **out_bytes, int *out_count) {
    HANDLE file;
    LARGE_INTEGER size;
    unsigned char *buffer;
    DWORD read_count;
    BOOL read_ok;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Could not read generated zip file. Win32 error: %lu\n", GetLastError());
        return 0;
    }
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > INT_MAX) {
        fprintf(stderr, "Generated zip file has an invalid size.\n");
        CloseHandle(file);
        return 0;
    }
    buffer = (unsigned char *)malloc((size_t)size.QuadPart);
    if (buffer == NULL) {
        fprintf(stderr, "Not enough memory to read generated zip file.\n");
        CloseHandle(file);
        return 0;
    }
    read_ok = ReadFile(file, buffer, (DWORD)size.QuadPart, &read_count, NULL);
    CloseHandle(file);
    if (!read_ok || read_count != (DWORD)size.QuadPart) {
        fprintf(stderr, "Could not read all generated zip bytes.\n");
        free(buffer);
        return 0;
    }
    *out_bytes = buffer;
    *out_count = (int)size.QuadPart;
    return 1;
}

static int write_binary_file_w(const wchar_t *path, const unsigned char *bytes, int byte_count) {
    HANDLE file;
    DWORD written;
    BOOL ok;

    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Could not create temporary raw file for zip compression. Win32 error: %lu\n", GetLastError());
        return 0;
    }
    ok = WriteFile(file, bytes, (DWORD)byte_count, &written, NULL);
    CloseHandle(file);
    if (!ok || written != (DWORD)byte_count) {
        fprintf(stderr, "Could not write all temporary raw bytes for zip compression.\n");
        return 0;
    }
    return 1;
}

static int run_local_zip_command(const wchar_t *working_dir, const wchar_t *entry_name) {
    wchar_t command[4096];
    wchar_t escaped_entry[520];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code;
    size_t read_pos;
    size_t write_pos = 0;
    int written;

    for (read_pos = 0; entry_name[read_pos] != L'\0'; ++read_pos) {
        if (entry_name[read_pos] == L'\'') {
            if (write_pos + 2 >= sizeof(escaped_entry) / sizeof(escaped_entry[0])) {
                fprintf(stderr, "Remote output name is too long for local zip compression.\n");
                return 0;
            }
            escaped_entry[write_pos++] = L'\'';
        } else if (write_pos + 1 >= sizeof(escaped_entry) / sizeof(escaped_entry[0])) {
            fprintf(stderr, "Remote output name is too long for local zip compression.\n");
            return 0;
        }
        escaped_entry[write_pos++] = entry_name[read_pos];
    }
    escaped_entry[write_pos] = L'\0';

    written = _snwprintf(
        command,
        sizeof(command) / sizeof(command[0]),
        L"powershell.exe -NoProfile -NonInteractive -Command "
        L"\"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
        L"$z=[System.IO.Compression.ZipFile]::Open('tt.zip','Create'); "
        L"[System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($z,'tt.raw','%ls'); "
        L"$z.Dispose()\"",
        escaped_entry);
    if (written < 0 || written >= (int)(sizeof(command) / sizeof(command[0]))) {
        fprintf(stderr, "Local PowerShell zip command is too long.\n");
        return 0;
    }

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, working_dir, &si, &pi)) {
        fprintf(stderr, "Could not start local PowerShell to create zip. Win32 error: %lu\n", GetLastError());
        return 0;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        exit_code = 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code != 0) {
        fprintf(stderr, "Local PowerShell zip compression failed with exit code %lu.\n", exit_code);
        return 0;
    }
    return 1;
}

static int run_local_directory_zip_command(const wchar_t *working_dir, const wchar_t *source_path) {
    static const wchar_t variable_name[] = L"trans_type_source";
    wchar_t command[] =
        L"powershell.exe -NoProfile -NonInteractive -Command "
        L"\"Add-Type -AssemblyName System.IO.Compression.FileSystem; "
        L"[System.IO.Compression.ZipFile]::CreateFromDirectory($env:trans_type_source,'tt.zip',"
        L"[System.IO.Compression.CompressionLevel]::Optimal,$false)\"";
    wchar_t *old_value = NULL;
    DWORD old_length;
    DWORD old_error;
    int had_old_value = 0;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exit_code;

    SetLastError(ERROR_SUCCESS);
    old_length = GetEnvironmentVariableW(variable_name, NULL, 0);
    old_error = GetLastError();
    if (old_length > 0) {
        old_value = (wchar_t *)calloc(old_length, sizeof(wchar_t));
        if (old_value == NULL || GetEnvironmentVariableW(variable_name, old_value, old_length) == 0) {
            free(old_value);
            fprintf(stderr, "Could not preserve the local compression environment.\n");
            return 0;
        }
        had_old_value = 1;
    } else if (old_error != ERROR_ENVVAR_NOT_FOUND && old_error != ERROR_SUCCESS) {
        fprintf(stderr, "Could not inspect the local compression environment. Win32 error: %lu\n", old_error);
        return 0;
    }

    if (!SetEnvironmentVariableW(variable_name, source_path)) {
        fprintf(stderr, "Could not prepare the local directory path for compression. Win32 error: %lu\n", GetLastError());
        free(old_value);
        return 0;
    }

    memset(&si, 0, sizeof(si));
    memset(&pi, 0, sizeof(pi));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, working_dir, &si, &pi)) {
        DWORD create_error = GetLastError();
        SetEnvironmentVariableW(variable_name, had_old_value ? old_value : NULL);
        free(old_value);
        fprintf(stderr, "Could not start local PowerShell to create directory zip. Win32 error: %lu\n", create_error);
        return 0;
    }
    SetEnvironmentVariableW(variable_name, had_old_value ? old_value : NULL);
    free(old_value);

    WaitForSingleObject(pi.hProcess, INFINITE);
    if (!GetExitCodeProcess(pi.hProcess, &exit_code)) {
        exit_code = 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (exit_code != 0) {
        fprintf(stderr, "Local PowerShell directory compression failed with exit code %lu.\n", exit_code);
        return 0;
    }
    return 1;
}

static int make_local_zip_payload(const unsigned char *bytes,
                                  int byte_count,
                                  const char *remote_output,
                                  unsigned char **zip_bytes,
                                  int *zip_count) {
    wchar_t temp_base[MAX_PATH];
    wchar_t temp_dir[MAX_PATH];
    wchar_t raw_path[MAX_PATH];
    wchar_t zip_path[MAX_PATH];
    wchar_t entry_name[260];
    const char *entry_utf8;
    DWORD len;
    int raw_path_len;
    int zip_path_len;
    int ok = 0;

    entry_utf8 = remote_output_name_ptr(remote_output);
    if (!arg_to_wide_path(entry_utf8, entry_name, sizeof(entry_name) / sizeof(entry_name[0]))) {
        fprintf(stderr, "Remote output name could not be converted for zip entry creation.\n");
        return 0;
    }

    len = GetTempPathW((DWORD)(sizeof(temp_base) / sizeof(temp_base[0])), temp_base);
    if (len == 0 || len >= sizeof(temp_base) / sizeof(temp_base[0])) {
        fprintf(stderr, "Could not locate the Windows temp directory.\n");
        return 0;
    }
    if (!GetTempFileNameW(temp_base, L"ttz", 0, temp_dir)) {
        fprintf(stderr, "Could not reserve a temporary directory name. Win32 error: %lu\n", GetLastError());
        return 0;
    }
    DeleteFileW(temp_dir);
    if (!CreateDirectoryW(temp_dir, NULL)) {
        fprintf(stderr, "Could not create temporary directory for zip compression. Win32 error: %lu\n", GetLastError());
        return 0;
    }

    raw_path_len = _snwprintf(raw_path, sizeof(raw_path) / sizeof(raw_path[0]), L"%ls\\tt.raw", temp_dir);
    zip_path_len = _snwprintf(zip_path, sizeof(zip_path) / sizeof(zip_path[0]), L"%ls\\tt.zip", temp_dir);
    if (raw_path_len < 0 || zip_path_len < 0 ||
        raw_path_len >= (int)(sizeof(raw_path) / sizeof(raw_path[0])) ||
        zip_path_len >= (int)(sizeof(zip_path) / sizeof(zip_path[0]))) {
        fprintf(stderr, "Temporary path is too long for zip compression.\n");
        RemoveDirectoryW(temp_dir);
        return 0;
    }

    if (write_binary_file_w(raw_path, bytes, byte_count) &&
        run_local_zip_command(temp_dir, entry_name) &&
        read_binary_file_w(zip_path, zip_bytes, zip_count)) {
        ok = 1;
    }

    DeleteFileW(raw_path);
    DeleteFileW(zip_path);
    RemoveDirectoryW(temp_dir);
    return ok;
}

static int make_local_directory_zip_payload(const TextData *data,
                                            const Options *opt,
                                            unsigned char **zip_bytes,
                                            int *zip_count) {
    wchar_t temp_base[MAX_PATH];
    wchar_t temp_dir[MAX_PATH];
    wchar_t zip_path[MAX_PATH];
    TextData current;
    DWORD len;
    int zip_path_len;
    int ok = 0;

    memset(&current, 0, sizeof(current));
    if (!scan_directory_recursive(data->source_path, opt, &current, 0)) {
        return 0;
    }
    if (current.bytes != data->bytes || current.file_count != data->file_count ||
        current.directory_count != data->directory_count) {
        fprintf(stderr, "Source directory changed after it was scanned; retry the transfer.\n");
        return 0;
    }

    len = GetTempPathW((DWORD)(sizeof(temp_base) / sizeof(temp_base[0])), temp_base);
    if (len == 0 || len >= sizeof(temp_base) / sizeof(temp_base[0])) {
        fprintf(stderr, "Could not locate the Windows temp directory.\n");
        return 0;
    }
    if (!GetTempFileNameW(temp_base, L"ttd", 0, temp_dir)) {
        fprintf(stderr, "Could not reserve a temporary directory name. Win32 error: %lu\n", GetLastError());
        return 0;
    }
    DeleteFileW(temp_dir);
    if (!CreateDirectoryW(temp_dir, NULL)) {
        fprintf(stderr, "Could not create a temporary directory for folder compression. Win32 error: %lu\n", GetLastError());
        return 0;
    }
    zip_path_len = _snwprintf(zip_path, sizeof(zip_path) / sizeof(zip_path[0]), L"%ls\\tt.zip", temp_dir);
    if (zip_path_len < 0 || zip_path_len >= (int)(sizeof(zip_path) / sizeof(zip_path[0]))) {
        fprintf(stderr, "Temporary zip path is too long.\n");
        RemoveDirectoryW(temp_dir);
        return 0;
    }

    if (run_local_directory_zip_command(temp_dir, data->source_path) &&
        read_binary_file_w(zip_path, zip_bytes, zip_count)) {
        ok = 1;
    }
    DeleteFileW(zip_path);
    RemoveDirectoryW(temp_dir);
    return ok;
}

static int shift_free_simple_char(wchar_t ch) {
    if ((ch >= L'a' && ch <= L'z') || (ch >= L'0' && ch <= L'9')) {
        return 1;
    }
    return wcschr(L" `-=[]\\;',./", ch) != NULL;
}

static int validate_text(const TextData *data, const TextStats *stats, const Options *opt) {
    int i;
    int line;
    int col;

    if (data->kind == DATA_DIRECTORY && opt->transfer_mode != TRANSFER_ZIP_HEX) {
        fprintf(stderr, "Directory sources are only supported with --mode zip-hex.\n");
        return EXIT_ARGS;
    }
    if (data->kind == DATA_FILE_BYTES &&
        (opt->transfer_mode == TRANSFER_SIMPLE || opt->output_encoding != OUTPUT_PRESERVE)) {
        fprintf(stderr, "Raw file bytes require cmd-hex or zip-hex with --output-encoding preserve.\n");
        return EXIT_ARGS;
    }

    if (data->kind == DATA_TEXT && data->chars == 0) {
        fprintf(stderr, "Input decoded to empty text.\n");
        return EXIT_CONTENT;
    }

    if (data->kind == DATA_TEXT && stats->control_count > 0) {
        index_to_line_col(data->text, data->chars, stats->first_control, &line, &col);
        fprintf(stderr, "Input contains unsupported control characters. First one at line %d, column %d.\n", line, col);
        fprintf(stderr, "Allowed control characters are tab and newlines only.\n");
        return EXIT_CONTENT;
    }

    if (data->kind == DATA_TEXT && stats->surrogate_error_count > 0) {
        index_to_line_col(data->text, data->chars, stats->first_surrogate_error, &line, &col);
        fprintf(stderr, "Input contains invalid UTF-16 surrogate data. First issue at line %d, column %d.\n", line, col);
        return EXIT_CONTENT;
    }

    if (data->kind == DATA_TEXT && opt->ascii_only && stats->non_ascii_count > 0) {
        index_to_line_col(data->text, data->chars, stats->first_non_ascii, &line, &col);
        fprintf(stderr, "--ascii-only is enabled, but input has non-ASCII characters. First one at line %d, column %d.\n", line, col);
        return EXIT_CONTENT;
    }

    if (opt->transfer_mode == TRANSFER_CMD_HEX || opt->transfer_mode == TRANSFER_ZIP_HEX) {
        if (opt->output_encoding == OUTPUT_PRESERVE && data->kind != DATA_FILE_BYTES &&
            data->kind != DATA_DIRECTORY) {
            fprintf(stderr, "--output-encoding preserve requires file input; clipboard text has no original bytes.\n");
            return EXIT_ARGS;
        }
        if (!remote_output_valid(opt->remote_output)) {
            return EXIT_ARGS;
        }
        return EXIT_OK;
    }

    if (opt->has_commands_out) {
        fprintf(stderr, "--commands-out is only valid with --mode cmd-hex or --mode zip-hex.\n");
        return EXIT_ARGS;
    }

    for (i = 0; i < data->chars; ++i) {
        wchar_t ch = data->text[i];
        if (ch == L'\r' || ch == L'\n' || ch == L'\t' || shift_free_simple_char(ch)) {
            continue;
        }
        index_to_line_col(data->text, data->chars, i, &line, &col);
        fprintf(stderr, "Simple mode rejects U+%04X at line %d, column %d because it is uppercase, Unicode, or requires a modifier.\n",
                (unsigned int)ch, line, col);
        fprintf(stderr, "Use --mode cmd-hex or --mode zip-hex.\n");
        return EXIT_CONTENT;
    }

    return EXIT_OK;
}

static void print_summary(const TextData *data, const TextStats *stats, const Options *opt) {
    print_wide_line(stdout, "Source: ", data->source_label);
    printf("Encoding: %s\n", data->encoding);
    printf("Bytes: %d\n", data->bytes);
    if (data->kind == DATA_DIRECTORY) {
        printf("Directory entries: %d file(s), %d subdirectory(s)\n", data->file_count, data->directory_count);
    } else if (data->kind == DATA_FILE_BYTES) {
        printf("Content: arbitrary regular file bytes\n");
    } else {
        printf("Characters: %d\n", data->chars);
        printf("Lines: %d\n", stats->lines);
        printf("Non-ASCII characters: %d\n", stats->non_ascii_count);
    }
    printf("Delay: %d ms per character, %d ms per line\n", opt->delay_ms, opt->line_delay_ms);
    printf("Focus check: %s\n", opt->no_focus_check ? "off" : "on");
    if (opt->transfer_mode == TRANSFER_CMD_HEX) {
        printf("Mode: cmd-hex transfer through remote cmd/certutil\n");
        printf("Output encoding: %s\n", output_encoding_name(opt->output_encoding));
        printf("Remote output: %s\n", opt->remote_output);
        printf("Path transport: %s\n",
               remote_output_is_shift_free(opt->remote_output) ? "direct" : "hex helper");
        printf("Hex chunk: %d bytes per generated line\n", opt->hex_chunk_bytes);
    } else if (opt->transfer_mode == TRANSFER_ZIP_HEX) {
        printf("Mode: zip-hex transfer through remote PowerShell/certutil/Expand-Archive\n");
        if (data->kind == DATA_DIRECTORY) {
            printf("Output encoding: directory file bytes preserved\n");
            printf("Remote destination directory: %s\n", opt->remote_output);
        } else {
            printf("Output encoding: %s\n", output_encoding_name(opt->output_encoding));
            printf("Remote output: %s\n", opt->remote_output);
        }
        printf("Path transport: %s\n",
               remote_output_is_shift_free(opt->remote_output) ? "direct" : "hex helper");
        printf("Hex chunk: %d bytes per generated line\n", opt->hex_chunk_bytes);
    } else if (opt->legacy_keys) {
        printf("Input mode: Legacy keybd_event ASCII keys\n");
    } else if (opt->ascii_keys) {
        printf("Input mode: ASCII virtual keys\n");
    } else {
        printf("Input mode: Unicode SendInput\n");
    }
    if (opt->transfer_mode == TRANSFER_CMD_HEX || opt->transfer_mode == TRANSFER_ZIP_HEX) {
        printf("Enter mode: physical Return key (forced for complex mode)\n");
    } else if (opt->legacy_keys) {
        printf("Enter mode: legacy VK_RETURN\n");
    } else {
        printf("Enter mode: %s\n", opt->enter_unicode ? "Unicode CR" : "VK_RETURN");
    }
}

static int wait_for_console_enter_or_esc(const char *message) {
    int ch;

    puts(message);
    puts("Press Enter in this console to start a new countdown, or Esc to abort.");
    for (;;) {
        ch = _getch();
        if (ch == 27) {
            return EXIT_ABORTED;
        }
        if (ch == '\r' || ch == '\n') {
            return EXIT_OK;
        }
    }
}

static int run_countdown(int seconds) {
    int remaining;

    if (seconds <= 0) {
        return EXIT_OK;
    }

    puts("");
    puts("Focus the target window now.");
    puts("Esc aborts. Press Enter during the countdown to start immediately.");

    GetAsyncKeyState(VK_ESCAPE);
    GetAsyncKeyState(VK_RETURN);
    while (GetAsyncKeyState(VK_RETURN) & 0x8000) {
        Sleep(20);
    }
    GetAsyncKeyState(VK_ESCAPE);
    GetAsyncKeyState(VK_RETURN);

    for (remaining = seconds; remaining > 0; --remaining) {
        int ticks;
        printf("\rStarting in %d second(s)... ", remaining);
        fflush(stdout);
        for (ticks = 0; ticks < 10; ++ticks) {
            if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
                printf("\n");
                return EXIT_ABORTED;
            }
            if (GetAsyncKeyState(VK_RETURN) & 0x8000) {
                printf("\n");
                return EXIT_OK;
            }
            Sleep(100);
        }
    }

    printf("\rStarting now.               \n");
    return EXIT_OK;
}

static void capture_foreground_window(TargetWindow *target) {
    target->hwnd = GetForegroundWindow();
    target->pid = 0;
    target->title[0] = L'\0';

    if (target->hwnd != NULL) {
        GetWindowThreadProcessId(target->hwnd, &target->pid);
        GetWindowTextW(target->hwnd, target->title, (int)(sizeof(target->title) / sizeof(target->title[0])));
        target->title[(sizeof(target->title) / sizeof(target->title[0])) - 1] = L'\0';
    }
}

static void print_target_window(const TargetWindow *target) {
    if (target->hwnd == NULL) {
        puts("Target window: <none>");
        return;
    }

    if (target->title[0] != L'\0') {
        char *title = wide_to_utf8_alloc(target->title);
        if (title != NULL) {
            printf("Target window: \"%s\" (pid %lu)\n", title, (unsigned long)target->pid);
            free(title);
        } else {
            printf("Target window: <unprintable Unicode title> (pid %lu)\n", (unsigned long)target->pid);
        }
    } else {
        printf("Target window: <untitled> (pid %lu)\n", (unsigned long)target->pid);
    }
}

static int prepare_target_window(const Options *opt, TargetWindow *target) {
    DWORD self_pid = GetCurrentProcessId();

    for (;;) {
        int rc = run_countdown(opt->start_delay_sec);
        if (rc != EXIT_OK) {
            return rc;
        }

        capture_foreground_window(target);
        print_target_window(target);

        if (GetKeyState(VK_CAPITAL) & 0x0001) {
            rc = wait_for_console_enter_or_esc("Caps Lock is on. Turn it off before typing.");
            if (rc != EXIT_OK) {
                return rc;
            }
            continue;
        }
        if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
            (GetAsyncKeyState(VK_MENU) & 0x8000)) {
            rc = wait_for_console_enter_or_esc("A modifier key is held. Release Shift, Ctrl, and Alt before typing.");
            if (rc != EXIT_OK) {
                return rc;
            }
            continue;
        }

        if (target->hwnd == NULL) {
            rc = wait_for_console_enter_or_esc("No foreground window was detected.");
            if (rc != EXIT_OK) {
                return rc;
            }
            continue;
        }

        if (target->pid == self_pid) {
            rc = wait_for_console_enter_or_esc("The tool console is still the foreground window. It will not type into itself.");
            if (rc != EXIT_OK) {
                return rc;
            }
            continue;
        }

        return EXIT_OK;
    }
}

static int send_input_checked(INPUT *inputs, UINT count, const char *what) {
    UINT sent;
    DWORD err;

    sent = SendInput(count, inputs, sizeof(INPUT));
    if (sent != count) {
        err = GetLastError();
        fprintf(stderr, "SendInput failed while typing %s. Sent %u of %u events.\n", what, (unsigned int)sent, (unsigned int)count);
        if (err != ERROR_SUCCESS) {
            print_windows_error_a("SendInput detail", err);
        } else {
            fprintf(stderr, "This can happen when the target runs at a higher integrity level. Try running this tool as Administrator.\n");
        }
        return EXIT_INPUT;
    }

    return EXIT_OK;
}

static int send_vk(WORD vk, const char *what) {
    INPUT inputs[2];

    ZeroMemory(inputs, sizeof(inputs));
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    return send_input_checked(inputs, 2, what);
}

static int send_legacy_vk(WORD vk) {
    keybd_event((BYTE)vk, 0, 0, 0);
    keybd_event((BYTE)vk, 0, KEYEVENTF_KEYUP, 0);
    return EXIT_OK;
}

static int run_sendinput_self_test(void) {
    TargetWindow target;
    int rc;

    puts("Running native SendInput self-test with a harmless F24 key press.");
    puts("This does not type visible text.");
    printf("sizeof(INPUT): %u bytes\n", (unsigned int)sizeof(INPUT));
    printf("sizeof(KEYBDINPUT): %u bytes\n", (unsigned int)sizeof(KEYBDINPUT));
    capture_foreground_window(&target);
    print_target_window(&target);

    rc = send_vk(VK_F24, "self-test F24");
    if (rc != EXIT_OK) {
        puts("Trying legacy keybd_event F24 test. This API has no reliable success return value.");
        (void)send_legacy_vk(VK_F24);
        puts("Legacy keybd_event call completed. If it also has no effect, the system/session is blocking synthetic input.");
        return rc;
    }

    puts("Self-test passed: SendInput accepted the F24 key events.");
    return EXIT_OK;
}

static int run_legacy_self_test(void) {
    TargetWindow target;

    puts("Running legacy keybd_event self-test with a harmless F24 key press.");
    puts("This does not type visible text.");
    puts("Note: keybd_event does not report whether the target accepted the event.");
    capture_foreground_window(&target);
    print_target_window(&target);
    (void)send_legacy_vk(VK_F24);
    puts("Legacy self-test completed. To verify visible typing, run the Python --debug-input mode against Notepad or the RDP target.");
    return EXIT_OK;
}

static int run_self_test(const Options *opt) {
    if (opt->legacy_keys) {
        return run_legacy_self_test();
    }
    return run_sendinput_self_test();
}

static int send_unicode_unit(wchar_t ch, const char *what) {
    INPUT inputs[2];

    ZeroMemory(inputs, sizeof(inputs));
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wScan = (WORD)ch;
    inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
    inputs[1] = inputs[0];
    inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;

    return send_input_checked(inputs, 2, what);
}

static int send_ascii_char(wchar_t ch) {
    SHORT vk_scan;
    BYTE vk;
    BYTE shift_state;
    INPUT inputs[8];
    UINT count = 0;

    vk_scan = VkKeyScanW(ch);
    if (vk_scan == -1) {
        fprintf(stderr, "Cannot map ASCII character U+%04X through the current keyboard layout.\n", (unsigned int)ch);
        return EXIT_INPUT;
    }

    vk = (BYTE)(vk_scan & 0xFF);
    shift_state = (BYTE)((vk_scan >> 8) & 0xFF);
    if (shift_state != 0) {
        fprintf(stderr, "Character U+%04X requires Shift/Ctrl/Alt in the current keyboard layout. Use cmd-hex or zip-hex.\n",
                (unsigned int)ch);
        return EXIT_INPUT;
    }

    ZeroMemory(inputs, sizeof(inputs));

    inputs[count].type = INPUT_KEYBOARD;
    inputs[count].ki.wVk = vk;
    ++count;

    inputs[count] = inputs[count - 1];
    inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
    ++count;

    return send_input_checked(inputs, count, "ASCII character");
}

static int send_legacy_ascii_char(wchar_t ch) {
    SHORT vk_scan;
    BYTE vk;
    BYTE shift_state;

    vk_scan = VkKeyScanW(ch);
    if (vk_scan == -1) {
        fprintf(stderr, "Cannot map ASCII character U+%04X through the current keyboard layout.\n", (unsigned int)ch);
        return EXIT_INPUT;
    }

    vk = (BYTE)(vk_scan & 0xFF);
    shift_state = (BYTE)((vk_scan >> 8) & 0xFF);
    if (shift_state != 0) {
        fprintf(stderr, "Character U+%04X requires Shift/Ctrl/Alt in the current keyboard layout. Use cmd-hex or zip-hex.\n",
                (unsigned int)ch);
        return EXIT_INPUT;
    }

    (void)send_legacy_vk(vk);

    return EXIT_OK;
}

static int send_enter(const Options *opt) {
    if (opt->legacy_keys) {
        return send_legacy_vk(VK_RETURN);
    }
    if (opt->enter_unicode) {
        return send_unicode_unit(L'\r', "Enter");
    }
    return send_vk(VK_RETURN, "Enter");
}

static int pause_and_recapture(const Options *opt, TargetWindow *target, const char *reason) {
    int rc;

    printf("\nPaused: %s\n", reason);
    rc = wait_for_console_enter_or_esc("Typing is paused.");
    if (rc != EXIT_OK) {
        return rc;
    }
    return prepare_target_window(opt, target);
}

static int check_runtime_controls(const Options *opt, TargetWindow *target) {
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        return EXIT_ABORTED;
    }

    if (!opt->no_focus_check && GetForegroundWindow() != target->hwnd) {
        return pause_and_recapture(opt, target, "Foreground window changed.");
    }

    if (GetKeyState(VK_CAPITAL) & 0x0001) {
        return pause_and_recapture(opt, target, "Caps Lock was enabled.");
    }
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) || (GetAsyncKeyState(VK_CONTROL) & 0x8000) ||
        (GetAsyncKeyState(VK_MENU) & 0x8000)) {
        return pause_and_recapture(opt, target, "A modifier key is held. Release Shift, Ctrl, and Alt.");
    }

    return EXIT_OK;
}

static int wait_typing_delay(const Options *opt, TargetWindow *target, int delay_ms) {
    int remaining = delay_ms;
    while (remaining > 0) {
        int slice = remaining > 10 ? 10 : remaining;
        int rc = check_runtime_controls(opt, target);
        if (rc != EXIT_OK) {
            return rc;
        }
        Sleep((DWORD)slice);
        remaining -= slice;
    }
    return EXIT_OK;
}

static int type_text(const TextData *data, const TextStats *stats, const Options *opt, TargetWindow *target) {
    int i;
    int line = 1;
    int col = 1;
    int typed_chars = 0;
    int rc;

    printf("\nTyping started. Esc aborts.\n");
    printf("Progress: line %d/%d\n", line, stats->lines);

    for (i = 0; i < data->chars; ++i) {
        wchar_t ch = data->text[i];

        rc = check_runtime_controls(opt, target);
        if (rc != EXIT_OK) {
            if (rc == EXIT_ABORTED) {
                printf("\nAborted at line %d, column %d after %d character(s).\n", line, col, typed_chars);
            }
            return rc;
        }

        if (ch == L'\r') {
            if (i + 1 < data->chars && data->text[i + 1] == L'\n') {
                ++i;
            }
            rc = send_enter(opt);
            if (rc != EXIT_OK) {
                fprintf(stderr, "Input failed at line %d, column %d.\n", line, col);
                return rc;
            }
            ++line;
            col = 1;
            ++typed_chars;
            printf("\rProgress: line %d/%d", line <= stats->lines ? line : stats->lines, stats->lines);
            fflush(stdout);
            rc = wait_typing_delay(opt, target, opt->line_delay_ms);
            if (rc != EXIT_OK) {
                return rc;
            }
            continue;
        }

        if (ch == L'\n') {
            rc = send_enter(opt);
            if (rc != EXIT_OK) {
                fprintf(stderr, "Input failed at line %d, column %d.\n", line, col);
                return rc;
            }
            ++line;
            col = 1;
            ++typed_chars;
            printf("\rProgress: line %d/%d", line <= stats->lines ? line : stats->lines, stats->lines);
            fflush(stdout);
            rc = wait_typing_delay(opt, target, opt->line_delay_ms);
            if (rc != EXIT_OK) {
                return rc;
            }
            continue;
        }

        if (ch == L'\t') {
            if (opt->legacy_keys) {
                rc = send_legacy_vk(VK_TAB);
            } else {
                rc = send_vk(VK_TAB, "Tab");
            }
        } else if (opt->legacy_keys) {
            rc = send_legacy_ascii_char(ch);
        } else if (opt->ascii_keys) {
            rc = send_ascii_char(ch);
        } else {
            rc = send_unicode_unit(ch, "Unicode character");
        }

        if (rc != EXIT_OK) {
            fprintf(stderr, "Input failed at line %d, column %d.\n", line, col);
            return rc;
        }

        ++col;
        ++typed_chars;
        rc = wait_typing_delay(opt, target, opt->delay_ms);
        if (rc != EXIT_OK) {
            return rc;
        }
    }

    printf("\rProgress: line %d/%d\n", stats->lines, stats->lines);
    printf("Typing completed. Typed %d character/event unit(s).\n", typed_chars);
    return EXIT_OK;
}

static int type_generated_ascii(const char *text, const char *label, const Options *base_opt, TargetWindow *target) {
    TextData generated;
    TextStats stats;
    Options opt = *base_opt;
    int rc;

    if (!text_data_from_ascii(text, label, &generated)) {
        fprintf(stderr, "Not enough memory to build generated command stream.\n");
        return EXIT_FILE;
    }

    opt.transfer_mode = TRANSFER_SIMPLE;
    opt.has_commands_out = 0;
    opt.enter_unicode = 0;
    opt.remote_output_set = 0;
    if (!opt.ascii_keys) {
        opt.legacy_keys = 1;
        opt.ascii_keys = 0;
    }

    stats = analyze_text(generated.text, generated.chars);
    rc = validate_text(&generated, &stats, &opt);
    if (rc == EXIT_OK) {
        rc = type_text(&generated, &stats, &opt, target);
    }
    free_text_data(&generated);
    return rc;
}

static int run_cmd_hex_transfer(const TextData *data, const Options *opt) {
    unsigned char *bytes = NULL;
    char *hex_text = NULL;
    char *commands = NULL;
    TargetWindow target;
    Options key_opt = *opt;
    unsigned char digest[32];
    char digest_hex[65];
    int byte_count = 0;
    size_t hex_line_count = 0;
    int rc;

    if (!make_output_bytes(data, opt, &bytes, &byte_count)) {
        fprintf(stderr, "Input could not be converted to the selected output encoding.\n");
        return EXIT_ENCODING;
    }
    if (!sha256_bytes(bytes, byte_count, digest)) {
        fprintf(stderr, "Could not calculate SHA-256.\n");
        free(bytes);
        return EXIT_FILE;
    }
    digest_to_hex(digest, digest_hex);

    hex_text = bytes_to_plain_hex(bytes, byte_count, opt->hex_chunk_bytes);
    free(bytes);
    if (hex_text == NULL) {
        fprintf(stderr, "Not enough memory to build hex payload.\n");
        return EXIT_FILE;
    }
    hex_line_count = count_hex_lines(hex_text);

    printf("cmd-hex transfer mode.\n");
    printf("Output bytes to transfer: %d\n", byte_count);
    printf("Remote output: %s\n", opt->remote_output);
    printf("Hex chunk: %d bytes per generated line (%u line(s))\n", opt->hex_chunk_bytes, (unsigned int)hex_line_count);
    printf("Expected SHA-256: %s\n", digest_hex);
    printf("Remote temporary %s is deleted after reconstruction.\n", REMOTE_HEX);
    printf("Focus a remote cmd.exe or PowerShell prompt. This mode uses PowerShell Set-Content/Add-Content, not redirection or Ctrl+Z/F6.\n");

    if (!key_opt.ascii_keys) {
        key_opt.legacy_keys = 1;
        key_opt.ascii_keys = 0;
    }

    commands = build_cmd_hex_commands(hex_text, opt);
    if (commands == NULL) {
        free(hex_text);
        fprintf(stderr, "Not enough memory to build cmd-hex commands.\n");
        return EXIT_FILE;
    }

    rc = prepare_generated_commands(commands, opt);
    if (rc != EXIT_OK) {
        free(hex_text);
        free(commands);
        return rc;
    }

    if (opt->dry_run) {
        printf("Generated hex characters: %u\n", (unsigned int)(strlen(hex_text) - hex_line_count));
        printf("Generated command characters: %u\n", (unsigned int)strlen(commands));
        puts("Dry run passed. No typing was performed.");
        free(hex_text);
        free(commands);
        return EXIT_OK;
    }

    rc = prepare_target_window(opt, &target);
    if (rc != EXIT_OK) {
        free(hex_text);
        free(commands);
        return rc;
    }

    rc = type_generated_ascii(commands, "cmd-hex echo/certutil commands", &key_opt, &target);
    free(hex_text);
    free(commands);
    if (rc != EXIT_OK) {
        return rc;
    }

    puts("cmd-hex transfer typing completed. Verify the remote output file before running it.");
    return EXIT_OK;
}

static int run_zip_hex_transfer(const TextData *data, const Options *opt) {
    unsigned char *bytes = NULL;
    unsigned char *zip_bytes = NULL;
    char *hex_text = NULL;
    char *commands = NULL;
    TargetWindow target;
    Options key_opt = *opt;
    unsigned char digest[32];
    char digest_hex[65];
    int byte_count = 0;
    int zip_count = 0;
    size_t hex_line_count = 0;
    double ratio;
    int directory_source = data->kind == DATA_DIRECTORY;
    int rc;

    if (directory_source) {
        byte_count = data->bytes;
        if (!make_local_directory_zip_payload(data, opt, &zip_bytes, &zip_count)) {
            return EXIT_FILE;
        }
        if (!sha256_bytes(zip_bytes, zip_count, digest)) {
            fprintf(stderr, "Could not calculate directory archive SHA-256.\n");
            free(zip_bytes);
            return EXIT_FILE;
        }
    } else {
        if (!make_output_bytes(data, opt, &bytes, &byte_count)) {
            fprintf(stderr, "Input could not be converted to the selected output encoding.\n");
            return EXIT_ENCODING;
        }
        if (!sha256_bytes(bytes, byte_count, digest)) {
            fprintf(stderr, "Could not calculate SHA-256.\n");
            free(bytes);
            return EXIT_FILE;
        }
        if (!make_local_zip_payload(bytes, byte_count, opt->remote_output, &zip_bytes, &zip_count)) {
            free(bytes);
            return EXIT_FILE;
        }
        free(bytes);
    }
    digest_to_hex(digest, digest_hex);

    hex_text = bytes_to_plain_hex(zip_bytes, zip_count, opt->hex_chunk_bytes);
    free(zip_bytes);
    if (hex_text == NULL) {
        fprintf(stderr, "Not enough memory to build zip hex payload.\n");
        return EXIT_FILE;
    }
    hex_line_count = count_hex_lines(hex_text);
    ratio = byte_count > 0 ? ((double)zip_count / (double)byte_count) * 100.0 : 0.0;

    printf("zip-hex transfer mode.\n");
    printf("Output bytes before zip: %d\n", byte_count);
    printf("Zip bytes to transfer: %d (%.2f%% of output size)\n", zip_count, ratio);
    printf("Remote output: %s\n", opt->remote_output);
    printf("Hex chunk: %d bytes per generated line (%u line(s))\n", opt->hex_chunk_bytes, (unsigned int)hex_line_count);
    if (directory_source) {
        printf("Expected archive SHA-256: %s\n", digest_hex);
        printf("The remote destination is merged/overwritten; unrelated existing files are not removed.\n");
    } else {
        printf("Expected SHA-256: %s\n", digest_hex);
    }
    printf("Remote temporary %s and %s are deleted after extraction.\n", REMOTE_HEX, REMOTE_ZIP);
    printf("Focus a remote cmd.exe or PowerShell prompt. This mode decodes a zip, then runs Expand-Archive.\n");

    if (!key_opt.ascii_keys) {
        key_opt.legacy_keys = 1;
        key_opt.ascii_keys = 0;
    }

    commands = build_zip_hex_commands(hex_text, opt, directory_source);
    if (commands == NULL) {
        free(hex_text);
        fprintf(stderr, "Not enough memory to build zip-hex commands.\n");
        return EXIT_FILE;
    }

    rc = prepare_generated_commands(commands, opt);
    if (rc != EXIT_OK) {
        free(hex_text);
        free(commands);
        return rc;
    }

    if (opt->dry_run) {
        printf("Generated hex characters: %u\n", (unsigned int)(strlen(hex_text) - hex_line_count));
        printf("Generated command characters: %u\n", (unsigned int)strlen(commands));
        puts("Dry run passed. No typing was performed.");
        free(hex_text);
        free(commands);
        return EXIT_OK;
    }

    rc = prepare_target_window(opt, &target);
    if (rc != EXIT_OK) {
        free(hex_text);
        free(commands);
        return rc;
    }

    rc = type_generated_ascii(commands, "zip-hex expand-archive commands", &key_opt, &target);
    free(hex_text);
    free(commands);
    if (rc != EXIT_OK) {
        return rc;
    }

    puts("zip-hex transfer typing completed. Verify the remote output file before running it.");
    return EXIT_OK;
}

int main(int argc, char **argv) {
    Options opt;
    TextData data;
    TextStats stats;
    TargetWindow target;
    wchar_t source_path[32768];
    int rc;

    setlocale(LC_ALL, "");
    SetConsoleOutputCP(CP_UTF8);

    init_options(&opt);
    if (!parse_args(argc, argv, &opt)) {
        puts("");
        print_usage();
        return EXIT_ARGS;
    }

    if (opt.self_test) {
        return run_self_test(&opt);
    }

    rc = read_text_source(&opt, &data, source_path, (DWORD)(sizeof(source_path) / sizeof(source_path[0])));
    if (rc != EXIT_OK) {
        return rc;
    }
    if (!apply_default_remote_output(&opt, &data)) {
        free_text_data(&data);
        return EXIT_ARGS;
    }

    stats = analyze_text(data.text, data.chars);
    print_summary(&data, &stats, &opt);

    rc = validate_text(&data, &stats, &opt);
    if (rc != EXIT_OK) {
        free_text_data(&data);
        return rc;
    }

    if (opt.transfer_mode == TRANSFER_CMD_HEX) {
        rc = run_cmd_hex_transfer(&data, &opt);
        free_text_data(&data);
        return rc;
    }
    if (opt.transfer_mode == TRANSFER_ZIP_HEX) {
        rc = run_zip_hex_transfer(&data, &opt);
        free_text_data(&data);
        return rc;
    }

    if (opt.dry_run) {
        puts("Dry run passed. No typing was performed.");
        free_text_data(&data);
        return EXIT_OK;
    }

    rc = prepare_target_window(&opt, &target);
    if (rc == EXIT_OK) {
        rc = type_text(&data, &stats, &opt, &target);
    }

    free_text_data(&data);
    return rc;
}
