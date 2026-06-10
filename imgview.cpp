#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
#  include <io.h>
#  define isatty _isatty
#  define fileno _fileno
#else
#  include <sys/ioctl.h>
#  include <unistd.h>
#  include <termios.h>
#endif

static void set_fg(unsigned r, unsigned g, unsigned b) {
    printf("\033[38;2;%u;%u;%um", r, g, b);
}
static void set_bg(unsigned r, unsigned g, unsigned b) {
    printf("\033[48;2;%u;%u;%um", r, g, b);
}
static void reset_colors() {
    printf("\033[0m");
}
static void clear_screen() {
    printf("\033[2J\033[H");
}
static void hide_cursor() {
    printf("\033[?25l");
}
static void show_cursor() {
    printf("\033[?25h");
}
static void move_to(int row, int col) {
    printf("\033[%d;%dH", row, col);
}

struct TermSize { int cols, rows; };

static TermSize get_term_size() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi))
        return { csbi.srWindow.Right - csbi.srWindow.Left + 1,
                 csbi.srWindow.Bottom - csbi.srWindow.Top + 1 };
#else
    struct winsize ws{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
        return { ws.ws_col, ws.ws_row };
#endif
    return { 80, 24 };
}


struct Pixel { unsigned char r, g, b, a; };

static Pixel get_pixel(const unsigned char* data, int x, int y,
                        int width, int channels) {
    const unsigned char* p = data + (y * width + x) * channels;
    Pixel px{};
    px.r = p[0];
    px.g = (channels >= 2) ? p[1] : p[0];
    px.b = (channels >= 3) ? p[2] : p[0];
    px.a = (channels == 4) ? p[3] : 255;
    if (channels == 2) { px.g = p[0]; px.b = p[0]; px.a = p[1]; }
    return px;
}

static Pixel sample_bilinear(const unsigned char* data, float fx, float fy,
                              int width, int height, int channels) {
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = std::min(x0 + 1, width  - 1);
    int y1 = std::min(y0 + 1, height - 1);
    x0 = std::max(0, x0); y0 = std::max(0, y0);
    float tx = fx - (int)fx, ty = fy - (int)fy;

    auto lerp = [](float a, float b, float t){ return a + (b - a) * t; };

    Pixel p00 = get_pixel(data, x0, y0, width, channels);
    Pixel p10 = get_pixel(data, x1, y0, width, channels);
    Pixel p01 = get_pixel(data, x0, y1, width, channels);
    Pixel p11 = get_pixel(data, x1, y1, width, channels);

    return {
        (unsigned char)lerp(lerp(p00.r, p10.r, tx), lerp(p01.r, p11.r, tx), ty),
        (unsigned char)lerp(lerp(p00.g, p10.g, tx), lerp(p01.g, p11.g, tx), ty),
        (unsigned char)lerp(lerp(p00.b, p10.b, tx), lerp(p01.b, p11.b, tx), ty),
        (unsigned char)lerp(lerp(p00.a, p10.a, tx), lerp(p01.a, p11.a, tx), ty),
    };
}


static Pixel to_gray(Pixel p) {
    unsigned char g = (unsigned char)(0.2126*p.r + 0.7152*p.g + 0.0722*p.b);
    return { g, g, g, p.a };
}


static Pixel blend_checker(Pixel p, int cx, int cy) {
    if (p.a == 255) return p;
    unsigned char bg = ((cx ^ cy) & 1) ? 200 : 100;
    float alpha = p.a / 255.0f;
    return {
        (unsigned char)(p.r * alpha + bg * (1 - alpha)),
        (unsigned char)(p.g * alpha + bg * (1 - alpha)),
        (unsigned char)(p.b * alpha + bg * (1 - alpha)),
        255
    };
}


struct RenderOptions {
    int  out_cols   = 0;   // 0 = auto
    int  out_rows   = 0;   // 0 = auto
    bool grayscale  = false;
    bool dither     = false;
};

static void render_image(const unsigned char* data,
                         int img_w, int img_h, int channels,
                         const RenderOptions& opt)
{
    TermSize ts = get_term_size();

    int max_cols = ts.cols;
    int max_rows = ts.rows - 1;

    int out_cols = opt.out_cols > 0 ? opt.out_cols : max_cols;
    int out_rows = opt.out_rows > 0 ? opt.out_rows : 0;

    if (out_rows == 0) {
        double aspect = (double)img_w / (double)img_h;
        out_rows = (int)((out_cols / aspect) / 2.0);
        if (out_rows > max_rows) {
            out_rows = max_rows;
            out_cols = (int)(out_rows * aspect * 2.0);
            if (out_cols > max_cols) out_cols = max_cols;
        }
    }

    out_cols = std::max(1, out_cols);
    out_rows = std::max(1, out_rows);

    float sx = (float)(img_w - 1) / (float)(out_cols - 1 + 1e-9f);
    float sy = (float)(img_h - 1) / (float)(out_rows * 2 - 1 + 1e-9f);

    struct Err { float r, g, b; };
    std::vector<Err> err_cur(out_cols + 2, {0,0,0});
    std::vector<Err> err_nxt(out_cols + 2, {0,0,0});

    auto quant6 = [](float v) -> unsigned char {
        int q = (int)std::round(v / 255.0f * 5.0f);
        q = std::max(0, std::min(5, q));
        return (unsigned char)(q * 51);
    };

    for (int row = 0; row < out_rows; ++row) {
        if (opt.dither) {
            std::fill(err_nxt.begin(), err_nxt.end(), Err{0,0,0});
        }

        for (int col = 0; col < out_cols; ++col) {
            float fy_top = row * 2 * sy;
            float fy_bot = (row * 2 + 1) * sy;
            float fx     = col * sx;

            Pixel top = sample_bilinear(data, fx, fy_top, img_w, img_h, channels);
            Pixel bot = sample_bilinear(data, fx, fy_bot, img_w, img_h, channels);

            top = blend_checker(top, col,   row * 2);
            bot = blend_checker(bot, col,   row * 2 + 1);

            if (opt.grayscale) { top = to_gray(top); bot = to_gray(bot); }

            if (opt.dither) {

                float tr = std::min(255.0f, std::max(0.0f, top.r + err_cur[col+1].r));
                float tg = std::min(255.0f, std::max(0.0f, top.g + err_cur[col+1].g));
                float tb = std::min(255.0f, std::max(0.0f, top.b + err_cur[col+1].b));
                unsigned char qr = quant6(tr), qg = quant6(tg), qb = quant6(tb);
                float er = tr - qr, eg = tg - qg, eb = tb - qb;

                if (col + 2 < (int)err_cur.size()) {
                    err_cur[col+2].r += er * 7/16.0f;
                    err_nxt[col  ].r += er * 3/16.0f;
                    err_nxt[col+1].r += er * 5/16.0f;
                    err_nxt[col+2].r += er * 1/16.0f;
                    err_cur[col+2].g += eg * 7/16.0f;
                    err_nxt[col  ].g += eg * 3/16.0f;
                    err_nxt[col+1].g += eg * 5/16.0f;
                    err_nxt[col+2].g += eg * 1/16.0f;
                    err_cur[col+2].b += eb * 7/16.0f;
                    err_nxt[col  ].b += eb * 3/16.0f;
                    err_nxt[col+1].b += eb * 5/16.0f;
                    err_nxt[col+2].b += eb * 1/16.0f;
                }
                top.r = qr; top.g = qg; top.b = qb;

                float br2 = std::min(255.0f, std::max(0.0f, bot.r + err_nxt[col+1].r));
                float bg2 = std::min(255.0f, std::max(0.0f, bot.g + err_nxt[col+1].g));
                float bb2 = std::min(255.0f, std::max(0.0f, bot.b + err_nxt[col+1].b));
                bot.r = quant6(br2); bot.g = quant6(bg2); bot.b = quant6(bb2);
            }

            set_bg(top.r, top.g, top.b);
            set_fg(bot.r, bot.g, bot.b);
            printf("\xe2\x96\x84");
        }
        reset_colors();
        printf("\n");

        if (opt.dither) std::swap(err_cur, err_nxt);
    }
}


static const char* channel_name(int c) {
    switch (c) {
        case 1: return "Grayscale";
        case 2: return "Grayscale+Alpha";
        case 3: return "RGB";
        case 4: return "RGBA";
        default: return "Unknown";
    }
}

static void print_info(const char* path, int w, int h, int ch) {
    printf("  File     : %s\n", path);
    printf("  Size     : %d × %d px\n", w, h);
    printf("  Channels : %d (%s)\n", ch, channel_name(ch));
    printf("  Memory   : %.1f KB\n", (double)(w * h * ch) / 1024.0);
}


static void print_status(const char* path, int w, int h, int ch,
                         int index, int total,
                         const RenderOptions& opt) {
    TermSize ts = get_term_size();
    move_to(ts.rows, 1);
    printf("\033[48;2;30;30;30m\033[38;2;200;200;200m");

    char buf[512];
    snprintf(buf, sizeof(buf),
             " [%d/%d] %s  |  %dx%d  %s%s%s ",
             index, total, path,
             w, h, channel_name(ch),
             opt.grayscale ? "  [gray]" : "",
             opt.dither    ? "  [dither]" : "");

    int len = (int)strlen(buf);
    printf("%.*s", ts.cols, buf);
    for (int i = len; i < ts.cols; ++i) putchar(' ');
    reset_colors();
    fflush(stdout);
}


#ifndef _WIN32
static char read_key() {
    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);
    char c = (char)getchar();
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return c;
}
#else
static char read_key() { return (char)_getch(); }
#endif


static void print_help(const char* prog) {
    printf(
        "Usage: %s [options] <image> [image2 ...]\n\n"
        "Options:\n"
        "  -w <cols>      Output width  in terminal columns (default: auto)\n"
        "  -h <rows>      Output height in terminal rows   (default: auto)\n"
        "  -g             Grayscale mode\n"
        "  -d             Floyd-Steinberg dithering\n"
        "  -i             Print info only, don't render\n"
        "  -s <sec>       Slideshow: seconds between images (0 = manual)\n"
        "  --help         Show this help\n\n"
        "Supported formats: JPEG, PNG, BMP, GIF, TGA, PNM, HDR, PIC\n\n"
        "Controls (during slideshow):\n"
        "  Enter / Space / → / n   Next image\n"
        "  ← / p                   Previous image\n"
        "  q / Esc                 Quit\n",
        prog);
}


int main(int argc, char* argv[]) {
    RenderOptions opt;
    std::vector<std::string> files;
    bool info_only    = false;
    double slide_secs = 0.0;  // 0 = wait for key

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-?") { print_help(argv[0]); return 0; }
        else if (a == "-g") { opt.grayscale = true; }
        else if (a == "-d") { opt.dither    = true; }
        else if (a == "-i") { info_only     = true; }
        else if (a == "-w" && i + 1 < argc) { opt.out_cols = atoi(argv[++i]); }
        else if (a == "-h" && i + 1 < argc) { opt.out_rows = atoi(argv[++i]); }
        else if (a == "-s" && i + 1 < argc) { slide_secs   = atof(argv[++i]); }
        else if (a[0] != '-') { files.push_back(a); }
        else {
            fprintf(stderr, "Unknown option: %s  (use --help)\n", argv[i]);
            return 1;
        }
    }

    if (files.empty()) {
        fprintf(stderr, "No image file specified. Try: %s --help\n", argv[0]);
        return 1;
    }

    if (info_only) {
        for (auto& f : files) {
            int w, h, ch;
            if (!stbi_info(f.c_str(), &w, &h, &ch)) {
                fprintf(stderr, "Cannot read: %s (%s)\n",
                        f.c_str(), stbi_failure_reason());
                continue;
            }
            print_info(f.c_str(), w, h, ch);
            printf("\n");
        }
        return 0;
    }

    hide_cursor();
    clear_screen();

    int total   = (int)files.size();
    int current = 0;
    bool quit   = false;

    while (!quit) {
        const std::string& path = files[current];

        int img_w, img_h, img_ch;
        unsigned char* data = stbi_load(path.c_str(), &img_w, &img_h, &img_ch, 0);

        move_to(1, 1);

        if (!data) {
            reset_colors();
            printf("\033[31mError loading '%s': %s\033[0m\n",
                   path.c_str(), stbi_failure_reason());
        } else {
            render_image(data, img_w, img_h, img_ch, opt);
            stbi_image_free(data);
        }

        fflush(stdout);

        if (total == 1 && slide_secs <= 0) {
            read_key();
            quit = true;
        } else if (slide_secs > 0) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds((int)(slide_secs * 1000)));
            current = (current + 1) % total;
            if (current == 0) quit = true;
        } else {
            while (true) {
                char k = read_key();
                if (k == 'q' || k == 'Q' || k == 27 /* ESC */) {
                    quit = true; break;
                }
                if (k == '\033') {
                    char k2 = read_key();
                    if (k2 == '[') {
                        char k3 = read_key();
                        if (k3 == 'C' || k3 == 'B') {
                            current = (current + 1) % total; break;
                        } else if (k3 == 'D' || k3 == 'A') {
                            current = (current - 1 + total) % total; break;
                        }
                    }
                    continue;
                }
                if (k == '\n' || k == '\r' || k == ' ' || k == 'n') {
                    current = (current + 1) % total; break;
                }
                if (k == 'p') {
                    current = (current - 1 + total) % total; break;
                }
            }
        }
        clear_screen();
    }

    reset_colors();
    show_cursor();
    clear_screen();
    return 0;
}
