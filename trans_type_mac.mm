#import <ApplicationServices/ApplicationServices.h>
#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
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
#include <vector>

#define PROGRAM_NAME "trans_type_mac"

static constexpr int EXIT_OK = 0;
static constexpr int EXIT_ARGS = 2;
static constexpr int EXIT_FILE = 3;
static constexpr int EXIT_CONTENT = 5;
static constexpr int EXIT_ABORTED = 6;
static constexpr int EXIT_INPUT = 7;

static constexpr int DEFAULT_DELAY_MS = 20;
static constexpr int DEFAULT_LINE_DELAY_MS = 100;
static constexpr int DEFAULT_START_DELAY_SEC = 5;
static constexpr int DEFAULT_MAX_BYTES = 1024 * 1024;
static constexpr int ABSOLUTE_MAX_BYTES = 100 * 1024 * 1024;

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
    std::filesystem::path file_path;
};

struct TextData {
    std::u16string text;
    size_t byte_count = 0;
    std::string encoding;
    std::string source_label;
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
};

static void print_usage() {
    puts(PROGRAM_NAME " - type clipboard or trans.txt into the current foreground macOS window");
    puts("");
    puts("Usage:");
    puts("  " PROGRAM_NAME " [options]");
    puts("");
    puts("Default behavior:");
    puts("  Reads the macOS clipboard. ASCII text uses real virtual keys for RDP;");
    puts("  non-ASCII text uses Unicode CGEvent payloads for local macOS apps.");
    puts("");
    puts("Options:");
    puts("  --source clipboard    Read text from the macOS clipboard. Default");
    puts("  --source file         Read text from trans.txt next to this binary");
    puts("  --file PATH           Read text from PATH. Implies --source file");
    puts("  --input-mode auto     ASCII virtual keys when possible, Unicode otherwise. Default");
    puts("  --input-mode keys     Type ASCII through real virtual keys. Best for RDP");
    puts("  --input-mode unicode  Type through Unicode CGEvent payloads. Best for local macOS apps");
    puts("  --delay-ms N          Delay after each character. Default: 20");
    puts("  --line-delay-ms N     Delay after each line. Default: 100");
    puts("  --start-delay-sec N   Countdown before typing. Default: 5");
    puts("  --max-bytes N         Maximum input size. Default: 1048576");
    puts("  --ascii-only          Refuse to type non-ASCII characters");
    puts("  --enter-mode key      Send newlines as Return key. Default");
    puts("  --enter-mode unicode  Send newlines as Unicode carriage return");
    puts("  --no-focus-check      Do not abort when the foreground app changes");
    puts("  --request-accessibility");
    puts("                         Ask macOS to show the Accessibility permission prompt");
    puts("  --dry-run             Parse and validate input without typing");
    puts("  --diagnose            Show source, focused app, and permission status without typing");
    puts("  --self-test           Check permission and send a harmless Shift key event");
    puts("  --debug-input         Type visible keys and Unicode test markers into the focused target");
    puts("  --help                Show this help");
    puts("");
    puts("Controls while running:");
    puts("  Ctrl-C                Abort immediately");
    puts("  Esc                   Abort before the next character if macOS reports it as pressed");
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

        matched = get_option_value(&i, argc, argv, "--source", &value);
        if (matched == 1) {
            if (strcmp(value, "clipboard") == 0) {
                opt->source = SourceKind::Clipboard;
            } else if (strcmp(value, "file") == 0) {
                opt->source = SourceKind::File;
            } else {
                fprintf(stderr, "Invalid --source value: %s\n", value);
                return false;
            }
            continue;
        }
        if (matched == 0) {
            return false;
        }

        matched = get_option_value(&i, argc, argv, "--file", &value);
        if (matched == 1) {
            opt->source = SourceKind::File;
            opt->file_path = value;
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

static bool read_file_source(const Options &opt, TextData *out, std::string *error) {
    std::filesystem::path path = opt.file_path.empty() ? executable_dir() / "trans.txt" : opt.file_path;
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

    NSData *data = [NSData dataWithBytes:bytes.data() length:bytes.size()];
    if (!decode_data(data, out, error)) {
        return false;
    }
    out->source_label = "File: " + path.string();
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
    if (data.text.empty()) {
        fprintf(stderr, "Input text is empty.\n");
        return EXIT_CONTENT;
    }
    if (stats.control_count > 0) {
        size_t line = 0;
        size_t col = 0;
        line_col_for_index(data.text, stats.first_control, &line, &col);
        fprintf(stderr, "Input contains unsupported control characters. First one at line %zu, column %zu.\n", line, col);
        fprintf(stderr, "Allowed control characters are tab and newlines only.\n");
        return EXIT_CONTENT;
    }
    if (stats.surrogate_error_count > 0) {
        size_t line = 0;
        size_t col = 0;
        line_col_for_index(data.text, stats.first_surrogate_error, &line, &col);
        fprintf(stderr, "Input contains invalid UTF-16 surrogate data. First issue at line %zu, column %zu.\n", line, col);
        return EXIT_CONTENT;
    }
    if (opt.ascii_only && stats.non_ascii_count > 0) {
        size_t line = 0;
        size_t col = 0;
        line_col_for_index(data.text, stats.first_non_ascii, &line, &col);
        fprintf(stderr, "--ascii-only is enabled, but input has non-ASCII characters. First one at line %zu, column %zu.\n", line, col);
        return EXIT_CONTENT;
    }
    if (opt.input_mode == InputMode::Keys && stats.non_ascii_count > 0) {
        size_t line = 0;
        size_t col = 0;
        line_col_for_index(data.text, stats.first_non_ascii, &line, &col);
        fprintf(stderr, "--input-mode keys can only type ASCII. First non-ASCII character is at line %zu, column %zu.\n", line, col);
        fprintf(stderr, "Use --input-mode unicode for local macOS apps that accept Unicode CGEvent payloads.\n");
        return EXIT_CONTENT;
    }
    return EXIT_OK;
}

static InputMode effective_input_mode(const Options &opt, const TextStats &stats) {
    if (opt.input_mode != InputMode::Auto) {
        return opt.input_mode;
    }
    return stats.non_ascii_count == 0 ? InputMode::Keys : InputMode::Unicode;
}

static const char *input_mode_name(InputMode mode) {
    switch (mode) {
        case InputMode::Auto:
            return "Auto";
        case InputMode::Keys:
            return "ASCII virtual keys";
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
        }
    }
    return app;
}

static void print_front_app(const FrontApp &app) {
    if (app.pid == 0) {
        puts("Target app: <unknown>");
    } else {
        printf("Target app: \"%s\" (pid %d)\n", app.name.c_str(), app.pid);
    }
}

static bool accessibility_trusted(bool prompt) {
    NSDictionary *options = @{(__bridge NSString *)kAXTrustedCheckOptionPrompt: @(prompt)};
    return AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
}

static void print_summary(const TextData &data, const TextStats &stats, const Options &opt) {
    InputMode mode = effective_input_mode(opt, stats);
    printf("Source: %s\n", data.source_label.c_str());
    printf("Encoding: %s\n", data.encoding.c_str());
    printf("Bytes: %zu\n", data.byte_count);
    printf("Characters: %zu\n", stats.scalar_count);
    printf("UTF-16 units: %zu\n", data.text.size());
    printf("Lines: %zu\n", stats.lines);
    printf("Non-ASCII characters: %zu\n", stats.non_ascii_count);
    printf("Delay: %d ms per character, %d ms per line\n", opt.delay_ms, opt.line_delay_ms);
    if (opt.input_mode == InputMode::Auto) {
        printf("Input mode: Auto -> %s\n", input_mode_name(mode));
    } else {
        printf("Input mode: %s\n", input_mode_name(mode));
    }
    if (mode == InputMode::Unicode) {
        printf("RDP note: some RDP clients ignore Unicode CGEvent payloads and may type repeated 'a'. Use --input-mode keys for ASCII text.\n");
    }
    printf("Enter mode: %s\n", opt.enter_mode == EnterMode::Unicode ? "Unicode CR" : "Return key");
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
    bool shift = false;
};

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
        stroke->shift = false;
        return true;
    }
    if (ch >= 'A' && ch <= 'Z') {
        if (!ascii_keystroke(static_cast<char16_t>(ch - 'A' + 'a'), stroke)) {
            return false;
        }
        stroke->shift = true;
        return true;
    }
    if (ch >= '0' && ch <= '9') {
        static const CGKeyCode keys[] = {
            kVK_ANSI_0, kVK_ANSI_1, kVK_ANSI_2, kVK_ANSI_3, kVK_ANSI_4,
            kVK_ANSI_5, kVK_ANSI_6, kVK_ANSI_7, kVK_ANSI_8, kVK_ANSI_9,
        };
        stroke->keycode = keys[ch - '0'];
        stroke->shift = false;
        return true;
    }

    switch (ch) {
        case ' ': stroke->keycode = kVK_Space; stroke->shift = false; return true;
        case '`': stroke->keycode = kVK_ANSI_Grave; stroke->shift = false; return true;
        case '~': stroke->keycode = kVK_ANSI_Grave; stroke->shift = true; return true;
        case '!': stroke->keycode = kVK_ANSI_1; stroke->shift = true; return true;
        case '@': stroke->keycode = kVK_ANSI_2; stroke->shift = true; return true;
        case '#': stroke->keycode = kVK_ANSI_3; stroke->shift = true; return true;
        case '$': stroke->keycode = kVK_ANSI_4; stroke->shift = true; return true;
        case '%': stroke->keycode = kVK_ANSI_5; stroke->shift = true; return true;
        case '^': stroke->keycode = kVK_ANSI_6; stroke->shift = true; return true;
        case '&': stroke->keycode = kVK_ANSI_7; stroke->shift = true; return true;
        case '*': stroke->keycode = kVK_ANSI_8; stroke->shift = true; return true;
        case '(': stroke->keycode = kVK_ANSI_9; stroke->shift = true; return true;
        case ')': stroke->keycode = kVK_ANSI_0; stroke->shift = true; return true;
        case '-': stroke->keycode = kVK_ANSI_Minus; stroke->shift = false; return true;
        case '_': stroke->keycode = kVK_ANSI_Minus; stroke->shift = true; return true;
        case '=': stroke->keycode = kVK_ANSI_Equal; stroke->shift = false; return true;
        case '+': stroke->keycode = kVK_ANSI_Equal; stroke->shift = true; return true;
        case '[': stroke->keycode = kVK_ANSI_LeftBracket; stroke->shift = false; return true;
        case '{': stroke->keycode = kVK_ANSI_LeftBracket; stroke->shift = true; return true;
        case ']': stroke->keycode = kVK_ANSI_RightBracket; stroke->shift = false; return true;
        case '}': stroke->keycode = kVK_ANSI_RightBracket; stroke->shift = true; return true;
        case '\\': stroke->keycode = kVK_ANSI_Backslash; stroke->shift = false; return true;
        case '|': stroke->keycode = kVK_ANSI_Backslash; stroke->shift = true; return true;
        case ';': stroke->keycode = kVK_ANSI_Semicolon; stroke->shift = false; return true;
        case ':': stroke->keycode = kVK_ANSI_Semicolon; stroke->shift = true; return true;
        case '\'': stroke->keycode = kVK_ANSI_Quote; stroke->shift = false; return true;
        case '"': stroke->keycode = kVK_ANSI_Quote; stroke->shift = true; return true;
        case ',': stroke->keycode = kVK_ANSI_Comma; stroke->shift = false; return true;
        case '<': stroke->keycode = kVK_ANSI_Comma; stroke->shift = true; return true;
        case '.': stroke->keycode = kVK_ANSI_Period; stroke->shift = false; return true;
        case '>': stroke->keycode = kVK_ANSI_Period; stroke->shift = true; return true;
        case '/': stroke->keycode = kVK_ANSI_Slash; stroke->shift = false; return true;
        case '?': stroke->keycode = kVK_ANSI_Slash; stroke->shift = true; return true;
        default:
            return false;
    }
}

static bool escape_pressed() {
    return CGEventSourceKeyState(kCGEventSourceStateCombinedSessionState, kVK_Escape);
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
        return send_key_with_shift(keycode, false);
    }

    bool send_key_with_shift(CGKeyCode keycode, bool shift) {
        if (!source_) {
            return false;
        }
        CGEventRef shift_down = nullptr;
        CGEventRef shift_up = nullptr;
        if (shift) {
            shift_down = CGEventCreateKeyboardEvent(source_, kVK_Shift, true);
            shift_up = CGEventCreateKeyboardEvent(source_, kVK_Shift, false);
            if (!shift_down || !shift_up) {
                if (shift_down) {
                    CFRelease(shift_down);
                }
                if (shift_up) {
                    CFRelease(shift_up);
                }
                return false;
            }
        }
        CGEventRef down = CGEventCreateKeyboardEvent(source_, keycode, true);
        CGEventRef up = CGEventCreateKeyboardEvent(source_, keycode, false);
        if (!down || !up) {
            if (shift_down) {
                CFRelease(shift_down);
            }
            if (shift_up) {
                CFRelease(shift_up);
            }
            if (down) {
                CFRelease(down);
            }
            if (up) {
                CFRelease(up);
            }
            return false;
        }
        if (shift_down) {
            CGEventPost(kCGHIDEventTap, shift_down);
        }
        CGEventPost(kCGHIDEventTap, down);
        CGEventPost(kCGHIDEventTap, up);
        if (shift_up) {
            CGEventPost(kCGHIDEventTap, shift_up);
        }
        if (shift_down) {
            CFRelease(shift_down);
        }
        if (shift_up) {
            CFRelease(shift_up);
        }
        CFRelease(down);
        CFRelease(up);
        return true;
    }

    bool send_ascii_char(char16_t ch) {
        KeyStroke stroke;
        if (!ascii_keystroke(ch, &stroke)) {
            return false;
        }
        return send_key_with_shift(stroke.keycode, stroke.shift);
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
    puts("Ctrl-C aborts. Esc aborts if macOS reports it as pressed.");
    int rc = run_countdown(opt.start_delay_sec);
    if (rc != EXIT_OK) {
        return rc;
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
    InputMode mode = effective_input_mode(opt, stats);

    puts("");
    puts("Typing started. Ctrl-C aborts. Esc aborts before the next character.");
    printf("Progress: line %zu/%zu\n", line, stats.lines);

    for (size_t i = 0; i < data.text.size();) {
        if (escape_pressed()) {
            printf("\nAborted at line %zu, column %zu after %zu character/event unit(s).\n", line, col, typed);
            return EXIT_ABORTED;
        }
        if (!opt.no_focus_check) {
            FrontApp current = frontmost_app();
            if (target.pid != 0 && current.pid != target.pid) {
                fprintf(stderr, "\nForeground app changed from \"%s\" to \"%s\". Aborting to avoid typing into the wrong target.\n",
                        target.name.c_str(), current.name.c_str());
                return EXIT_ABORTED;
            }
        }

        uint32_t scalar = 0;
        size_t units = 1;
        bool invalid = false;
        next_scalar(data.text, i, &scalar, &units, &invalid);
        bool ok = false;

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
            sleep_ms(opt.line_delay_ms);
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
            sleep_ms(opt.line_delay_ms);
        } else if (scalar == '\t') {
            ok = keyboard.send_key(kVK_Tab);
            ++col;
            sleep_ms(opt.delay_ms);
        } else {
            if (mode == InputMode::Keys) {
                ok = keyboard.send_ascii_char(data.text[i]);
            } else {
                ok = keyboard.send_unicode_units(&data.text[i], units);
            }
            ++col;
            sleep_ms(opt.delay_ms);
        }

        if (!ok) {
            fprintf(stderr, "\nInput failed at line %zu, column %zu.\n", line, col);
            return EXIT_INPUT;
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
    if (!keyboard.send_key(kVK_Shift)) {
        fprintf(stderr, "Self-test failed: cannot post Shift key event.\n");
        return EXIT_INPUT;
    }
    puts("Self-test completed: posted a harmless Shift key event.");
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
    NSString *keys_marker = @"TTDBG_MAC_KEYS abcXYZ 0123 \\ / : ; ' \" []{} () !@#$%^&*\n";
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
    NSString *unicode_marker = @"TTDBG_MAC_UNICODE Unicode=中 emoji=✓\n";
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

static int run_diagnose(const Options &opt) {
    printf("Accessibility trusted: %s\n", accessibility_trusted(opt.request_accessibility) ? "yes" : "no");
    print_front_app(frontmost_app());

    TextData data;
    std::string error;
    if (read_text_source(opt, &data, &error)) {
        TextStats stats = analyze_text(data.text);
        print_summary(data, stats, opt);
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

        TextStats stats = analyze_text(data.text);
        print_summary(data, stats, opt);

        int rc = validate_text(data, stats, opt);
        if (rc != EXIT_OK) {
            return rc;
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
