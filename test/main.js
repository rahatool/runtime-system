import { Fiber, FileHandle, DirectoryHandle, dns, TCPServer, UDPSocket, TLSServer, KeyHandle, CertHandle, TextDecoder, TLSSocket } from '../stdlib.js';

console.log("--- Advanced Runtime Starting (Full DNS, FS, SNI) ---");

let fsExample = () => {
	using directory = DirectoryHandle.open("../test");
	try {
		console.log("Reading directory '.' ...");
		using directory = DirectoryHandle.open("../test");
		for (const entry of directory) {
			console.log(`Found: ${entry.name} (type: ${entry.type})`);
		}
	} catch (fault) {
		console.log(`fs.readdir failed: ${fault.message}`);
	}
};

let dnsExample = () => {
	try {
		console.log("Resolving 'google.com' A records...");
		let a_records = dns.resolve('google.com', 'A');
		console.log("Google A:", a_records[0]);
		return;

		console.log("Resolving 'google.com' AAAA records...");
		const aaaa_records = dns.resolve('google.com', 'AAAA');
		console.log("Google AAAA:", aaaa_records[0]);

		console.log("Resolving 'google.com' MX records...");
		const mx_records = dns.resolve('google.com', 'MX');
		console.log("Google MX:", mx_records[0]);
	} catch (fault) {
		console.log(`DNS lookup failed: ${fault.message}`);
	}
}

let udpServer = () => {
	try {
		using socket = UDPSocket.listen(8081);
		// Fiber.run(udpClient);
		for (const dgram of socket) {
			console.log(`UDP datagram from ${dgram.host}:${dgram.port}`);
			socket.write("UDP> ".toBytes(), dgram.port, dgram.host);
			socket.write(dgram.data, dgram.port, dgram.host);
		}
	} catch (fault) {
		console.log(`UDP Socket Error: ${fault.message}`);
	}
};
let udpClient = () => {
	// const id = primordials.udp.create();
	let write = (buffer, port, host) => {
		primordials.udp.write(id, buffer, host, port);
	};
	// write('Hi!'.toBytes(), 8081, '0.0.0.0');
	// write('Hi!'.toBytes(), 8081, '0.0.0.0');
};

let tcpServer = () => {
	try {
		using server = TCPServer.listen(8080);
		for (const socket of server) {
			console.log("TCP connection accepted!");
			Fiber.run(() => {
				try {
					using connection = socket;
					for (const chunk of connection) {
						connection.write("TCP> ".toBytes());
						connection.write(chunk);
					}
				} catch (fault) {
					console.log(`TCP socket error: ${fault.message}`);
				}
			});
		}
	} catch (fault) {
		console.log(`TCP Server Error: ${fault.message}`);
	}
};
let tcpClient = () => {
	try {
		using socket = TCPSocket.connect('127.0.0.1', 8080);
		socket.write('Hi!'.toBytes());
		for (const chunk of socket) {
			console.log(chunk);
		}
	} catch (fault) {
		console.log(`TCP Client Error: ${fault.message}`);
	}
};

let tlsServer = () => {
	// Create self-signed certificates to run this demo:
	// openssl req -x509 -newkey rsa:2048 -nodes -keyout a.key -out a.crt -days 365 -subj "/CN=a.example.com"
	// openssl req -x509 -newkey rsa:2048 -nodes -keyout b.key -out b.crt -days 365 -subj "/CN=b.example.com"
	//
	// Then connect:
	// openssl s_client -connect localhost:8443 -crlf -servername a.example.com
	// openssl s_client -connect localhost:8443 -crlf -servername b.example.com
	
	let certificates;
	try {
		let path = '../test/';
		certificates = {
			'a.example.com': {
				key: new KeyHandle(FileHandle.open(path + 'a.key').readAll()),
				cert: new CertHandle(FileHandle.open(path + 'a.crt').readAll()),
			},
			'b.example.com': {
				key: new KeyHandle(FileHandle.open(path + 'b.key').readAll()),
				cert: new CertHandle(FileHandle.open(path + 'b.crt').readAll()),
			}
		};
		console.log("Loaded certificates for: a.example.com, b.example.com");
	} catch (fault) {
		console.log(`Failed to load certificates: ${fault.message}`);
		console.log("Make sure 'a.key', 'a.crt', 'b.key', and 'b.crt' exist.");
		return;
	}
	let certificateResolver = (serverName) => {
		console.log(`SNI: Client requested server: ${serverName}`);
		const certs = certificates[serverName];
		if (certs) {
			console.log(`Found matching cert for ${serverName}`);
			return certs; // Return { key: KeyHandle, cert: CertHandle }
		}
		console.log(`No matching cert, using default`);
		return certificates['a.example.com']; // Default
	};

	try {
		using tlsServer = TLSServer.listen(certificateResolver, 8443);
		// This loop will block this fiber forever (or until server.close() or error)
		for (const socket of tlsServer) {
			console.log("TLS connection accepted!");
			// Handle each connection concurrently
			Fiber.run(() => {
				try {
					using connection = socket;
					for (const chunk of connection) {
						console.log(`TLS Received: ${chunk.toString().trim()}`);
						connection.write("TLS> ".toBytes());
						connection.write(chunk);
					}
				} catch (fault) {
					console.log(`TLS socket error: ${fault.message}`);
				}
			});
		}
	} catch (fault) {
		console.log(`TLS Server Error: ${fault.message}\n${fault.stack}`);
	}
};
let tlsClient = () => {
	try {
		using socket = TLSSocket.connect('127.0.0.1', 8443);
		socket.write('Hi!'.toBytes());
		for (let chunk of socket) {
			console.log(chunk);
		}
	} catch (fault) {
		console.log(`TLS Client Error: ${fault.message}`);
	}
};

// Fiber.run(fsExample);
// Fiber.run(dnsExample);
// Fiber.run(udpServer);
// Fiber.run(udpClient);
// Fiber.run(tcpServer);
Fiber.run(tcpClient);
// Fiber.run(tlsServer);
// Fiber.run(tlsClient);