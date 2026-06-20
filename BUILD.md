## Windows

Install MSYS2

Add the relevant PATH to win env: C:\msys64\ucrt64\bin

Install below via pacman

pacman -S mingw-w64-ucrt-x86_64-gcc
pacman -S mingw-w64-ucrt-x86_64-cmake
pacman -S mingw-w64-ucrt-x86_64-ninja
pacman -S mingw-w64-ucrt-x86_64-ncurses
pacman -S mingw-w64-ucrt-x86_64-taglib
pacman -S mingw-w64-ucrt-x86_64-flac
pacman -S mingw-w64-ucrt-x86_64-lame
pacman -S mingw-w64-ucrt-x86_64-libebur128

C:\msys64\usr\bin\bash.exe -l -c 'export PATH=/ucrt64/bin:$PATH && cd /e/code/remoct && rm -rf build && mkdir build && cd build && cmake .. -G Ninja && ninja'

Run remoct.exe
