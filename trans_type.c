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

typedef struct Options {
    int delay_ms;
    int line_delay_ms;
    int start_delay_sec;
    int ascii_only;
    int ascii_keys;
    int no_focus_check;
    int dry_run;
    int self_test;
    int enter_unicode;
    int max_bytes;
} Options;

typedef struct TextData {
    wchar_t *text;
    int chars;
    int bytes;
    const char *encoding;
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
    opt->ascii_only = 0;
    opt->ascii_keys = 0;
    opt->no_focus_check = 0;
    opt->dry_run = 0;
    opt->self_test = 0;
    opt->enter_unicode = 0;
    opt->max_bytes = DEFAULT_MAX_BYTES;
}

static void print_usage(void) {
    puts(PROGRAM_NAME " - type trans.txt into the current foreground window");
    puts("");
    puts("Usage:");
    puts("  " PROGRAM_NAME " [options]");
    puts("");
    puts("Options:");
    puts("  --delay-ms N          Delay after each character. Default: 20");
    puts("  --line-delay-ms N     Delay after each line. Default: 100");
    puts("  --start-delay-sec N   Countdown before typing. Default: 5");
    puts("  --max-bytes N         Maximum trans.txt size. Default: 1048576");
    puts("  --ascii-only          Refuse to type non-ASCII characters");
    puts("  --ascii-keys          Type ASCII through virtual keys instead of Unicode input");
    puts("  --enter-mode key      Send Enter as VK_RETURN. Default");
    puts("  --enter-mode unicode  Send Enter as Unicode carriage return");
    puts("  --no-focus-check      Do not pause when the foreground window changes");
    puts("  --dry-run             Parse and validate trans.txt without typing");
    puts("  --self-test           Test whether SendInput accepts a harmless Shift key event");
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

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        print_windows_error_a("Cannot open trans.txt", GetLastError());
        fwprintf(stderr, L"Path: %ls\n", path);
        return EXIT_FILE;
    }

    if (!GetFileSizeEx(file, &size)) {
        print_windows_error_a("Cannot get trans.txt size", GetLastError());
        CloseHandle(file);
        return EXIT_FILE;
    }

    if (size.QuadPart == 0) {
        fprintf(stderr, "trans.txt is empty.\n");
        CloseHandle(file);
        return EXIT_FILE;
    }

    if (size.QuadPart > opt->max_bytes) {
        fprintf(stderr, "trans.txt is too large: %lld bytes. Limit: %d bytes. Use --max-bytes to override.\n",
                (long long)size.QuadPart, opt->max_bytes);
        CloseHandle(file);
        return EXIT_FILE;
    }

    if (size.QuadPart > INT_MAX) {
        fprintf(stderr, "trans.txt is too large for this build.\n");
        CloseHandle(file);
        return EXIT_FILE;
    }

    bytes = (unsigned char *)malloc((size_t)size.QuadPart);
    if (bytes == NULL) {
        fprintf(stderr, "Not enough memory to read trans.txt.\n");
        CloseHandle(file);
        return EXIT_FILE;
    }

    ok = ReadFile(file, bytes, (DWORD)size.QuadPart, &read_bytes, NULL);
    CloseHandle(file);

    if (!ok || read_bytes != (DWORD)size.QuadPart) {
        print_windows_error_a("Cannot read trans.txt", GetLastError());
        free(bytes);
        return EXIT_FILE;
    }

    data->bytes = (int)size.QuadPart;

    if (data->bytes >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        if (!decode_with_codepage(bytes + 3, data->bytes - 3, CP_UTF8, MB_ERR_INVALID_CHARS, &data->text, &data->chars)) {
            fprintf(stderr, "trans.txt has a UTF-8 BOM but could not be decoded as UTF-8.\n");
            free(bytes);
            return EXIT_ENCODING;
        }
        data->encoding = "UTF-8 BOM";
    } else if (data->bytes >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        if (!decode_utf16le(bytes + 2, data->bytes - 2, &data->text, &data->chars)) {
            fprintf(stderr, "trans.txt has a UTF-16 LE BOM but could not be decoded.\n");
            free(bytes);
            return EXIT_ENCODING;
        }
        data->encoding = "UTF-16 LE BOM";
    } else if (data->bytes >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        if (!decode_utf16be(bytes + 2, data->bytes - 2, &data->text, &data->chars)) {
            fprintf(stderr, "trans.txt has a UTF-16 BE BOM but could not be decoded.\n");
            free(bytes);
            return EXIT_ENCODING;
        }
        data->encoding = "UTF-16 BE BOM";
    } else if (decode_with_codepage(bytes, data->bytes, CP_UTF8, MB_ERR_INVALID_CHARS, &data->text, &data->chars)) {
        data->encoding = "UTF-8";
    } else if (decode_with_codepage(bytes, data->bytes, CP_ACP, 0, &data->text, &data->chars)) {
        data->encoding = "ANSI code page";
    } else {
        fprintf(stderr, "trans.txt could not be decoded as UTF-8 or the Windows ANSI code page.\n");
        free(bytes);
        return EXIT_ENCODING;
    }

    free(bytes);
    return EXIT_OK;
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

static int validate_text(const TextData *data, const TextStats *stats, const Options *opt) {
    int line;
    int col;

    if (data->chars == 0) {
        fprintf(stderr, "trans.txt decoded to empty text.\n");
        return EXIT_CONTENT;
    }

    if (stats->control_count > 0) {
        index_to_line_col(data->text, data->chars, stats->first_control, &line, &col);
        fprintf(stderr, "trans.txt contains unsupported control characters. First one at line %d, column %d.\n", line, col);
        fprintf(stderr, "Allowed control characters are tab and newlines only.\n");
        return EXIT_CONTENT;
    }

    if (stats->surrogate_error_count > 0) {
        index_to_line_col(data->text, data->chars, stats->first_surrogate_error, &line, &col);
        fprintf(stderr, "trans.txt contains invalid UTF-16 surrogate data. First issue at line %d, column %d.\n", line, col);
        return EXIT_CONTENT;
    }

    if (opt->ascii_only && stats->non_ascii_count > 0) {
        index_to_line_col(data->text, data->chars, stats->first_non_ascii, &line, &col);
        fprintf(stderr, "--ascii-only is enabled, but trans.txt has non-ASCII characters. First one at line %d, column %d.\n", line, col);
        return EXIT_CONTENT;
    }

    if (opt->ascii_keys && stats->non_ascii_count > 0) {
        index_to_line_col(data->text, data->chars, stats->first_non_ascii, &line, &col);
        fprintf(stderr, "--ascii-keys cannot type non-ASCII characters. First one at line %d, column %d.\n", line, col);
        return EXIT_CONTENT;
    }

    return EXIT_OK;
}

static void print_summary(const wchar_t *path, const TextData *data, const TextStats *stats, const Options *opt) {
    fwprintf(stdout, L"File: %ls\n", path);
    printf("Encoding: %s\n", data->encoding);
    printf("Bytes: %d\n", data->bytes);
    printf("Characters: %d\n", data->chars);
    printf("Lines: %d\n", stats->lines);
    printf("Non-ASCII characters: %d\n", stats->non_ascii_count);
    printf("Delay: %d ms per character, %d ms per line\n", opt->delay_ms, opt->line_delay_ms);
    printf("Focus check: %s\n", opt->no_focus_check ? "off" : "on");
    printf("Input mode: %s\n", opt->ascii_keys ? "ASCII virtual keys" : "Unicode SendInput");
    printf("Enter mode: %s\n", opt->enter_unicode ? "Unicode CR" : "VK_RETURN");
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

static void send_legacy_vk(WORD vk) {
    keybd_event((BYTE)vk, 0, 0, 0);
    keybd_event((BYTE)vk, 0, KEYEVENTF_KEYUP, 0);
}

static int run_self_test(void) {
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
        send_legacy_vk(VK_SHIFT);
        puts("Legacy keybd_event call completed. If it also has no effect, the system/session is blocking synthetic input.");
        return rc;
    }

    puts("Self-test passed: SendInput accepted the Shift key events.");
    return EXIT_OK;
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

static int send_enter(const Options *opt) {
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
            rc = send_vk(VK_TAB, "Tab");
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

int main(int argc, char **argv) {
    Options opt;
    TextData data;
    TextStats stats;
    TargetWindow target;
    wchar_t trans_path[32768];
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
        return run_self_test();
    }

    if (!build_trans_path(trans_path, (DWORD)(sizeof(trans_path) / sizeof(trans_path[0])))) {
        fprintf(stderr, "Cannot build path to trans.txt next to the executable.\n");
        return EXIT_FILE;
    }

    rc = read_trans_file(trans_path, &opt, &data);
    if (rc != EXIT_OK) {
        return rc;
    }

    stats = analyze_text(data.text, data.chars);
    print_summary(trans_path, &data, &stats, &opt);

    rc = validate_text(&data, &stats, &opt);
    if (rc != EXIT_OK) {
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
