// Create self-signed certificates to run this demo:
// openssl req -x509 -newkey rsa:2048 -nodes -keyout a.key -out a.crt -days 365 -subj "/CN=a.example.com"
// openssl req -x509 -newkey rsa:2048 -nodes -keyout b.key -out b.crt -days 365 -subj "/CN=b.example.com"
//
// Then connect:
// openssl s_client -connect localhost:8443 -crlf -servername a.example.com
// openssl s_client -connect localhost:8443 -crlf -servername b.example.com

import { Fiber, FileHandle, DirectoryHandle, dns, TCPServer, UDPSocket, TLSServer, KeyHandle, CertHandle, TextDecoder } from './stdlib.js';

console.log("--- Advanced Runtime Starting (Full DNS, FS, SNI) ---");

// --- Pre-load certs using FileHandle.open().readAll() ---
// No 'using' needed here, as readAll opens, reads, and implicitly closes (via GC)
let certificates;
try {
	certificates = {
		'a.example.com': {
			key: new KeyHandle(FileHandle.open('./a.key').readAll()),
			cert: new CertHandle(FileHandle.open('./a.crt').readAll()),
		},
		'b.example.com': {
			key: new KeyHandle(FileHandle.open('./b.key').readAll()),
			cert: new CertHandle(FileHandle.open('./b.crt').readAll()),
		}
	};
	console.log("Loaded certificates for: a.example.com, b.example.com");
} catch (e) {
	console.log(`Failed to load certificates: ${e.message}`);
	console.log("Make sure 'a.key', 'a.crt', 'b.key', and 'b.crt' exist.");
	// Exit or handle appropriately
}


// --- This is the dynamic certificate resolver ---
function certificateResolver(servername) {
	console.log(`SNI: Client requested server: ${servername}`);
	const certs = certificates[servername];
	if (certs) {
		console.log(`Found matching cert for ${servername}`);
		return certs; // Return { key: KeyHandle, cert: CertHandle }
	}
	console.log(`No matching cert, using default`);
	return certificates['a.example.com']; // Default
}

// --- Main Server Logic ---
function main() { // No longer async
	try {
		console.log("Resolving 'google.com' MX records...");
		const mx_records = dns.resolve('google.com', 'MX');
		console.log("Google MX:", mx_records[0]);

		console.log("Resolving 'google.com' AAAA records...");
		const aaaa_records = dns.resolve('google.com', 'AAAA');
		console.log("Google AAAA:", aaaa_records[0]);
	} catch (e) {
		console.log(`DNS lookup failed: ${e.message}`);
	}

	try {
		console.log("Reading directory '.' ...");
		// Use `using` for automatic closing
		using dir = DirectoryHandle.open(".");
		for(const entry of dir) {
			console.log(`  Found: ${entry.name} (type: ${entry.type})`);
		}
		// dir.close() called automatically by `using`
	} catch(e) {
		console.log(`fs.readdir failed: ${e.message}`);
	}

	// Run TCP server in a separate fiber
	Fiber.run(() => { // No longer async
		try {
			using server = TCPServer.listen(8080); // Use `using`
			for (const socket of server) {
				console.log("TCP connection accepted!");
				Fiber.run(() => { // No longer async
					try {
						using conn = socket; // Use `using`
						for (const chunk of conn) {
							conn.write("TCP> ".bytes());
							conn.write(chunk);
						}
					} catch(e) {
						console.log(`TCP socket error: ${e.message}`);
					}
					// conn.close() called automatically
				});
			}
		} catch(e) {
			 console.log(`TCP Server Error: ${e.message}`);
		}
		// server.close() called automatically
	});

	// Run UDP server in a separate fiber
	Fiber.run(() => { // No longer async
		try {
			using socket = UDPSocket.listen(8081); // Use `using`
			for(const dgram of socket) {
				console.log(`UDP datagram from ${dgram.host}:${dgram.port}`);
				socket.write("UDP> ".bytes(), dgram.port, dgram.host);
				socket.write(dgram.data, dgram.port, dgram.host);
			}
		} catch (e) {
			console.log(`UDP Socket Error: ${e.message}`);
		}
		// socket.close() called automatically
	});

	// Run TLS server in the main fiber (or another one)
	try {
		if (!certificates) throw new Error("Certificates not loaded.");

		using tlsServer = TLSServer.listen(certificateResolver, 8443); // Use `using`
		// This loop will block this fiber forever (or until server.close() or error)
		for (const socket of tlsServer) {
			console.log("TLS connection accepted!");
			// Handle each connection concurrently
			Fiber.run(() => { // No longer async
				try {
					using conn = socket; // Use `using`
					for (const chunk of conn) {
						console.log(`TLS Received: ${new TextDecoder().decode(chunk).trim()}`);
						conn.write("TLS> ".bytes());
						conn.write(chunk);
					}
				} catch(e) {
					console.log(`TLS socket error: ${e.message}`);
				} finally {
					console.log("Closing TLS connection.");
					// conn.close() called automatically by `using`
				}
			});
		}
	} catch (e) {
		console.log(`TLS Server Error: ${e.message}\n${e.stack}`);
	}
	// tlsServer.close() called automatically by `using` only if the loop exits
	// In reality, this server loop runs forever unless manually stopped or an error occurs.
}

// Run the main logic
Fiber.run(main);
// No need for .catch as Fiber.run no longer returns a Promise

