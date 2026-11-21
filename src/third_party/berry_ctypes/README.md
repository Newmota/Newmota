# Berry ctypes support

Imported from Tasmota's `lib/libesp32/berry_tasmota/include/be_ctypes.h` and
`src/be_ctypes.c`, authored by Stephan Hadinger. The source library declares the
MIT license in its `library.json`. These files retain their upstream notices.

This support is selected with `USE_LVGL`. The matched LVGL ctypes tables assume
32-bit pointers and `size_t`.
