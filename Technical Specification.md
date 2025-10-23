# CoroJS Runtime Technical Specification

Version: 1.0

## 1. Overview

CoroJS is a high-performance, single-threaded, asynchronous JavaScript runtime. Its primary design goal is to enable the development of high-concurrency network applications using a simple, synchronous, blocking I/O style, powered by stackful coroutines (fibers) instead of `async/await` and Promises.

It is built on the V8 JavaScript engine and uses `libuv` for platform-agnostic asynchronous I/O and `OpenSSL` for TLS/SSL. All I/O operations are exposed to JavaScript via a robust set of native C++ bindings.

## 2. Core Architecture

The runtime is composed of several distinct layers, designed for clear separation of concerns, safety, and performance.

### 2.1. Runtime Kernel (C++)

The C++ executable (`runtime`) is the "kernel" of the system. Its responsibilities are minimal:

- V8 Engine: It initializes and embeds the V8 JavaScript engine.

- libuv Event Loop: It initializes and runs the `libuv` event loop (`uv_run`), which drives all I/O.

- Native Bindings: It injects a single global object, `__primordials`, into the JavaScript context. This object is the only bridge between JavaScript and the C++ layer.

- Module Loader: It loads the JavaScript standard library (`stdlib.js`) and the user-provided entry script (`main.mjs`) using V8's ES Module API.

### 2.2. Coroutine Subsystem (Fibers)

This is the core feature of CoroJS. It allows asynchronous operations to be written as simple blocking calls.

- Implementation: The fiber scheduler is implemented in C++ (`fiber.cpp`, `fiber.h`).

- Cross-Platform:

	- On Windows, it uses the native Windows Fibers API (`CreateFiber`, `SwitchToFiber`, `ConvertThreadToFiber`).

	- On POSIX (Linux, macOS), it uses the `ucontext.h` API (`getcontext`, `makecontext`, `swapcontext`).

- Stack: Each fiber is allocated a fixed-size stack (1MB). Stacks are not resizable.

- Integration:
	- A JS function (e.g., `net.read`) calls a C++ primitive (e.g., `NET_Read`).
	- The C++ primitive initiates the asynchronous `libuv` operation.
	- It calls `Fiber::yield()`, saving the current fiber's stack and context, and switching execution back to the main `libuv` event loop fiber.
	- When the `libuv` operation completes (e.g., data is available), its C callback is fired.
	- The C callback retrieves the suspended fiber associated with the operation and calls `Fiber::resume()`, passing back the result.
	- The fiber's stack and context are restored, and execution continues in JavaScript exactly where it left off, with `net.read()` now returning the data.

### 2.3. Handle Management (`HandleStore`)

To safely manage the lifecycle of C++ objects (like sockets, files, and TLS contexts) from JavaScript, a `HandleStore` is used.

- JS never holds a raw C++ pointer.
- When a native resource is created (e.g., `fs.open`), a `std::shared_ptr<BaseHandle>` is created in C++ and stored in a global `std::map`.
- A unique `uint64_t` ID is returned to JavaScript.
- All subsequent JS calls (`fs.read`, `fs.close`) pass this ID.
- The C++ layer uses the ID to look up the `shared_ptr`, ensuring the object is valid and its type is correct.
- When `fs.close()` is called, the handle is removed from the map, and the `shared_ptr`'s reference count drops, allowing for safe destruction.

### 2.4. Native Bindings (`__primordials`)

This C++ object is the internal, unstable API between the kernel and the standard library. See Section 3 for a full API reference.

### 2.5. JavaScript Standard Library (`stdlib.js`)

This JS file is loaded before the user's script. It wraps the "unsafe" `__primordials` in a clean, user-friendly, public-facing API (e.g., `net.createServer`, `fs.open`). All user-facing classes, such as `FileHandle` and Socket, are defined here.

## 3. Internal C++ API (`__primordials`)

This section documents the low-level contract between C++ and `stdlib.js`.

### `fiber`

- `fiber.run(jsFunction)`: Creates a new fiber and immediately executes `jsFunction` within it.

### `fs`

- `fs.open(path, flags, mode) -> BigInt(id)`: Opens a file. Returns a handle ID.

- `fs.read(id, length, offset) -> Uint8Array | null`: Reads from a file handle. Returns `null` on EOF.

- `fs.write(id, buffer, offset) -> BigInt(bytesWritten)`: Writes a Uint8Array to a file handle.

- `fs.fstat(id) -> Object`: Returns a stat object.

- `fs.fsync(id)`: Flushes file data to disk.

- `fs.fdatasync(id)`: Flushes file data (but not metadata) to disk.

- `fs.close(id)`: Closes a file handle.

- `fs.readdir(path) -> Array[String]`: Reads directory contents.

- `fs.unlink(path)`: Deletes a file.

- `fs.mkdir(path, mode)`: Creates a directory.

- `fs.rmdir(path)`: Removes a directory.

`net`

- `net.listen(host, port) -> BigInt(id)`: Creates a TCP server. Returns a server handle ID.

- `net.accept(id) -> BigInt(id)`: Blocks until a connection is accepted. Returns a client socket handle ID.

- `net.connect(host, port) -> BigInt(id)`: Connects to a TCP server. Returns a client socket handle ID.

- `net.read(id) -> String | null`: Reads data from a socket. Returns `null` on EOF.

- `net.write(id, data) -> BigInt(bytesWritten)`: Writes string data to a socket.

- `net.close(id)`: Closes a TCP socket or server.

- `net.poll(id, events) -> Int(events)`: Polls a handle for readability (1) or writability (2). Used by TLS.

### `dns`

- `dns.resolve(hostname, rrtype) -> Object`: Performs a full DNS query. Returns an object keyed by record type (e.g., `{ A: [], AAAA: [], MX: [] }`).

### `tls`

- `tls.createContext(jsResolverFunc) -> BigInt(ctxPtr)`: Creates a new SSL_CTX and stores it (Note: This is a raw pointer, not a HandleStore ID). It configures the context to use the provided JS function as the SNI certificateResolver.

- `tls.accept(ctxPtr, netSocketId) -> BigInt(id)`: Performs a TLS handshake on an accepted TCP socket. Returns a new TLS socket handle ID.

- `tls.read(id) -> String | null`: Reads decrypted data from a TLS socket.

- `tls.write(id, data) -> BigInt(bytesWritten)`: Writes encrypted data to a TLS socket.

- `tls.close(id)`: Shuts down and closes a TLS socket.

## 4. Public JavaScript API (`stdlib.js`)

This is the stable, user-facing API.

### `fs`

- `fs.open(path, flags = 'r', mode = 0o666) -> FileHandle`: Opens a file and returns a `FileHandle` object. `flags` is a string like `'r'`, `'w'`, `'a'`.

- `fs.readFile(path) -> String`: Helper function that opens, reads, and closes a file, returning its string content.

- `fs.readdir(path) -> Array[String]`

- `fs.unlink(path)`

- `fs.mkdir(path, mode = 0o777)`

- `fs.rmdir(path)`

- Class: `FileHandle`

	- `read(length, offset) -> Uint8Array | null`: Reads from the file.

	- `write(buffer, offset) -> BigInt`: Writes a `String` or `Uint8Array`.

	- `fstat() -> Object`: Returns file stats.

	- `fsync()`

	- `fdatasync()`

	- `close()`

### `dns`

	- `dns.resolve(hostname, rrtype = 'A') -> Object`: Resolves DNS records (A, AAAA, MX, TXT, SRV, etc.).

### `net`

- `net.createServer(connectionHandler) -> net.Server`

- Class: `net.Server`

	- `listen(port, host = '0.0.0.0')`: Starts the server. This function blocks the current fiber and runs the accept loop indefinitely.

- Class: `net.Socket`

	- `read() -> String | null`: Reads available data. Returns `null` on EOF.

	- `write(data)`: Writes a `String` or `Uint8Array`.

	- `close()`

### tls

- `tls.createServer(options, connectionHandler) -> tls.Server`

`options.certificateResolver (Function, Required)`: A JavaScript `function (servername) => ({ key: "...", cert: "..." })`. This function is called during the handshake to dynamically provide the correct certificate based on the client's SNI request.

- Class: `tls.Server`

- `listen(port, host = '0.0.0.0')`: Starts the TLS server. This function blocks the current fiber and runs the accept loop.

- Class: `tls.Socket`

- `read() -> String | null`: Reads decrypted data.

- `write(data)`: Writes encrypted data.

- `close()`

### Globals

- `Fiber.run(jsFunction)`: Runs jsFunction in a new coroutine.

- `console.log(...args)`: Prints to stdout.

## 5. Rationale and Design Decisions

- Stackful Fibers vs. `async/await`: We use fibers to eliminate "function coloring" and the syntactic overhead of `async/await` and `.then()`. This allows for a simpler, linear control flow (`while (true) { let data = socket.read(); }`), which is easier to reason about, especially with complex error handling and resource management.

- Native Bindings vs. FFI: While an FFI (like `libffi`) is flexible, it is also fragile. It creates a high-risk dependency on system-level ABI, `libuv` struct layouts, and versioning. Native C++ bindings provide a stable, high-performance, and maintainable abstraction layer that insulates the JavaScript API from the complexities of the underlying C libraries.

- HandleStore: Exposing raw C++ pointers to JavaScript is a major security and stability risk. The `HandleStore` acts as a "kernel boundary," ensuring that JS code can only interact with resources via a safe ID, preventing use-after-free, type confusion, and memory corruption.

- Fixed-Size Stacks: Implementing dynamically resizable stacks (stack "hot-swapping") is extraordinarily complex and a common source of bugs. By allocating a generous (1MB) fixed-size stack per fiber, we provide a simple, robust, and highly performant solution that is sufficient for all but the most extreme recursive use cases.

## future works
```js:runtime.js
let loadedExtensions = new Map();
import.meta.resolver = (path, from/base) => {
	if (loadedExtensions.has(path)) {
		return loadedExtensions.get(path);
	}

	// loader
	// remotely-loaded extension
	// locally-loaded extension

	// parser
	return import.meta.parse(source);
};
export let resolver = import.meta.resolver;
```

### in wasm
- WebCrypto API: https://github.com/jedisct1/openssl-wasm
- brotli: https://github.com/httptoolkit/brotli-wasm
- gzip: https://github.com/ColinTimBarndt/wasm-gzip
- libuv: https://github.com/nodejs/uvwasi

### see also
https://gemini.google.com/app/4dc9d4c215bb653a