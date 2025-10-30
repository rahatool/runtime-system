const primordials = globalThis.__primordials;
if (!primordials) throw new Error("Fatal: primordials not found.");

// --- Fiber Control ---
export const Fiber = {
	run: primordials.fiber.run, // No longer returns a Promise
	current: primordials.fiber.current,
	sleep: (ms) => primordials.fiber.sleep(BigInt(ms)),
};

// --- Encoding ---
const _TextEncoder = class { encode(str) { return primordials.encoding.encode(str); } };
const _TextDecoder = class { decode(buffer) { return primordials.encoding.decode(buffer); } };
export { _TextEncoder as TextEncoder };
export { _TextDecoder as TextDecoder };
const textEncoder = new _TextEncoder();

String.prototype.bytes = function() {
	return textEncoder.encode(this.toString());
};

// --- Handle Cleanup ---
const handleRegistry = new FinalizationRegistry((id) => {
	primordials.handles.free(id);
});

// --- Base class for external resources ---
class ExternalResource {
	constructor(id) {
		if (typeof id !== 'bigint' || id <= 0n) {
			 throw new Error("Handle ID must be a positive BigInt");
		}
		this.id = id;
		handleRegistry.register(this, id);
	}
	_unregister() {
		handleRegistry.unregister(this);
	}
	// Base close unregisters.
	close() { this._unregister(); }

	// Implement explicit resource management symbol
	[Symbol.dispose]() {
		this.close();
	}
}

// --- Base Class for Stream I/O ---
class StreamHandle extends ExternalResource {
	/**
	 * Reads all data from the stream until EOF.
	 * This method repeatedly calls .read() and concatenates the chunks.
	 * @returns {Uint8Array} A new buffer containing all data.
	 */
	readAll() {
		const chunks = [];
		let totalBytes = 0;
		// Use for...of on the iterator
		for (const chunk of this) {
			// chunk is a subarray view, which will be overwritten.
			// We must make a copy using slice().
			chunks.push(chunk.slice());
			totalBytes += chunk.length;
		}

		// Concatenate all chunks into one new buffer
		const result = new Uint8Array(totalBytes);
		let offset = 0;
		for (const chunk of chunks) {
			result.set(chunk, offset);
			offset += chunk.length;
		}
		return result;
	}

	/**
	 * Writes all data from the buffer to the stream.
	 * This method repeatedly calls .write() until the buffer is fully sent.
	 * @param {Uint8Array} buffer The buffer to write.
	 * @returns {BigInt} The total number of bytes written.
	 */
	writeAll(buffer) {
		let totalWritten = 0n;
		const totalLength = BigInt(buffer.length);
		while (totalWritten < totalLength) {
			// Use subarray for the remaining part (this is a view, which is efficient)
			const bytesWritten = this.write(buffer.subarray(Number(totalWritten)));
			if (bytesWritten === -1n) { // Check for errors/closure
				throw new Error("Write failed, stream closed prematurely or error occurred");
			}
			totalWritten += bytesWritten;
		}
		return totalWritten;
	}

	*[Symbol.iterator](chunkSize = 65536) {
		 const buffer = new Uint8Array(chunkSize);
		 try {
			 while(true) {
				 // Read using current file pointer or stream
				 const bytesRead = this.read(buffer);
				 if (bytesRead === -1n) break; // EOF is -1n
				 if (bytesRead > 0n) {
					// yield a *view* (subarray) of the bytes read
					yield buffer.subarray(0, Number(bytesRead));
				 }
			 }
		 } catch (e) {
			 console.log(`Stream read error: ${e.message}`);
		 }
	}
}

// --- Filesystem ---
export class FileHandle extends StreamHandle {
	// Deno-style options object
	static open(path, options = {}) { // Default options to {}
		// Default read to true
		const { read = true, write = false, append = false, create = false, truncate = false, mode = 0o666 } = options;

		let flags = 0;
		// POSIX flags
		if (read && write) flags |= 2;      // O_RDWR
		else if (read) flags |= 0;           // O_RDONLY
		else if (write || append) flags |= 1;// O_WRONLY

		if (append) flags |= 1024;           // O_APPEND
		if (create) flags |= 64;             // O_CREAT
		if (truncate) flags |= 512;          // O_TRUNC

		const id = primordials.fs.open(path, flags, mode);
		return new FileHandle(id);
	}

	static remove(path) {
		primordials.fs.remove(path);
	}

	// Returns BigInt bytes read or -1n on EOF
	read(buffer, position = -1) {
		const offset = BigInt(position);
		return primordials.fs.read(this.id, buffer, offset);
	}
	// Returns BigInt bytes written
	write(buffer, position = -1) {
		const offset = BigInt(position);
		return primordials.fs.write(this.id, buffer, offset);
	}

	status() { return primordials.fs.status(this.id); }
	sync() { return primordials.fs.sync(this.id); }
	dataSync() { return primordials.fs.dataSync(this.id); }

	// Overrides base close to call primitive
	close() {
		this._unregister();
		return primordials.fs.close(this.id);
	}
}

export class DirectoryHandle extends ExternalResource {
	static open(path) {
		const id = primordials.fs.dirOpen(path);
		return new DirectoryHandle(id);
	}

	static make(path, mode = 0o777) {
		primordials.fs.dirMake(path, mode);
	}

	static remove(path) {
		primordials.fs.dirRemove(path);
	}

	read() {
		// Primitive returns { name: string, type: number } or null
		return primordials.fs.dirRead(this.id);
	}

	close() { this._unregister(); return primordials.fs.dirClose(this.id); }

	*[Symbol.iterator]() {
		while (true) {
			const entry = this.read();
			if (entry === null) break;
			yield entry;
		}
	}
}

// --- DNS ---
export const dns = {
	resolve: (hostname, rrtype = 'A') => {
		return primordials.dns.resolve(hostname, rrtype);
	},
};

// --- Networking (TCP) ---
export class TCPSocket extends StreamHandle {
	// Factory method
	static connect(host, port) {
		const id = primordials.tcp.connect(host, port);
		return new TCPSocket(id);
	}

	// Returns BigInt bytes read or -1n on EOF
	read(buffer) {
		const bytesRead = primordials.tcp.read(this.id, buffer);
		return bytesRead;
	}
	// Returns BigInt bytes written
	write(buffer) { // Expects Uint8Array
		const bytesWritten = primordials.tcp.write(this.id, buffer);
		return bytesWritten;
	}

	close() { this._unregister(); primordials.tcp.close(this.id); }
}

export class TCPServer extends ExternalResource {
	// Factory method
	static listen(port, host = '0.0.0.0') {
		const id = primordials.tcp.listen(host, port);
		console.log(`TCP Server listening on ${host}:${port}`);
		return new TCPServer(id);
	}

	*[Symbol.iterator]() {
		try {
			while(true) {
				const clientId = primordials.tcp.accept(this.id);
				if (clientId < 0n) {
					 console.log(`TCP Accept error: ${clientId}`);
					 break;
				}
				yield new TCPSocket(clientId);
			}
		} catch (e) {
			console.log(`TCP Server error: ${e.message}`);
		}
	}

	close() { this._unregister(); primordials.tcp.close(this.id); }
}

// --- Dgram (UDP) ---
export class UDPSocket extends ExternalResource {
	static listen(port, host = '0.0.0.0') {
		 const id = primordials.udp.create();
		 primordials.udp.listen(id, host, port);
		 console.log(`UDP Socket listening on ${host}:${port}`);
		 return new UDPSocket(id);
	}

	write(buffer, port, host) {
		primordials.udp.write(this.id, buffer, host, port);
	}

	// Returns { bytes: BigInt, host: string, port: number } or null
	read(buffer) {
		return primordials.udp.read(this.id, buffer);
	}

	close() { this._unregister(); primordials.udp.close(this.id); }

	 *[Symbol.iterator](bufferSize = 65536) {
		 const buffer = new Uint8Array(bufferSize);
		 try {
			 while(true) {
				 const result = this.read(buffer);
				 if (result === null) break; // Socket closed
				 if (result.bytes > 0n) {
					 // Return object includes host and port directly
					 yield {
						 data: buffer.subarray(0, Number(result.bytes)),
						 host: result.host,
						 port: result.port
					 };
				 }
			 }
		 } catch(e) {
			  console.log(`UDP read error: ${e.message}`);
		 }
	}
}

// --- TLS ---
export class KeyHandle extends ExternalResource {
	constructor(pem) { // Expects Uint8Array
		super(primordials.tls.parseKey(pem));
	}
}
export class CertHandle extends ExternalResource {
	 constructor(pem) { // Expects Uint8Array
		super(primordials.tls.parseCert(pem));
	}
}

export class TLSSocket extends StreamHandle {
	 // Factory method
	 static connect(host, port, options = {}) {
		const tcpSocket = TCPSocket.connect(host, port);
		try {
			// We pass the tcpSocket *ID* to the connect primitive
			const id = primordials.tls.connect(tcpSocket.id, host);
			const tlsSocket = new TLSSocket(id);
			tlsSocket._tcpSocket = tcpSocket; // Store the JS object for closing
			return tlsSocket;
		} catch (e) {
			tcpSocket.close();
			throw e;
		}
	}

	// Returns BigInt bytes read or -1n on EOF/Error
	read(buffer) {
		const bytesRead = primordials.tls.read(this.id, buffer);
		return bytesRead;
	}
	// Returns BigInt bytes written
	write(buffer) { // Expects Uint8Array
		const bytesWritten = primordials.tls.write(this.id, buffer);
		return bytesWritten;
	}

	close() {
		this._unregister();
		try {
			primordials.tls.close(this.id); // Closes SSL* layer
		} catch(e) {
			 console.log(`TLS close error: ${e.message}`);
		}
		// Also close the underlying TCP socket
		if (this._tcpSocket) {
			 try { this._tcpSocket.close(); }
			 catch(e) { /* Ignore errors closing underlying socket */ }
		}
	}
}

// TLSServer manages the TLSContextHandle
export class TLSServer extends ExternalResource {
	 // Factory method - Takes resolver, port, host
	 static listen(certificateResolver, port, host = '0.0.0.0') {
		 if (typeof certificateResolver !== 'function') {
			 throw new Error("TLSServer.listen requires a certificateResolver function");
		 }

		 const internalResolver = (servername) => {
			 const result = certificateResolver(servername);
			 if (!result || !(result.key instanceof KeyHandle) || !(result.cert instanceof CertHandle)) {
				  throw new Error("certificateResolver must return { key: KeyHandle, cert: CertHandle }");
			 }
			 // Pass the BigInt IDs to C++
			 return { key: result.key.id, cert: result.cert.id };
		 };

		 // 1. Create the C++ TLS Context (returns a handle ID)
		 const tls_context_id = primordials.tls.createContext(internalResolver);
		 let tcpServer;
		 try {
			 // 2. Create the underlying TCP Server
			 tcpServer = TCPServer.listen(port, host); // Use the existing factory
		 } catch(e) {
			 // If TCP listen fails, free the TLS context we just made
			 primordials.handles.free(tls_context_id);
			 throw e;
		 }

		 console.log(`TLS Server listening on ${host}:${port}`);
		 return new TLSServer(tls_context_id, tcpServer);
	 }

	 constructor(tls_context_id, tcpServer) {
		super(tls_context_id); // This ID is the TLSContextHandle
		this._tcpServer = tcpServer; // Store the TCPServer instance
	 }

	 *[Symbol.iterator]() {
		 try {
			 // Iterate over incoming TCP connections from the underlying TCPServer
			 for (const tcpSocket of this._tcpServer) {
				  try {
					  // this.id is the tls_context_id, tcpSocket.id is the net_handle_id
					  const tlsSocketId = primordials.tls.accept(this.id, tcpSocket.id);
					  const tlsSocket = new TLSSocket(tlsSocketId);
					  tlsSocket._tcpSocket = tcpSocket; // Store JS object for closing
					  yield tlsSocket;
				  } catch (e) {
					  console.log(`TLS Handshake Error: ${e.message}`);
					  tcpSocket.close(); // Close the underlying TCP socket on handshake failure
				  }
			 }
		 } catch (e) {
			  console.log(`TLS Server error: ${e.message}`);
		 }
	 }

	 close() {
		 this._unregister(); // Unregisters the TLSContextHandle
		 // FinalizationRegistry will call primordials.handles.free(this.id)

		 if (this._tcpServer) {
			 this._tcpServer.close();
		 }
	 }
}

// --- Console ---
globalThis.console = {
	log: (...args) => {
		const msg = args.map(String).join(' ') + '\n';
		print(msg);
	}
};

