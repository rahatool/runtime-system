# V8 Runtime with Dynamic TLS, Full DNS, and Stateful FS (Fully Implemented)

This is the definitive, professional-grade version of the cross-platform JavaScript runtime. It has been significantly re-architected to support the advanced, high-performance features required by real-world servers.

## Key Upgrades in This Version

1.  **Dynamic TLS Certificates (SNI):** The TLS server now supports a `certificateResolver` callback. During the TLS handshake, your JavaScript function is called with the client's requested server name (SNI), allowing you to dynamically provide the correct pre-parsed `{ key, cert }` handles for any domain.

2.  **Full DNS Resolver:** The `dns` module is no longer limited to basic lookups. It now uses `uv_dns_query` to resolve *any* DNS record type (A, AAAA, MX, TXT, SRV, CNAME, etc.).

3.  **Stateful File Descriptor API:** The `fs` module now supports a complete, handle-based API. You can `fs.open()` a file to get a file descriptor (`FileHandle`) and then perform stateful operations like `fs.read()`, `fs.write()`, `fs.fstat()`, `fs.fsync()`, and `fs.close()` on that handle.

4.  **Robust Handle Management:** A central C++ `HandleStore` manages the lifecycle of all I/O objects (files, TCP sockets, TLS sockets, TLS contexts, keys, certs). A JavaScript `FinalizationRegistry` automatically cleans up C++ resources when JS objects are garbage collected.

---

## Dependencies

You need development libraries for **V8**, **libuv**, and **OpenSSL**, plus standard build tools.

### MSYS2 MINGW64 (Windows)

- Open MSYS2 MINGW64 shell.
- Install toolchain and dependencies:
  ```sh
  pacman -S \
    mingw-w64-x86_64-toolchain \
    mingw-w64-x86_64-cmake \
    mingw-w64-x86_64-ninja \
    mingw-w64-x86_64-pkg-config \
    mingw-w64-x86_64-v8 \
    mingw-w64-x86_64-libuv \
    mingw-w64-x86_64-openssl
  ```
- V8 headers and libs are under `/mingw64`. The build uses `V8_DIR` to locate them.

### Debian/Ubuntu

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build pkg-config \
  libuv1-dev libssl-dev
# V8: install from your distro or custom build; set V8_DIR accordingly
```

### Alpine

```sh
apk add --no-cache \
  build-base cmake ninja pkgconfig \
  libuv-dev openssl-dev
# V8: install from community repo or custom build; set V8_DIR accordingly
```

---

## Environment

- The CMake project uses `V8_DIR` to locate V8 headers and libraries.
- By default, `CMakeLists.txt` sets:
  ```
  set(V8_DIR "/mingw64")
  ```
- You can override in your shell:
  ```sh
  export V8_DIR=/mingw64
  ```

---

## Build & Run

From the project root:

```sh
rm -rf build && mkdir build && cd build
cmake ..
ninja   # or: cmake --build .   or: mingw32-make
```

### TLS Demo Certificates (optional)

```sh
MSYS_NO_PATHCONV=1 openssl req -x509 -newkey rsa:2048 -nodes -keyout a.key -out a.crt -days 365 -subj "/CN=a.example.com"
MSYS_NO_PATHCONV=1 openssl req -x509 -newkey rsa:2048 -nodes -keyout b.key -out b.crt -days 365 -subj "/CN=b.example.com"
```

### Run

From the `build` directory:
```sh
./runtime ../main.js
```

## Notes

- Build system: C++20, CMake, Ninja/MinGW.
- Links against: `v8`, `v8_libbase`, `v8_libplatform`, `libuv`, `OpenSSL::SSL`, `OpenSSL::Crypto` (plus Windows system libs).