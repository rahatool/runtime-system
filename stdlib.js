const primordials = globalThis.__primordials;
if (!primordials) throw new Error("Fatal: primordials not found.");

// --- Fiber Control ---
export const Fiber = {
	run: primordials.fiber.run,
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
	// Base close unregisters. Subclasses needing explicit C++ close call override this.
	close() { this._unregister(); }
}

// --- Filesystem ---
export class FileHandle extends ExternalResource {
	// Deno-style options object
	static open(path, options = { read: true }) {
		const { read = false, write = false, append = false, create = false, truncate = false, mode = 0o666 } = options;
		
		let flags = 0;
		if (read && write) flags |= 2;      // O_RDWR
		else if (read) flags |= 0;           // O_RDONLY
		else if (write || append) flags |= 1;// O_WRONLY (required for append/truncate/create)
		
		if (append) flags |= 1024;           // O_APPEND
		if (create) flags |= 64;             // O_CREAT
		if (truncate) flags |= 512;          // O_TRUNC
		
		const id = primordials.fs.open(path, flags, mode);
		return new FileHandle(id);
	}
	// Renamed from unlink
	static remove(path) {
		primordials.fs.remove(path);
	}
	// Reads entire file content as Uint8Array
	static readAll(path) {
		const file = FileHandle.open(path, { read: true });
		try {
			const stat = file.status();
			const buffer = new Uint8Array(Number(stat.size)); // Size is BigInt, convert
			let totalRead = 0n;
			while (totalRead < stat.size) {
				const currentRead = file.read(buffer.subarray(Number(totalRead)), Number(totalRead));
				if (currentRead === null) break; // EOF unexpected?
				totalRead += currentRead;
			}
			return buffer;
		} finally {
			file.close();
		}
	}

	// Returns BigInt bytes read or null on EOF
	read(buffer, position = -1) {
		const offset = BigInt(position);
		return primordials.fs.read(this.id, buffer, offset);
	}
	// Returns BigInt bytes written
	write(buffer, position = -1) {
		const offset = BigInt(position);
		return primordials.fs.write(this.id, buffer, offset);
	}
	// Renamed from fstat
	status() { return primordials.fs.status(this.id); }
	// Renamed from fsync
	sync() { return primordials.fs.sync(this.id); }
	// Renamed from fdatasync
	dataSync() { return primordials.fs.dataSync(this.id); }
	// Overrides base close to call primitive
	close() { this._unregister(); return primordials.fs.close(this.id); }
	
	 *[Symbol.iterator](chunkSize = 65536) {
		 const buffer = new Uint8Array(chunkSize);
		 try {
			 while(true) {
				 // Read using current file pointer
				 const bytesRead = this.read(buffer);
				 if (bytesRead === null) break; // EOF
				 if (bytesRead > 0n) {
					yield buffer.subarray(0, Number(bytesRead));
				 }
			 }
		 } finally {
			 this.close();
		 }
	}
}

export class DirectoryHandle extends ExternalResource {
	static open(path) {
		const id = primordials.fs.dirOpen(path);
		return new DirectoryHandle(id);
	}
	// Renamed from dirMake
	static make(path, mode = 0o777) {
		primordials.fs.dirMake(path, mode);
	}
	// Renamed from dirRemove
	static remove(path) {
		primordials.fs.dirRemove(path);
	}

	// Renamed from dirRead
	read() {
		// Primitive returns array of { name: string, type: number } or null
		return primordials.fs.dirRead(this.id);
	}
	// Overrides base close
	close() { this._unregister(); return primordials.fs.dirClose(this.id); }

	*[Symbol.iterator]() {
		try {
			while (true) {
				const entries = this.read();
				if (entries === null) break;
				yield* entries;
			}
		} finally {
			 this.close();
		}
	}
}

// --- DNS ---
export const dns = {
	resolve: (hostname, rrtype = 'A') => {
		// Primitive returns parsed JS object or throws
		return primordials.dns.resolve(hostname, rrtype);
	},
};

// --- Networking (TCP) ---
export class TCPSocket extends ExternalResource {
	// Factory method
	static connect(host, port) {
		const id = primordials.tcp.connect(host, port);
		return new TCPSocket(id);
	}

	read(buffer) {
		const bytesRead = primordials.tcp.read(this.id, buffer);
		if (bytesRead === null) return null; // EOF
		return bytesRead; // Retained as BigInt
	}
	write(buffer) { // Expects Uint8Array (use string.bytes())
		const bytesWritten = primordials.tcp.write(this.id, buffer);
		return bytesWritten; // Retained as BigInt
	}
	// Overrides base close
	close() { this._unregister(); primordials.tcp.close(this.id); }

	*[Symbol.iterator](chunkSize = 65536) {
		 const buffer = new Uint8Array(chunkSize);
		 try {
			 while(true) {
				 const bytesRead = this.read(buffer);
				 if (bytesRead === null) break; // EOF
				 if (bytesRead > 0n) {
					yield buffer.subarray(0, Number(bytesRead));
				 }
			 }
		 } catch (e) {
			 console.log(`TCP Socket read error: ${e.message}`);
			 // Don't re-throw, just end iteration
		 }
	}
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
		} finally {
			this.close();
		}
	}
	// Overrides base close
	close() { this._unregister(); primordials.tcp.close(this.id); }
}

// --- Dgram (UDP) ---
export class UDPSocket extends ExternalResource {
	// Factory method - Renamed from listen
	static listen(port, host = '0.0.0.0') {
		 const id = primordials.udp.create();
		 primordials.udp.listen(id, host, port);
		 console.log(`UDP Socket listening on ${host}:${port}`);
		 return new UDPSocket(id);
	}

	// Renamed from send
	write(buffer, port, host) {
		primordials.udp.write(this.id, buffer, host, port);
	}
	// Renamed from recv
	read(buffer) {
		// Primitive returns { bytes: BigInt, host: string, port: number } or null
		return primordials.udp.read(this.id, buffer);
	}
	// Overrides base close
	close() { this._unregister(); primordials.udp.close(this.id); }

	 *[Symbol.iterator](bufferSize = 65536) {
		 const buffer = new Uint8Array(bufferSize);
		 try {
			 while(true) {
				 const result = this.read(buffer);
				 if (result === null) break; // Socket closed?
				 if (result.bytes > 0n) {
					 yield {
						 // Use subarray instead of slice
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

export class TLSSocket extends ExternalResource {
	 // Factory method
	 static connect(host, port, options = {}) {
		const tcpSocket = TCPSocket.connect(host, port);
		try {
			// Pass the tcpSocket.id (a BigInt) to the primitive
			const id = primordials.tls.connect(tcpSocket.id, host);
			 const tlsSocket = new TLSSocket(id);
			 // Store the underlying socket ID to close it later
			 tlsSocket._tcpSocketId = tcpSocket.id;
			 return tlsSocket;
		} catch (e) {
			tcpSocket.close();
			throw e;
		}
	}

	read(buffer) {
		const bytesRead = primordials.tls.read(this.id, buffer);
		if (bytesRead === null) return null; // EOF or error
		return bytesRead; // Retained as BigInt
	}
	write(buffer) { // Expects Uint8Array
		const bytesWritten = primordials.tls.write(this.id, buffer);
		return bytesWritten; // Retained as BigInt
	}
	// Overrides base close
	close() {
		this._unregister();
		try {
			primordials.tls.close(this.id); // Closes SSL* layer
		} catch(e) {
			 console.log(`TLS close error: ${e.message}`);
		}
		// Also close the underlying TCP socket
		if (this._tcpSocketId) {
			 try { primordials.tcp.close(this._tcpSocketId); }
			 catch(e) { /* Ignore errors closing underlying socket */ }
		}
	}
	 *[Symbol.iterator](chunkSize = 16384) {
		 const buffer = new Uint8Array(chunkSize);
		 try {
			 while(true) {
				 const bytesRead = this.read(buffer);
				 if (bytesRead === null) break; // EOF or error
				 if (bytesRead > 0n) {
					yield buffer.subarray(0, Number(bytesRead));
				 }
			 }
		 } catch(e) {
			 console.log(`TLS read error: ${e.message}`);
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
			 // Pass BigInt IDs to C++
			 return { key: result.key.id, cert: result.cert.id };
		 };

		 const tls_context_id = primordials.tls.createContext(internalResolver);
		 const tcpServer = TCPServer.listen(port, host); // Use the existing factory

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
				  let tlsSocket = null;
				  try {
					  // this.id is the tls_context_id
					  const tlsSocketId = primordials.tls.accept(this.id, tcpSocket.id);
					  tlsSocket = new TLSSocket(tlsSocketId);
					  tlsSocket._tcpSocketId = tcpSocket.id; // Give it the underlying ID
					  yield tlsSocket;
				  } catch (e) {
					  console.log(`TLS Handshake Error: ${e.message}`);
					  tcpSocket.close();
				  }
			 }
		 } finally {
			  this.close();
		 }
	 }
	 
	 // Overrides base close
	 close() {
		 this._unregister(); // Unregisters the TLSContextHandle
		 // FinalizationRegistry will call primordials.handles.free(this.id) eventually
		 
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

