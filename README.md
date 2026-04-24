# Langton's Ants Win32 &nbsp;<img src="./assets/icon48.png" height="38">

<p align="center">
<img src="./assets/screenshot.png?raw=true" width="50%">
</p>

## About

A raw Win32 C++ implementation of [Langton's Ants](https://en.wikipedia.org/wiki/Langton%27s_ant).

It is multi-threaded, and supports Windows 2000 - 11.

<img src="./assets/Windows_2000_logo.svg" height="64">

## Building

### Via MinGW
Use the Makefile or `build_mingw.sh`. It should work on Linux, and with MinGW on Windows.  
You can also use my fork of [w64devkit](https://github.com/skeeto/w64devkit), called [win32-devkit](https://github.com/Alex313031/win32-devkit).

Using this method, you can compile on Linux, or Windows XP+ (using win32-devkit).

```
make -B all -j# (where # is number of jobs)

```

### With GN/Ninja
[Chromium](https://www.chromium.org) uses a build system with [GN](https://gn.googlesource.com/gn/+/refs/heads/main/README.md) and [Ninja](https://ninja-build.org/).

I have made a minimal, modified version configured specifically for compiling Win32 programs
for legacy Windows called [gn-legacy](https://github.com/Alex313031/gn-legacy).  
It can be used on Windows 7+ or Linux. (Unlike the regular MinGW method above, gn.exe does not work on Windows XP/Vista.)

Really, it is a meta-build system. GN stands for "Generate Ninja" and can use __BUILD.gn__ files to
generate `.ninja` files. These are used by Ninja (the actual build system), to run the commands to compile it.  
The compiler itself is dependant on the host platform:  
On Linux, a special MinGW build I compiled on Ubuntu 24.04 to support legacy Windows and use static linkage is used.
On Windows, it simply uses an extracted toolchain from win32-devkit mentioned above.

### Resources/Credits

Science Article describing cellular automata in programming: https://www.sciencedirect.com/science/article/abs/pii/016727898690237X

Charles Petzold - [Programming Windows 5th Ed.](https://www.charlespetzold.com/books/)
