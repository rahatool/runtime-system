// Create self-signed certificates to run this demo:
// openssl req -x509 -newkey rsa:2048 -nodes -keyout a.key -out a.crt -days 365 -subj "/CN=a.example.com"
// openssl req -x509 -newkey rsa:2048 -nodes -keyout b.key -out b.crt -days 365 -subj "/CN=b.example.com"
//
// Then connect:
// openssl s_client -connect localhost:8443 -crlf -servername a.example.com
// openssl s_client -connect localhost:8443 -crlf -servername b.example.com

import { tls, fs, dns } from './stdlib.js';

console.log("--- Advanced Runtime Starting (Full DNS, FS, SNI) ---");

// --- Pre-load certs into a map ---
const certificates = {
	'a.example.com': {
		key: fs.readFile('./a.key'),
		cert: fs.readFile('./a.crt'),
	},
	'b.example.com': {
		key: fs.readFile('./b.key'),
		cert: fs.readFile('./b.crt'),
	}
};
console.log("Loaded certificates for: a.example.com, b.example.com");

// --- This is the dynamic certificate resolver ---
function certificateResolver(servername) {
	console.log(`SNI: Client requested server: ${servername}`);
	const certs = certificates[servername];
	if (certs) {
		console.log(`Found matching cert for ${servername}`);
		return certs; // Return { key, cert }
	}
	console.log(`No matching cert, using default`);
	return certificates['a.example.com']; // Default
}

// --- Main Server Logic ---
async function main() {
	try {
		console.log("Resolving 'google.com' MX records...");
		const mx_records = dns.resolve('google.com', 'MX');
		console.log("Google MX:", mx_records.MX[0]);

		console.log("Resolving 'google.com' AAAA records...");
		const aaaa_records = dns.resolve('google.com', 'AAAA');
		console.log("Google AAAA:", aaaa_records.AAAA[0]);
	} catch (e) {
		console.log(`DNS lookup failed: ${e.message}`);
	}

	try {
		const options = {
			certificateResolver: certificateResolver
		};

		// Create a TLS Echo Server
		const server = tls.createServer(options, (socket) => {
			try {
				console.log("TLS connection accepted!");
				// Echo loop
				while (true) {
					const data = socket.read();
					if (data === null) {
						console.log("Client disconnected (EOF).");
						break;
					}
					console.log(`Received: ${data.trim()}`);
					socket.write(`[ECHO]: ${data}`);
				}
			} catch(e) {
				console.log(`Socket error: ${e.message}`);
			} finally {
				console.log("Closing TLS connection.");
				socket.close();
			}
		});
		
		server.listen(8443);

	} catch (e) {
		console.log(`Server Error: ${e.message}\n${e.stack}`);
	}
}

// Run the main logic in a fiber
Fiber.run(main);