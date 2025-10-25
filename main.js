// openssl req -x509 -newkey rsa:2048 -nodes -keyout a.key -out a.crt -days 365 -subj "/CN=a.example.com"
// openssl req -x509 -newkey rsa:2048 -nodes -keyout b.key -out b.crt -days 365 -subj "/CN=b.example.com"
//
// openssl s_client -connect localhost:8443 -crlf -servername a.example.com
// openssl s_client -connect localhost:8443 -crlf -servername b.example.com
// nc -u localhost 5353

import { Fiber, fs, dns, udp, tcp, tls, FileHandle, DirectoryHandle, KeyHandle, CertHandle, TCPServer, TLSServer, UDPSocket } from './stdlib.js';

console.log("--- Advanced Runtime v1.2 Starting ---");

// --- Pre-load and PARSE certs once ---
const certificates = {
	'a.example.com': {
		key: new KeyHandle(fs.readFile('./a.key')),
		cert: new CertHandle(fs.readFile('./a.crt')),
	},
	'b.example.com': {
		key: new KeyHandle(fs.readFile('./b.key')),
		cert: new CertHandle(fs.readFile('./b.crt')),
	}
};
console.log("Pre-parsed certificates for: a.example.com, b.example.com");

// --- Dynamic certificate resolver ---
function certificateResolver(servername) {
	console.log(`SNI: Client requested server: ${servername}`);
	const certs = certificates[servername];
	if (certs) {
		console.log(`Found matching cert for ${servername}`);
		return certs; // Return { key: KeyHandle, cert: CertHandle }
	}
	console.log(`No matching cert, using default 'a.example.com'`);
	return certificates['a.example.com']; // Default
}

// --- UDP Echo Server ---
Fiber.run(async () => {
	try {
		const socket = UDPSocket.listen(5353); // Use factory
		console.log("UDP Echo server listening on 0.0.0.0:5353");
		// Use async iterator
		for await (const { data, remote } of socket) {
			 console.log(`UDP: Received ${data.byteLength} from ${remote.host}:${remote.port}`);
			 socket.write(data, remote.port, remote.host);
		}
	} catch(e) {
		console.log(`UDP Server Error: ${e.stack}`);
	}
});

// --- Main Server Logic ---
async function main() {
	try {
		console.log("Resolving 'google.com' A records...");
		const result = dns.resolve('google.com', 'A');
		console.log("Google A:", result.A[0]);
	} catch (e) {
		console.log(`DNS lookup failed: ${e.message}`);
	}

	console.log("Sleeping for 500ms...");
	Fiber.sleep(500);
	console.log("Awake!");

	// Test FileHandle & DirectoryHandle
	try {
		await DirectoryHandle.make("temp_dir");
		const dir = DirectoryHandle.open("temp_dir");
		let entries = [];
		for await(const entry of dir) entries.push(entry.name);
		console.log("temp_dir entries (should be empty):", entries);
		dir.close();
		await DirectoryHandle.remove("temp_dir");

		const file = FileHandle.open('test.txt', 'w+');
		let bytesWritten = file.write("Hello!");
		bytesWritten += file.write(" World!", 0, undefined, bytesWritten); // Append
		console.log(`Wrote ${bytesWritten} bytes to test.txt`);

		const stats = file.status();
		console.log(`test.txt size: ${stats.size}`);

		const readBuffer = new Uint8Array(100);
		const bytesRead = file.read(readBuffer, 0, 100, 0);
		console.log(`Read back: ${new TextDecoder().decode(readBuffer.subarray(0, bytesRead))}`);

		file.close();
		await FileHandle.unlink('test.txt');

	} catch(e) {
		console.log(`File/Dir test failed: ${e.stack}`);
	}


	// --- TLS Echo Server ---
	try {
		// Use factory method and async iterator
		const server = TLSServer.listen(certificateResolver, 8443);

		for await (const socket of server) {
			console.log("TLS connection accepted!");
			// Handle connection in a new fiber
			Fiber.run(async () => {
				 try {
					 const buffer = new Uint8Array(16384); // Reusable buffer per connection
					 // Use socket async iterator
					 for await (const chunk of socket) {
						 console.log(`TLS: Received ${chunk.byteLength} bytes.`);
						 socket.write(textEncoder.encode("[ECHO]: "));
						 socket.write(chunk); // Echo back the received chunk
					 }
					 console.log("Client disconnected (EOF).");
				 } catch(e) {
					 console.log(`Socket error: ${e.message}`);
				 } finally {
					 console.log("Closing TLS connection.");
					 socket.close();
				 }
			});
		}
		console.log("TLS Server stopped iterating."); // Should not happen unless server closed

	} catch (e) {
		console.log(`Server Error: ${e.message}\n${e.stack}`);
	}
}

Fiber.run(main);

