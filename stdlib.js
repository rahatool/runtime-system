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

// Changed String.prototype.bytes to a function for consistency.
// Example: "hello".bytes()
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
		// Simplified check: must be a BigInt and greater than 0n
		if (typeof id !== 'bigint' || id <= 0n) {
			 throw new Error("Handle ID must be a positive BigInt");
		}
		this.id = id;
		handleRegistry.register(this, id);
	}
	_unregister() {
		handleRegistry.unregister(this);
	}
	// Base close implementation now performs safe unregistering.
	// Subclasses needing custom primitive calls must override this method.
	close() { this._unregister(); }
}

// --- Filesystem ---
export class FileHandle extends ExternalResource {
	// Changed signature to accept a single options object
	static open(path, options = {}) {
		const { flags = 'r', mode = 0o666 } = options;
		
		// POSIX flags: O_RDONLY=0, O_WRONLY=1, O_RDWR=2, O_CREAT=64, O_TRUNC=512, O_APPEND=1024
		const flagMap = {
			'r': 0, // O_RDONLY
			'w': 1 | 64 | 512, // O_WRONLY | O_CREAT | O_TRUNC
			'a': 1 | 64 | 1024, // O_WRONLY | O_CREAT | O_APPEND
			'r+': 2, // O_RDWR
			'w+': 2 | 64 | 512, // O_RDWR | O_CREAT | O_TRUNC
			'a+': 2 | 64 | 1024, // O_RDWR | O_CREAT | O_APPEND
		};
		const jsFlags = flagMap[flags];

		if (jsFlags === undefined) {
			throw new Error(`Unsupported file open flag: ${flags}`);
		}

		const id = primordials.fs.open(path, jsFlags, mode);
		return new FileHandle(id);
	}
	static unlink(path) {
		primordials.fs.unlink(path);
	}

	// Returns BigInt bytes read
	read(buffer, position = -1) {
		const offset = BigInt(position);
		const bytesRead = primordials.fs.read(this.id, buffer, offset);
		if (bytesRead === null) return null; // EOF
		return bytesRead; // Retained as BigInt
	}
	// Returns BigInt bytes written
	write(buffer, position = -1) {
		const offset = BigInt(position);
		const bytesWritten = primordials.fs.write(this.id, buffer, offset);
		return bytesWritten; // Retained as BigInt
	}
	status() { return primordials.fs.status(this.id); }
	sync() { return primordials.fs.sync(this.id); }
	dataSync() { return primordials.fs.dataSync(this.id); }
	close() { this._unregister(); return primordials.fs.close(this.id); }
	// Synchronous iterator for reading file contents
	 *[Symbol.iterator](chunkSize = 65536) {
		 const buffer = new Uint8Array(chunkSize);
		 try {
			 while(true) {
				 // Note: Calling read without position allows the internal file pointer to advance
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
	static make(path, mode = 0o777) {
		primordials.fs.dirMake(path, mode);
	}
	static remove(path) {
		primordials.fs.dirRemove(path);
	}

	read() {
		// Primitive returns array of { name: string, type: number } or null
		return primordials.fs.dirRead(this.id);
	}
	close() { this._unregister(); return primordials.fs.dirClose(this.id); }

	// Synchronous iterator for directory entries
	*[Symbol.iterator]() {
		try {
			while (true) {
				const entries = this.read();
				if (entries === null) break;
				yield* entries;
			}
		} finally {
			 // Close implicitly when iterator finishes or breaks
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

	// Simplified read: Reads into the provided Uint8Array view. Returns BigInt bytes read.
	read(buffer) {
		const bytesRead = primordials.tcp.read(this.id, buffer);
		if (bytesRead === null) return null; // EOF
		return bytesRead; // Retained as BigInt
	}
	// Simplified write: Writes the provided Uint8Array view. Returns BigInt bytes written.
	write(buffer) { // Expects Uint8Array (use string.bytes())
		const bytesWritten = primordials.tcp.write(this.id, buffer);
		return bytesWritten; // Retained as BigInt
	}
	close() { this._unregister(); primordials.tcp.close(this.id); }

	// Synchronous iterator for reading data chunks
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
			 throw e;
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

	constructor(id) { super(id); } // Store the server handle ID

	// Synchronous iterator for accepting connections
	*[Symbol.iterator]() {
		try {
			while(true) {
				// Accept blocks the fiber until a connection is ready
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
	close() { this._unregister(); primordials.tcp.close(this.id); }
}

// --- Dgram (UDP) ---
export class UDPSocket extends ExternalResource {
	// Factory method
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
		// Primitive returns { bytes: BigInt, remote: { host: string, port: number } } or null
		const result = primordials.udp.read(this.id, buffer);
		if (result === null) return null;
		// remote port is a standard Number, but bytes is now retained as BigInt
		return { bytes: result.bytes, remote: result.remote };
	}
	close() { this._unregister(); primordials.udp.close(this.id); }

	// Synchronous iterator for reading datagrams
	 *[Symbol.iterator](bufferSize = 65536) {
		 const buffer = new Uint8Array(bufferSize);
		 try {
			 while(true) {
				 const result = this.read(buffer);
				 if (result === null) break;
				 if (result.bytes > 0n) {
					 yield {
						 data: buffer.slice(0, Number(result.bytes)), // Convert BigInt to Number for slicing
						 remote: result.remote
					 };
				 }
			 }
		 } catch(e) {
			  console.log(`UDP read error: ${e.message}`);
			  throw e;
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
			const id = primordials.tls.connect(tcpSocket.id, host);
			 const tlsSocket = new TLSSocket(id);
			 tlsSocket._tcpSocketId = tcpSocket.id;
			 return tlsSocket;
		} catch (e) {
			tcpSocket.close();
			throw e;
		}
	}

	// Simplified read (no offset/length). Returns BigInt bytes read.
	read(buffer) {
		const bytesRead = primordials.tls.read(this.id, buffer);
		if (bytesRead === null) return null;
		return bytesRead; // Retained as BigInt
	}
	// Simplified write (no offset/length). Returns BigInt bytes written.
	write(buffer) { // Expects Uint8Array
		const bytesWritten = primordials.tls.write(this.id, buffer);
		return bytesWritten; // Retained as BigInt
	}
	close() {
		this._unregister();
		primordials.tls.close(this.id);
		// Close underlying TCP socket
		if (this._tcpSocketId) {
			 try { primordials.tcp.close(this._tcpSocketId); }
			 catch(e) { /* Ignore errors closing underlying socket */ }
		}
	}
	 // Synchronous iterator for reading data chunks
	 *[Symbol.iterator](chunkSize = 16384) {
		 const buffer = new Uint8Array(chunkSize);
		 try {
			 while(true) {
				 const bytesRead = this.read(buffer);
				 if (bytesRead === null) break;
				 if (bytesRead > 0n) {
					yield buffer.subarray(0, Number(bytesRead)); // Convert BigInt to Number for subarray
				 }
			 }
		 } catch(e) {
			 console.log(`TLS read error: ${e.message}`);
			 throw e;
		 }
	}
}

// TLSServer now extends ExternalResource to manage the TLS Context pointer
export class TLSServer extends ExternalResource {
	 // Factory method - Takes resolver, port, host
	 static listen(certificateResolver, port, host = '0.0.0.0') {
		 if (typeof certificateResolver !== 'function') throw new Error("TLSServer.listen requires a certificateResolver function");

		 // 1. Wrap the JS resolver (moved from constructor)
		 const internalResolver = (servername) => {
			 const result = certificateResolver(servername);
			 if (!result || !(result.key instanceof KeyHandle) || !(result.cert instanceof CertHandle)) {
				  throw new Error("certificateResolver must return { key: KeyHandle, cert: CertHandle }");
			 }
			 return { key: result.key.id, cert: result.cert.id };
		 };

		 // 2. Create the C++ context pointer (this becomes the ExternalResource ID)
		 const tls_context_ptr_id = primordials.tls.createContext(internalResolver);

		 // 3. Start the underlying TCP Server (moved from iterator/constructor check)
		 const tcpServer = TCPServer.listen(port, host);

		 console.log(`TLS Server listening on ${host}:${port}`);
		 // Pass context ptr ID and tcpServer instance to constructor
		 return new TLSServer(tls_context_ptr_id, tcpServer);
	 }

	 constructor(tls_context_ptr_id, tcpServer) {
		// Use the C++ TLS context pointer as the ExternalResource ID
		super(tls_context_ptr_id);
		this._tcpServer = tcpServer; // Store the TCPServer instance
	 }

	 // Synchronous iterator for accepting connections
	 *[Symbol.iterator]() {
		 try {
			 // Iterate over incoming TCP connections from the underlying TCPServer
			 for (const tcpSocket of this._tcpServer) {
				  let tlsSocket = null;
				  try {
					  // Perform TLS handshake (blocks fiber). Use this.id (context ptr)
					  const tlsSocketId = primordials.tls.accept(this.id, tcpSocket.id);
					  tlsSocket = new TLSSocket(tlsSocketId);
					  tlsSocket._tcpSocketId = tcpSocket.id;
					  yield tlsSocket;
				  } catch (e) {
					  console.log(`TLS Handshake Error: ${e.message} ${e.stack}`);
					  tcpSocket.close(); // Close underlying TCP on failure
				  }
			 }
		 } finally {
			  // TCPServer's iterator close handles the underlying server.
			  // Explicitly close the TLS context here.
			  this.close();
		 }
	 }
	 // Explicit close method to free context and close TCP server
	 close() {
		 this._unregister(); // Unregister the TLS context pointer
		 // Explicitly call the primitive to free the C++ TLSContext object
		 primordials.tls.freeContext(this.id);
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
