const primordials = globalThis.__primordials;
if (!primordials) throw new Error("Fatal: primordials not found.");

// --- Fiber Control ---
const Fiber = {
	run: primordials.fiber.run,
};
globalThis.Fiber = Fiber;

// --- Filesystem ---
class FileHandle {
	constructor(id) {
		this.id = id;
	}
	read(length = 16384, offset = -1) {
		return primordials.fs.read(this.id, BigInt(length), BigInt(offset));
	}
	write(buffer, offset = -1) {
		if (typeof buffer === 'string') {
			buffer = new TextEncoder().encode(buffer);
		}
		return primordials.fs.write(this.id, buffer, BigInt(offset));
	}
	fstat() {
		return primordials.fs.fstat(this.id);
	}
	fsync() {
		return primordials.fs.fsync(this.id);
	}
	fdatasync() {
		return primordials.fs.fdatasync(this.id);
	}
	close() {
		return primordials.fs.close(this.id);
	}
}

export const fs = {
	open: (path, flags = 'r', mode = 0o666) => {
		// Simple flag parser
		const flagMap = { 'r': 0, 'w': 1 | 64 | 512, 'a': 1 | 64 | 1024 };
		const jsFlags = flagMap[flags] || 0;
		const id = primordials.fs.open(path, jsFlags, mode);
		return new FileHandle(id);
	},
	readFile: (path) => {
		// This is a helper, not a primitive. We can build it in JS.
		const file = fs.open(path, 'r');
		try {
			const stats = file.fstat();
			const data = file.read(stats.size, 0);
			return new TextDecoder().decode(data);
		} finally {
			file.close();
		}
	},
	readdir: primordials.fs.readdir,
	unlink: primordials.fs.unlink,
	mkdir: (path, mode = 0o777) => primordials.fs.mkdir(path, mode),
	rmdir: primordials.fs.rmdir,
};

// --- DNS ---
export const dns = {
	resolve: (hostname, rrtype = 'A') => {
		return primordials.dns.resolve(hostname, rrtype);
	},
};

// --- Networking ---
export const net = (() => {
	class Socket {
		constructor(id) {
			this.id = id;
		}
		read() {
			return primordials.net.read(this.id);
		}
		write(data) {
			return primordials.net.write(this.id, data);
		}
		close() {
			primordials.net.close(this.id);
		}
	}

	class Server {
		constructor(connectionHandler) {
			this._connectionHandler = connectionHandler;
			this.id = null;
		}
		listen(port, host = '0.0.0.0') {
			this.id = primordials.net.listen(host, port);
			console.log(`TCP Server listening on ${host}:${port}`);
			while (true) {
				const clientId = primordials.net.accept(this.id);
				if (clientId < 0n) {
					console.log(`Accept error: ${clientId}`);
					continue;
				}
				const socket = new Socket(clientId);
				Fiber.run(() => {
					try {
						this._connectionHandler(socket);
					} catch (e) {
						console.log(`Error in connection handler: ${e.stack}`);
					}
				});
			}
		}
	}

	return {
		createServer: (connectionHandler) => new Server(connectionHandler),
	};
})();

// --- TLS ---
export const tls = (() => {
	class TLSSocket {
		constructor(id) {
			this.id = id;
		}
		read() {
			return primordials.tls.read(this.id);
		}
		write(data) {
			return primordials.tls.write(this.id, data);
		}
		close() {
			primordials.tls.close(this.id);
		}
	}

	class Server {
		 constructor(options, connectionHandler) {
			this._connectionHandler = connectionHandler;
			if (!options.certificateResolver) {
				throw new Error("tls.createServer requires an 'options.certificateResolver' function");
			}
			this._tcpServer = net.createServer(this._onConnection.bind(this));
			this._tls_context = primordials.tls.createContext(options.certificateResolver);
		 }
		 listen(port, host = '0.0.0.0') {
			 console.log(`TLS Server listening on ${host}:${port}`);
			 this._tcpServer.listen(port, host);
		 }
		 _onConnection(tcpSocket) {
			 try {
				// tcpSocket is a net.Socket, .id is the raw NetHandle ID
				const tlsSocketId = primordials.tls.accept(this._tls_context, tcpSocket.id);
				console.log("TLS handshake complete!");
				this._connectionHandler(new TLSSocket(tlsSocketId));
			 } catch (e) {
				 console.log(`TLS Handshake Error: ${e.message}`);
				 tcpSocket.close();
			 }
		 }
	}
	
	return {
		createServer: (options, connectionHandler) => new Server(options, connectionHandler),
	};
})();

// --- Console ---
globalThis.console = {
	log: (...args) => {
		const msg = args.map(String).join(' ') + '\n';
		print(msg);
	}
};