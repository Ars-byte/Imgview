# imgview — Terminal Image Viewer
 
A lightweight terminal image viewer written in C++ that renders images directly in your terminal using Unicode half-block characters (`▄`) and 24-bit ANSI true color. No dependencies beyond a C++ compiler — image decoding is handled by the single-header [`stb_image`](https://github.com/nothings/stb) library.


<img width="1571" height="884" alt="image" src="https://github.com/user-attachments/assets/86a2d9ba-231e-425e-a939-8013ee3de646" />
 
## How it works
 
Each terminal cell displays **two pixels** — the top half uses the background color and the bottom half uses the foreground color via the `▄` character. This doubles the effective vertical resolution. Images are scaled with bilinear interpolation to fit your terminal window while preserving the aspect ratio.
 
## Requirements
 
- A C++17 compiler (`g++` or `clang++`)
- A terminal with **24-bit true color** support (most modern terminals: iTerm2, Kitty, Alacritty, Windows Terminal, GNOME Terminal, etc.)
- `stb_image.h` in the same directory as the source
## Build
 
```bash
g++ -O2 -std=c++17 -o imgview imgview.cpp -lm
```
 
Or with clang:
 
```bash
clang++ -O2 -std=c++17 -o imgview imgview.cpp -lm
```
 
## Usage
 
```
imgview [options] <image> [image2 ...]
```
 
### Options
 
| Flag | Description |
|---|---|
| `-w <cols>` | Force output width in terminal columns (default: auto) |
| `-h <rows>` | Force output height in terminal rows (default: auto) |
| `-g` | Grayscale mode |
| `-d` | Floyd–Steinberg dithering (useful for low-color terminals) |
| `-i` | Print image metadata only, don't render |
| `-s <sec>` | Slideshow mode — advance every `<sec>` seconds automatically |
| `--help` | Show help message |
 
### Examples
 
```bash
# View a single image
./imgview photo.jpg
 
# View at a specific size
./imgview -w 80 -h 40 photo.png
 
# Grayscale with dithering
./imgview -g -d photo.jpg
 
# Print image info without rendering
./imgview -i *.jpg
 
# Slideshow — cycle through images every 3 seconds
./imgview -s 3 img1.png img2.jpg img3.bmp
 
# Manual slideshow (navigate with keys)
./imgview img1.png img2.jpg img3.bmp
```




### Navigation controls
 
| Key | Action |
|---|---|
| `Enter` / `Space` / `→` / `n` | Next image |
| `←` / `p` | Previous image |
| `q` / `Esc` | Quit |
 
## Supported formats
 
| Format | Notes |
|---|---|
| JPEG | Full support |
| PNG | Full support, including transparency |
| BMP | Full support |
| GIF | First frame only |
| TGA | Full support |
| PNM / PGM / PPM | Full support |
| HDR | Radiance RGBE format |
| PIC | Softimage PIC |
 
Transparency (alpha channel) is rendered over a checkerboard background pattern.
 
## Project structure
 
```
imgview/
├── imgview.cpp     # Main source — everything in one file
├── stb_image.h     # Single-header image decoding library (nothings/stb)
└── README.md
```
 
## License
 
`imgview.cpp` is released under the MIT License.  
`stb_image.h` is public domain / MIT — see the header file for details.
