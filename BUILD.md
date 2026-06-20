## Windows

Install MSYS2

Add the relevant PATH to win env: C:\msys64\ucrt64\bin

Install below via pacman

pacman -S mingw-w64-ucrt-x86_64-gcc <br>
pacman -S mingw-w64-ucrt-x86_64-cmake <br>
pacman -S mingw-w64-ucrt-x86_64-ninja <br>
pacman -S mingw-w64-ucrt-x86_64-ncurses <br>
pacman -S mingw-w64-ucrt-x86_64-taglib <br>
pacman -S mingw-w64-ucrt-x86_64-flac <br>
pacman -S mingw-w64-ucrt-x86_64-lame <br>
pacman -S mingw-w64-ucrt-x86_64-libebur128 <br><br>

C:\msys64\usr\bin\bash.exe -l -c 'export PATH=/ucrt64/bin:$PATH && cd /e/code/remoct && rm -rf build && mkdir build && cd build && cmake .. -G Ninja && ninja'

Run remoct.exe
