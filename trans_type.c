#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>
#include <ctype.h>
#include <limits.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define DEFAULT_DELAY_MS 20
#define DEFAULT_LINE_DELAY_MS 100
#define DEFAULT_START_DELAY_SEC 5
#define DEFAULT_MAX_BYTES (1024 * 1024)
#define ABSOLUTE_MAX_BYTES (100 * 1024 * 1024)

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
    TRANSFER_CMD_HEX = 1
};

typedef struct Options {
    int delay_ms;
    int line_delay_ms;
    int start_delay_sec;
    int source;
    int transfer_mode;
    int ascii_only;
    int ascii_keys;
    int legacy_keys;
    int no_focus_check;
    int dry_run;
    int self_test;
    int enter_unicode;
    int max_bytes;
    int has_file_path;
    wchar_t file_path[32768];
    char remote_output[260];
    char remote_hex[260];
} Options;

typedef struct TextData {
    wchar_t *text;
    int chars;
    int bytes;
    const char *encoding;
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
    opt->ascii_only = 0;
    opt->ascii_keys = 0;
    opt->legacy_keys = 1;
    opt->no_focus_check = 0;
    opt->dry_run = 0;
    opt->self_test = 0;
    opt->enter_unicode = 0;
    opt->max_bytes = DEFAULT_MAX_BYTES;
    opt->has_file_path = 0;
    opt->file_path[0] = L'\0';
    strcpy(opt->remote_output, "trans.txt");
    strcpy(opt->remote_hex, "tt.hex");
}

static void print_usage(void) {
    puts(PROGRAM_NAME " - type trans.txt or clipboard text into the current foreground window");
    puts("");
    puts("Usage:");
    puts("  " PROGRAM_NAME " [options]");
    puts("");
    puts("Options:");
    puts("  --source file         Read text from trans.txt next to this executable. Default");
    puts("  --source clipboard    Read text from the Windows clipboard");
    puts("  --file PATH           Read text from PATH. Implies --source file");
    puts("  --mode simple         Type the source text directly through keyboard events. Default");
    puts("  --mode cmd-hex        Type a cmd/certutil hex transfer to recreate the text file");
    puts("  --remote-output PATH  Remote output file for --mode cmd-hex. Default: trans.txt");
    puts("  --remote-hex PATH     Remote temporary hex file for --mode cmd-hex. Default: tt.hex");
    puts("  --delay-ms N          Delay after each character. Default: 20");
    puts("  --line-delay-ms N     Delay after each line. Default: 100");
    puts("  --start-delay-sec N   Countdown before typing. Default: 5");
    puts("  --max-bytes N         Maximum input size. Default: 1048576");
    puts("  --ascii-only          Refuse to type non-ASCII characters");
    puts("  --ascii-keys          Type ASCII through SendInput virtual keys");
    puts("  --legacy-keys         Type ASCII through legacy keybd_event. Default");
    puts("  --sendinput           Use Unicode SendInput instead of default legacy keybd_event");
    puts("  --enter-mode key      Send Enter as VK_RETURN. Default");
    puts("  --enter-mode unicode  Send Enter as Unicode carriage return");
    puts("  --no-focus-check      Do not pause when the foreground window changes");
    puts("  --dry-run             Parse and validate the source text without typing");
    puts("  --self-test           Test whether the selected input mode can send a harmless Shift key event");
    puts("  --help                Show this help");
    puts("");
    puts("Controls while running:");
    puts("  Esc                   Abort");
    puts("  Pause/Break           Pause and resume through a new countdown");
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

static int copy_remote_path(char *dest, size_t dest_len, const char *value, const char *name) {
    size_t len;

    if (value == NULL || *value == '\0') {
        fprintf(stderr, "%s must not be empty.\n", name);
        return 0;
    }
    len = strlen(value);
    if (len >= dest_len) {
        fprintf(stderr, "%s is too long.\n", name);
        return 0;
    }
    memcpy(dest, value, len + 1);
    return 1;
}

static int parse_args(int argc, char **argv, Options *opt) {
    int i;

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

        matched = get_option_value(&i, argc, argv, "--source", &value);
        if (matched == 1) {
            if (strcmp(value, "file") == 0) {
                opt->source = SOURCE_FILE;
            } else if (strcmp(value, "clipboard") == 0) {
                opt->source = SOURCE_CLIPBOARD;
            } else {
                fprintf(stderr, "Invalid --source value: %s\n", value);
                return 0;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--file", &value);
        if (matched == 1) {
            opt->source = SOURCE_FILE;
            opt->has_file_path = 1;
            if (!arg_to_wide_path(value, opt->file_path, sizeof(opt->file_path) / sizeof(opt->file_path[0]))) {
                fprintf(stderr, "Invalid --file path: %s\n", value);
                return 0;
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
            opt->transfer_mode = TRANSFER_CMD_HEX;
            if (!copy_remote_path(opt->remote_output, sizeof(opt->remote_output), value, "--remote-output")) {
                return 0;
            }
            continue;
        } else if (matched == 0) {
            return 0;
        }

        matched = get_option_value(&i, argc, argv, "--remote-hex", &value);
        if (matched == 1) {
            opt->transfer_mode = TRANSFER_CMD_HEX;
            if (!copy_remote_path(opt->remote_hex, sizeof(opt->remote_hex), value, "--remote-hex")) {
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

static int read_trans_file(const wchar_t *path, const Options *opt, TextData *data) {
    HANDLE file;
    LARGE_INTEGER size;
    DWORD read_bytes;
    unsigned char *bytes;
    int ok;

    memset(data, 0, sizeof(*data));
    _snwprintf(data->source_label, (sizeof(data->source_label) / sizeof(data->source_label[0])) - 1, L"File: %ls", path);
    data->source_label[(sizeof(data->source_label) / sizeof(data->source_label[0])) - 1] = L'\0';

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        print_windows_error_a("Cannot open input file", GetLastError());
        fwprintf(stderr, L"Path: %ls\n", path);
        return EXIT_FILE;
    }

    if (!GetFileSizeEx(file, &size)) {
        print_windows_error_a("Cannot get input file size", GetLastError());
        CloseHandle(file);
        return EXIT_FILE;
    }

    if (size.QuadPart == 0) {
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

    bytes = (unsigned char *)malloc((size_t)size.QuadPart);
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

    free(bytes);
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

static int is_safe_cmd_hex_path(const char *path) {
    const unsigned char *p = (const unsigned char *)path;

    if (path == NULL || *path == '\0') {
        return 0;
    }
    while (*p) {
        unsigned char ch = *p++;
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            continue;
        }
        if (ch == '.' || ch == '-' || ch == '/' || ch == '\\') {
            continue;
        }
        return 0;
    }
    return 1;
}

static int text_to_utf8_bytes(const TextData *data, unsigned char **out_bytes, int *out_count) {
    int needed;
    unsigned char *bytes;

    needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data->text, data->chars, NULL, 0, NULL, NULL);
    if (needed <= 0) {
        return 0;
    }
    bytes = (unsigned char *)malloc((size_t)needed);
    if (bytes == NULL) {
        return 0;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data->text, data->chars, (LPSTR)bytes, needed, NULL, NULL) != needed) {
        free(bytes);
        return 0;
    }

    *out_bytes = bytes;
    *out_count = needed;
    return 1;
}

static char *bytes_to_plain_hex(const unsigned char *bytes, int count) {
    size_t line_breaks = (size_t)(count / 24) + 2;
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
        if ((i + 1) % 24 == 0) {
            out[pos++] = '\n';
        }
    }
    if (pos == 0 || out[pos - 1] != '\n') {
        out[pos++] = '\n';
    }
    out[pos] = '\0';
    return out;
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

static int validate_text(const TextData *data, const TextStats *stats, const Options *opt) {
    int line;
    int col;

    if (data->chars == 0) {
        fprintf(stderr, "Input decoded to empty text.\n");
        return EXIT_CONTENT;
    }

    if (stats->control_count > 0) {
        index_to_line_col(data->text, data->chars, stats->first_control, &line, &col);
        fprintf(stderr, "Input contains unsupported control characters. First one at line %d, column %d.\n", line, col);
        fprintf(stderr, "Allowed control characters are tab and newlines only.\n");
        return EXIT_CONTENT;
    }

    if (stats->surrogate_error_count > 0) {
        index_to_line_col(data->text, data->chars, stats->first_surrogate_error, &line, &col);
        fprintf(stderr, "Input contains invalid UTF-16 surrogate data. First issue at line %d, column %d.\n", line, col);
        return EXIT_CONTENT;
    }

    if (opt->ascii_only && stats->non_ascii_count > 0) {
        index_to_line_col(data->text, data->chars, stats->first_non_ascii, &line, &col);
        fprintf(stderr, "--ascii-only is enabled, but input has non-ASCII characters. First one at line %d, column %d.\n", line, col);
        return EXIT_CONTENT;
    }

    if (opt->transfer_mode == TRANSFER_CMD_HEX) {
        if (!is_safe_cmd_hex_path(opt->remote_output)) {
            fprintf(stderr, "--remote-output must be a relative cmd-safe path using only lowercase letters, digits, '.', '-', '/', or '\\'.\n");
            fprintf(stderr, "Example: trans.txt\n");
            return EXIT_ARGS;
        }
        if (!is_safe_cmd_hex_path(opt->remote_hex)) {
            fprintf(stderr, "--remote-hex must be a relative cmd-safe path using only lowercase letters, digits, '.', '-', '/', or '\\'.\n");
            fprintf(stderr, "Example: tt.hex\n");
            return EXIT_ARGS;
        }
        if (strcmp(opt->remote_output, opt->remote_hex) == 0) {
            fprintf(stderr, "--remote-output and --remote-hex must be different files.\n");
            return EXIT_ARGS;
        }
        return EXIT_OK;
    }

    if ((opt->ascii_keys || opt->legacy_keys) && stats->non_ascii_count > 0) {
        index_to_line_col(data->text, data->chars, stats->first_non_ascii, &line, &col);
        fprintf(stderr, "Legacy/ASCII key modes cannot type non-ASCII characters. First one at line %d, column %d.\n", line, col);
        fprintf(stderr, "Use --sendinput for Unicode input if this Windows session allows SendInput.\n");
        return EXIT_CONTENT;
    }

    return EXIT_OK;
}

static void print_summary(const TextData *data, const TextStats *stats, const Options *opt) {
    fwprintf(stdout, L"Source: %ls\n", data->source_label);
    printf("Encoding: %s\n", data->encoding);
    printf("Bytes: %d\n", data->bytes);
    printf("Characters: %d\n", data->chars);
    printf("Lines: %d\n", stats->lines);
    printf("Non-ASCII characters: %d\n", stats->non_ascii_count);
    printf("Delay: %d ms per character, %d ms per line\n", opt->delay_ms, opt->line_delay_ms);
    printf("Focus check: %s\n", opt->no_focus_check ? "off" : "on");
    if (opt->transfer_mode == TRANSFER_CMD_HEX) {
        printf("Mode: cmd-hex transfer through remote cmd/certutil\n");
        printf("Remote output: %s\n", opt->remote_output);
        printf("Remote temporary hex: %s\n", opt->remote_hex);
    } else if (opt->legacy_keys) {
        printf("Input mode: Legacy keybd_event ASCII keys\n");
    } else if (opt->ascii_keys) {
        printf("Input mode: ASCII virtual keys\n");
    } else {
        printf("Input mode: Unicode SendInput\n");
    }
    if (opt->legacy_keys) {
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
        fwprintf(stdout, L"Target window: \"%ls\" (pid %lu)\n", target->title, (unsigned long)target->pid);
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

    puts("Running native SendInput self-test with a harmless Shift key press.");
    puts("This does not type visible text.");
    printf("sizeof(INPUT): %u bytes\n", (unsigned int)sizeof(INPUT));
    printf("sizeof(KEYBDINPUT): %u bytes\n", (unsigned int)sizeof(KEYBDINPUT));
    capture_foreground_window(&target);
    print_target_window(&target);

    rc = send_vk(VK_SHIFT, "self-test Shift");
    if (rc != EXIT_OK) {
        puts("Trying legacy keybd_event Shift test. This API has no reliable success return value.");
        (void)send_legacy_vk(VK_SHIFT);
        puts("Legacy keybd_event call completed. If it also has no effect, the system/session is blocking synthetic input.");
        return rc;
    }

    puts("Self-test passed: SendInput accepted the Shift key events.");
    return EXIT_OK;
}

static int run_legacy_self_test(void) {
    TargetWindow target;

    puts("Running legacy keybd_event self-test with a harmless Shift key press.");
    puts("This does not type visible text.");
    puts("Note: keybd_event does not report whether the target accepted the event.");
    capture_foreground_window(&target);
    print_target_window(&target);
    (void)send_legacy_vk(VK_SHIFT);
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

    ZeroMemory(inputs, sizeof(inputs));

    if (shift_state & 1) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = VK_SHIFT;
        ++count;
    }
    if (shift_state & 2) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = VK_CONTROL;
        ++count;
    }
    if (shift_state & 4) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = VK_MENU;
        ++count;
    }

    inputs[count].type = INPUT_KEYBOARD;
    inputs[count].ki.wVk = vk;
    ++count;

    inputs[count] = inputs[count - 1];
    inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
    ++count;

    if (shift_state & 4) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = VK_MENU;
        inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
        ++count;
    }
    if (shift_state & 2) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = VK_CONTROL;
        inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
        ++count;
    }
    if (shift_state & 1) {
        inputs[count].type = INPUT_KEYBOARD;
        inputs[count].ki.wVk = VK_SHIFT;
        inputs[count].ki.dwFlags = KEYEVENTF_KEYUP;
        ++count;
    }

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

    if (shift_state & 1) {
        keybd_event(VK_SHIFT, 0, 0, 0);
    }
    if (shift_state & 2) {
        keybd_event(VK_CONTROL, 0, 0, 0);
    }
    if (shift_state & 4) {
        keybd_event(VK_MENU, 0, 0, 0);
    }

    (void)send_legacy_vk(vk);

    if (shift_state & 4) {
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
    }
    if (shift_state & 2) {
        keybd_event(VK_CONTROL, 0, KEYEVENTF_KEYUP, 0);
    }
    if (shift_state & 1) {
        keybd_event(VK_SHIFT, 0, KEYEVENTF_KEYUP, 0);
    }

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

    if (GetAsyncKeyState(VK_PAUSE) & 0x0001) {
        return pause_and_recapture(opt, target, "Pause/Break was pressed.");
    }

    if (!opt->no_focus_check) {
        HWND current = GetForegroundWindow();
        if (current != target->hwnd) {
            return pause_and_recapture(opt, target, "Foreground window changed.");
        }
    }

    return EXIT_OK;
}

static int type_text(const TextData *data, const TextStats *stats, const Options *opt, TargetWindow *target) {
    int i;
    int line = 1;
    int col = 1;
    int typed_chars = 0;
    int rc;

    printf("\nTyping started. Esc aborts, Pause/Break pauses.\n");
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
            if (opt->line_delay_ms > 0) {
                Sleep((DWORD)opt->line_delay_ms);
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
            if (opt->line_delay_ms > 0) {
                Sleep((DWORD)opt->line_delay_ms);
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
        if (opt->delay_ms > 0) {
            Sleep((DWORD)opt->delay_ms);
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
    if (!opt.ascii_keys) {
        opt.legacy_keys = 1;
        opt.ascii_keys = 0;
    }

    stats = analyze_text(generated.text, generated.chars);
    rc = validate_text(&generated, &stats, &opt);
    if (rc == EXIT_OK) {
        rc = type_text(&generated, &stats, &opt, target);
    }
    free(generated.text);
    return rc;
}

static int send_eof_key(const Options *base_opt) {
    if (base_opt->ascii_keys) {
        return send_vk(VK_F6, "F6 EOF");
    }
    return send_legacy_vk(VK_F6);
}

static int run_cmd_hex_transfer(const TextData *data, const Options *opt) {
    unsigned char *bytes = NULL;
    char *hex_text = NULL;
    char *setup = NULL;
    char *decode = NULL;
    TargetWindow target;
    Options key_opt = *opt;
    int byte_count = 0;
    int rc;
    int needed;

    if (!text_to_utf8_bytes(data, &bytes, &byte_count)) {
        fprintf(stderr, "Input text could not be encoded as UTF-8.\n");
        return EXIT_ENCODING;
    }

    hex_text = bytes_to_plain_hex(bytes, byte_count);
    free(bytes);
    if (hex_text == NULL) {
        fprintf(stderr, "Not enough memory to build hex payload.\n");
        return EXIT_FILE;
    }

    printf("cmd-hex transfer mode.\n");
    printf("UTF-8 bytes to transfer: %d\n", byte_count);
    printf("Remote output: %s\n", opt->remote_output);
    printf("Remote temporary hex: %s\n", opt->remote_hex);
    printf("Focus a remote cmd.exe or PowerShell prompt. The tool first types: cmd /q /d\n");

    if (!key_opt.ascii_keys) {
        key_opt.legacy_keys = 1;
        key_opt.ascii_keys = 0;
    }

    if (opt->dry_run) {
        printf("Generated hex characters: %u\n", (unsigned int)strlen(hex_text));
        puts("Dry run passed. No typing was performed.");
        free(hex_text);
        return EXIT_OK;
    }

    rc = prepare_target_window(opt, &target);
    if (rc != EXIT_OK) {
        free(hex_text);
        return rc;
    }

    needed = snprintf(NULL, 0, "cmd /q /d\ncopy con %s\n", opt->remote_hex);
    setup = (char *)malloc((size_t)needed + 1);
    if (setup == NULL) {
        free(hex_text);
        fprintf(stderr, "Not enough memory to build setup commands.\n");
        return EXIT_FILE;
    }
    snprintf(setup, (size_t)needed + 1, "cmd /q /d\ncopy con %s\n", opt->remote_hex);

    rc = type_generated_ascii(setup, "cmd-hex setup commands", &key_opt, &target);
    free(setup);
    if (rc != EXIT_OK) {
        free(hex_text);
        return rc;
    }

    rc = type_generated_ascii(hex_text, "cmd-hex payload", &key_opt, &target);
    free(hex_text);
    if (rc != EXIT_OK) {
        return rc;
    }

    rc = send_eof_key(&key_opt);
    if (rc != EXIT_OK) {
        fprintf(stderr, "Input failed while sending F6 EOF to finish copy con.\n");
        return rc;
    }
    if (opt->line_delay_ms > 0) {
        Sleep((DWORD)opt->line_delay_ms);
    }
    rc = send_enter(&key_opt);
    if (rc != EXIT_OK) {
        fprintf(stderr, "Input failed while sending Enter after F6 EOF.\n");
        return rc;
    }
    if (opt->line_delay_ms > 0) {
        Sleep((DWORD)opt->line_delay_ms);
    }

    needed = snprintf(NULL, 0, "certutil -f -decodehex %s %s\ndel %s\nexit\n", opt->remote_hex, opt->remote_output, opt->remote_hex);
    decode = (char *)malloc((size_t)needed + 1);
    if (decode == NULL) {
        fprintf(stderr, "Not enough memory to build decode commands.\n");
        return EXIT_FILE;
    }
    snprintf(decode, (size_t)needed + 1, "certutil -f -decodehex %s %s\ndel %s\nexit\n", opt->remote_hex, opt->remote_output, opt->remote_hex);

    rc = type_generated_ascii(decode, "cmd-hex decode commands", &key_opt, &target);
    free(decode);
    if (rc != EXIT_OK) {
        return rc;
    }

    puts("cmd-hex transfer typing completed. Verify the remote output file before running it.");
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

    stats = analyze_text(data.text, data.chars);
    print_summary(&data, &stats, &opt);

    rc = validate_text(&data, &stats, &opt);
    if (rc != EXIT_OK) {
        free(data.text);
        return rc;
    }

    if (opt.transfer_mode == TRANSFER_CMD_HEX) {
        rc = run_cmd_hex_transfer(&data, &opt);
        free(data.text);
        return rc;
    }

    if (opt.dry_run) {
        puts("Dry run passed. No typing was performed.");
        free(data.text);
        return EXIT_OK;
    }

    if (stats.non_ascii_count > 0) {
        puts("");
        printf("Warning: trans.txt contains %d non-ASCII character(s).\n", stats.non_ascii_count);
        puts("Unicode input usually works through RDP, but some target applications may reject it.");
    }

    rc = prepare_target_window(&opt, &target);
    if (rc == EXIT_OK) {
        rc = type_text(&data, &stats, &opt, &target);
    }

    free(data.text);
    return rc;
}
