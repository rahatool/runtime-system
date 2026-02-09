// public APIs of `runtime.js`

let check = (assertion, message) => {
	if (!assertion) {
		throw new Error(message);
	}
};

const primordials = globalThis.__primordials;
check(primordials, "Fatal: primordials not found.");

// --- Fiber Control ---
export const Fiber = {
	run: primordials.fiber.run,
	current: primordials.fiber.current,
	sleep: (ms) => primordials.fiber.sleep(BigInt(ms)),
};

// --- Encoding ---
export class TextEncoder {
	encode(str) {
		return primordials.encoding.encode(str);
	}
};
export class TextDecoder {
	decode(buffer) {
		return primordials.encoding.decode(buffer);
	}
};
String.prototype.toBytes = function() {
	return primordials.encoding.encode(this);
};
Uint8Array.prototype.toString = function() {
	return primordials.encoding.decode(this);
};

// --- Handle Cleanup ---
const handleRegistry = new FinalizationRegistry((id) => {
	primordials.handles.free(id);
});

// --- Base class for external resources ---
class ExternalResource {
	constructor(id) {
		check(typeof id === 'bigint', "Handle ID must be a positive BigInt");
		this.id = id;
		handleRegistry.register(this, id);
	}
	_unregister() {
		handleRegistry.unregister(this);
	}
	close() {
		this._unregister();
	}
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
		for (const chunk of this) {
			// chunk is a subarray view, which will be overwritten.
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
			check(bytesWritten !== -1n, "Write failed, stream closed prematurely or error occurred");
			totalWritten += bytesWritten;
		}
		return totalWritten;
	}

	*[Symbol.iterator](chunkSize = 65536) {
		const buffer = new Uint8Array(chunkSize);
		while (true) {
			// Read using current file pointer or stream
			const bytesRead = this.read(buffer);
			if (bytesRead === -1n) break; // EOF is -1n
			if (bytesRead > 0n) {
				// yield a *view* (subarray) of the bytes read
				yield buffer.subarray(0, Number(bytesRead));
			}
		}
	}
}

// --- Filesystem ---
export class FileHandle extends StreamHandle {
	static open(path, options = {}) {
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

	status() {
		return primordials.fs.status(this.id);
	}
	sync() {
		return primordials.fs.sync(this.id);
	}
	dataSync() {
		return primordials.fs.dataSync(this.id);
	}

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

	close() {
		this._unregister();
		return primordials.fs.dirClose(this.id);
	}

	*[Symbol.iterator]() {
		while (true) {
			const entry = this.read();
			if (entry === null) break;
			yield entry;
		}
	}
}

// --- DNS ---

function httpsGet(host, path) {
	try {
		using socket = TLSSocket.connect(host, 443);
		const request =
			`GET ${path} HTTP/1.1\r\n` +
			`Host: ${host}\r\n` +
			`User-Agent: v8-runtime/1\r\n` +
			`Accept: application/dns-json\r\n` +
			`Connection: close\r\n` +
			`\r\n`;
		socket.writeAll(request.toBytes());
		const response = socket.readAll().toString();
		const sep = response.indexOf("\r\n\r\n");
		check(sep !== -1, "Invalid HTTP response");
		const body = response.slice(sep + 4);
		return JSON.parse(body);
	} catch (fault) {
		throw new Error(`HTTPS DNS lookup failed: ${fault.message}`);
	}
}

const RRNUM = { A: 1, AAAA: 28, MX: 15, TXT: 16, CNAME: 5, NS: 2 };

export const dns = {
    resolve: (hostname, rrtype = 'A') => {
        const upper = String(rrtype).toUpperCase();
        if (upper === 'A' || upper === 'AAAA') {
            return primordials.dns.resolve(hostname, upper);
        }
        const type = RRNUM[upper];
        check(type, 'DNS record type not supported');
        const q = encodeURIComponent(hostname);
        const response = httpsGet('dns.google', `/resolve?name=${q}&type=${type}`);
        check(response.Status === 0 && response.Answer instanceof Array);
        // Map common record types to simple objects
        return response.Answer.map(a => {
            // a: { name, type, TTL, data }
            switch (upper) {
                case 'CNAME':
                case 'NS':
                    return { value: a.data.replace(/\.$/, ''), ttl: a.TTL };
                case 'TXT': {
                    const m = a.data.match(/^"(.*)"$/);
                    return { value: m ? m[1] : a.data, ttl: a.TTL };
                }
                case 'MX': {
                    const m = a.data.match(/^(\d+)\s+(.*)$/);
                    return m ? { priority: Number(m[1]), exchange: m[2].replace(/\.$/, ''), ttl: a.TTL } : { value: a.data, ttl: a.TTL };
                }
                default:
                    return { value: a.data, ttl: a.TTL };
            }
        });
    },
};

// --- Networking (TCP) ---
export class TCPSocket extends StreamHandle {
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

	close() {
		this._unregister();
		primordials.tcp.close(this.id);
	}
}

export class TCPServer extends ExternalResource {
	static listen(port, host = '0.0.0.0') {
		const id = primordials.tcp.listen(host, port);
		return new TCPServer(id);
	}

	*[Symbol.iterator]() {
		while (true) {
			const clientId = primordials.tcp.accept(this.id);
			if (clientId < 0n) {
				console.log(`TCP Accept error: ${clientId}`);
				break;
			}
			yield new TCPSocket(clientId);
		}
	}

	close() {
		this._unregister();
		primordials.tcp.close(this.id);
	}
}

// --- Dgram (UDP) ---
export class UDPSocket extends ExternalResource {
	static listen(port, host = '0.0.0.0') {
		const id = primordials.udp.create();
		primordials.udp.listen(id, host, port);
		return new UDPSocket(id);
	}

	write(buffer, port, host) {
		primordials.udp.write(this.id, buffer, host, port);
	}

	// Returns { bytes: BigInt, host: string, port: number } or null
	read(buffer) {
		return primordials.udp.read(this.id, buffer);
	}

	close() {
		this._unregister();
		primordials.udp.close(this.id);
	}

	*[Symbol.iterator](bufferSize = 65536) {
		const buffer = new Uint8Array(bufferSize);
		while (true) {
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
	static connect(host, port, options = {}) {
		const tcpSocket = TCPSocket.connect(host, port);
		try {
			// We pass the tcpSocket *ID* to the connect primitive
			const id = primordials.tls.connect(tcpSocket.id, host);
			const tlsSocket = new TLSSocket(id);
			tlsSocket._tcpSocket = tcpSocket; // Store the JS object for closing
			return tlsSocket;
		} catch (fault) {
			tcpSocket.close();
			throw fault;
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
		} catch(fault) {
			 console.log(`TLS close error: ${fault.message}`);
		}
		// Also close the underlying TCP socket
		if (this._tcpSocket) {
			try {
				this._tcpSocket.close();
			} catch(fault) {
				/* Ignore errors closing underlying socket */
			}
		}
	}
}

// TLSServer manages the TLSContextHandle
export class TLSServer extends ExternalResource {
	static listen(certificateResolver, port, host = '0.0.0.0') {
		check(typeof certificateResolver == 'function', "TLSServer.listen requires a certificateResolver function");

		const internalResolver = (serverName) => {
			const result = certificateResolver(serverName);
			check(result && result.key instanceof KeyHandle && result.cert instanceof CertHandle, "certificateResolver must return { key: KeyHandle, cert: CertHandle }");
			// Pass the BigInt IDs to C++
			return { key: result.key.id, cert: result.cert.id };
		};

		const tlsContextId = primordials.tls.createContext(internalResolver);
		try {
			let tcpServer = TCPServer.listen(port, host);
			return new TLSServer(tlsContextId, tcpServer);
		} catch(fault) {
			primordials.handles.free(tlsContextId);
			throw fault;
		}
	}

	constructor(tlsContextId, tcpServer) {
		super(tlsContextId);
		this._tcpServer = tcpServer;
	}

	*[Symbol.iterator]() {
		// Iterate over incoming TCP connections from the underlying TCPServer
		for (const tcpSocket of this._tcpServer) {
			try {
				// this.id is the tls_context_id, tcpSocket.id is the net_handle_id
				const tlsSocketId = primordials.tls.accept(this.id, tcpSocket.id);
				const tlsSocket = new TLSSocket(tlsSocketId);
				tlsSocket._tcpSocket = tcpSocket; // Store JS object for closing
				yield tlsSocket;
			} catch (fault) {
				console.log(`TLS Handshake Error: ${fault.message}`);
				tcpSocket.close(); // Close the underlying TCP socket on handshake failure
			}
		}
	}

	close() {
		this._unregister();
		primordials.handles.free(this.id);

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