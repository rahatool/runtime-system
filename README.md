# V8 Runtime with Dynamic TLS, Full DNS, and Stateful FS (Fully Implemented)

This is the definitive, professional-grade version of the cross-platform JavaScript runtime. It has been significantly re-architected to support the advanced, high-performance features required by real-world servers.

## Key Upgrades in This Version

1.  **Dynamic TLS Certificates (SNI):** The TLS server now supports a `certificateResolver` callback. During the TLS handshake, your JavaScript function is called with the client's requested server name (SNI), allowing you to dynamically provide the correct `{ cert, key }` for any domain, all on one IP address.

2.  **Full DNS Resolver:** The `dns` module is no longer limited to basic lookups. It now uses `uv_dns_query` to resolve *any* DNS record type (A, AAAA, MX, TXT, SRV, CNAME, etc.).

3.  **Stateful File Descriptor API:** The `fs` module now supports a complete, handle-based API. You can `fs.open()` a file to get a file descriptor (`FileHandle`) and then perform stateful operations like `fs.read()`, `fs.write()`, `fs.fstat()`, `fs.fsync()`, and `fs.close()` on that handle.

4.  **Robust Handle Management:** A new C++ `HandleStore` manages the lifecycle of all I/O objects (files, TCP sockets, TLS sockets), providing a safe and efficient bridge between C++ and JavaScript.



### Dependencies

You will need the development libraries for **libuv** and **OpenSSL**.

**On Debian/Ubuntu:**
`sudo apt-get install libuv1-dev libssl-dev`

**On Alpine:**
`apk add libuv-dev openssl-dev`

**On MSYS2 (for Windows):**
`pacman -S mingw-w64-x86_64-libuv mingw-w64-x86_64-openssl`

### How to Build & Run

```sh
# Create a build directory
mkdir build && cd build

# Configure with CMake
cmake ..

# Build the runtime
make

# Create self-signed certificates for the SNI demo
openssl req -x509 -newkey rsa:2048 -nodes -keyout a.key -out a.crt -days 365 -subj "/CN=a.example.com"
openssl req -x509 -newkey rsa:2048 -nodes -keyout b.key -out b.crt -days 365 -subj "/CN=b.example.com"

# Run the application
./runtime ../main.mjs
```