// Create self-signed certificates to run this demo:
// openssl req -x509 -newkey rsa:2048 -nodes -keyout a.key -out a.crt -days 365 -subj "/CN=a.example.com"
// openssl req -x509 -newkey rsa:2048 -nodes -keyout b.key -out b.crt -days 365 -subj "/CN=b.example.com"
//
// Then connect:
// openssl s_client -connect localhost:8443 -crlf -servername a.example.com
// openssl s_client -connect localhost:8443 -crlf -servername b.example.com

// Note: Use 'export { ... }' pattern now
import { Fiber, FileHandle, DirectoryHandle, dns, TCPServer, UDPSocket, TLSServer, KeyHandle, CertHandle, TextDecoder } from './stdlib.js';

console.log("--- Advanced Runtime Starting (Full DNS, FS, SNI) ---");

// --- Pre-load certs using new FileHandle API ---
function loadPem(path) {
	// Use static readAll method
	return FileHandle.readAll(path);
}

const certificates = {
	'a.example.com': {
		key: new KeyHandle(loadPem('./a.key')),
		cert: new CertHandle(loadPem('./a.crt')),
	},
	'b.example.com': {
		key: new KeyHandle(loadPem('./b.key')),
		cert: new CertHandle(loadPem('./b.crt')),
	}
};
console.log("Loaded certificates for: a.example.com, b.example.com");

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
function main() {
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
		const dir = DirectoryHandle.open(".");
		for(const entry of dir) {
			console.log(`  Found: ${entry.name} (type: ${entry.type})`);
		} // dir.close() is called implicitly by iterator finally
	} catch(e) {
		console.log(`fs.readdir failed: ${e.message}`);
	}

	// Run TCP server in a separate fiber
	Fiber.run(() => {
		try {
			const server = TCPServer.listen(8080);
			for (const socket of server) {
				console.log("TCP connection accepted!");
				Fiber.run(() => {
					try {
						for (const chunk of socket) {
							socket.write("TCP> ".bytes());
							socket.write(chunk);
						}
					} catch(e) {
						console.log(`TCP socket error: ${e.message}`);
					} finally {
						socket.close();
					}
				});
			}
		} catch(e) {
			 console.log(`TCP Server Error: ${e.message}`);
		}
	});
	
	// Run UDP server in a separate fiber
	Fiber.run(() => {
		try {
			const socket = UDPSocket.listen(8081);
			for(const dgram of socket) {
				console.log(`UDP datagram from ${dgram.host}:${dgram.port}`);
				socket.write("UDP> ".bytes(), dgram.port, dgram.host);
				socket.write(dgram.data, dgram.port, dgram.host);
			}
		} catch (e) {
			console.log(`UDP Socket Error: ${e.message}`);
		}
	});

	// Run TLS server in the main fiber
	try {
		const server = TLSServer.listen(certificateResolver, 8443);
		// This loop will block this fiber forever
		for (const socket of server) {
			console.log("TLS connection accepted!");
			// Handle each connection in its own new fiber
			Fiber.run(() => {
				try {
					// Echo loop
					for (const chunk of socket) {
						console.log(`TLS Received: ${new TextDecoder().decode(chunk).trim()}`);
						socket.write("TLS> ".bytes());
						socket.write(chunk);
					}
				} catch(e) {
					console.log(`TLS socket error: ${e.message}`);
				} finally {
					console.log("Closing TLS connection.");
					socket.close();
				}
			});
		}
	} catch (e) {
		console.log(`TLS Server Error: ${e.message}\n${e.stack}`);
		console.log("\nMake sure 'a.key'/'a.crt' and 'b.key'/'b.crt' exist.");
	}
}

// Run the main logic
Fiber.run(main);

