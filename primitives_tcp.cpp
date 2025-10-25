#include "primitives.h"
#include "handles.h"
#include <string>
#include <queue>

// --- Handle Implementation ---
void NetHandle::Close() {
	// Ensure read is stopped if pending
	if (pending_read) {
		uv_read_stop((uv_stream_t*)&handle);
		// Resume with an error? Or let uv_close handle it?
		// For simplicity, let uv_close handle cleanup.
		// pending_read->ResumeError("Socket closed during read");
		// delete pending_read; // Or let OnClose handle it? Risky.
		pending_read = nullptr;
	}
	uv_close((uv_handle_t*)&handle, [](uv_handle_t* h){
		// NetHandle shared_ptr manages deletion
	});
}

// --- Contexts for Async Ops ---
struct ConnectContext : public AsyncContext {
	uv_connect_t req;
	ConnectContext(Fiber* f) : AsyncContext(f) { req.data = this; }
};
struct WriteContext : public AsyncContext {
	uv_write_t req;
	uv_buf_t buf; // Points to data copied below
	char* data;   // Holds copied data for async write duration
	WriteContext(Fiber* f, char* d, size_t len) : AsyncContext(f) {
		req.data = this;
		data = new char[len]; // Copy data
		memcpy(data, d, len);
		buf = uv_buf_init(data, len);
	}
	~WriteContext() { delete[] data; } // Free copied data
};
struct ReadContext : public AsyncContext {
	uv_buf_t buf; // Points directly into the JS buffer view
	ReadContext(Fiber* f, char* d, size_t len) : AsyncContext(f) {
		buf = uv_buf_init(d, len);
	}
};
struct PollContext : public AsyncContext {
	uv_poll_t poll_handle;
	PollContext(Fiber* f) : AsyncContext(f) { poll_handle.data = this; }
	 ~PollContext() {
		// Ensure poll handle is closed if context is destroyed prematurely
		// uv_close((uv_handle_t*)&poll_handle, [](uv_handle_t*){}); // Risky if already closing
	}
};

// --- Callbacks ---
void OnTCPConnection(uv_stream_t* server_handle, int status) {
	NetHandle* wrap = static_cast<NetHandle*>(server_handle->data);
	if (wrap->accept_queue.empty()) return;
	Fiber* fiber = wrap->accept_queue.front();
	wrap->accept_queue.pop();
	v8::Isolate* isolate = fiber->isolate();
	v8::HandleScope handle_scope(isolate);

	if (status < 0) {
		wrap->ResumeError(status, "accept");
	} else {
		auto client_handle = std::make_shared<NetHandle>();
		uv_tcp_init(Fiber::get_loop(), &client_handle->handle);
		if (uv_accept(server_handle, (uv_stream_t*)&client_handle->handle) == 0) {
			uint64_t id = HandleStore::Add(client_handle);
			wrap->Resume(v8::BigInt::New(isolate, id));
		} else {
			uv_close((uv_handle_t*)&client_handle->handle, [](uv_handle_t* h){}); // Close immediately on accept error
			wrap->ResumeError(uv_last_error(Fiber::get_loop()).code, "accept");
		}
	}
}
void OnTCPConnect(uv_connect_t* req, int status) {
	ConnectContext* context = static_cast<ConnectContext*>(req->data);
	if (status < 0) context->ResumeError(status, "connect");
	else context->Resume(v8::Undefined(context->fiber->isolate()));
	delete context;
}
void OnTCPWrite(uv_write_t* req, int status) {
	WriteContext* context = static_cast<WriteContext*>(req->data);
	if (status < 0) context->ResumeError(status, "write");
	else context->Resume(v8::BigInt::New(context->fiber->isolate(), context->buf.len));
	delete context;
}
void OnTCPAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
	NetHandle* wrap = static_cast<NetHandle*>(handle->data);
	ReadContext* context = static_cast<ReadContext*>(wrap->pending_read);
	// If no read pending or buffer invalid, provide dummy buffer
	if (!context || !context->buf.base || context->buf.len == 0) {
		 static char dummy_buf[1]; // Static to avoid allocation
		 *buf = uv_buf_init(dummy_buf, 0);
		 return;
	}
	*buf = context->buf; // Use the buffer provided from JS
}
void OnTCPRead(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
	NetHandle* wrap = static_cast<NetHandle*>(stream->data);
	ReadContext* context = static_cast<ReadContext*>(wrap->pending_read);
	if (context == nullptr) return; // Read stopped prematurely

	uv_read_stop(stream);
	wrap->pending_read = nullptr; // Clear pending read

	if (nread < 0) {
		if (nread != UV_EOF) context->ResumeError(nread, "read");
		else context->Resume(v8::Null(context->fiber->isolate())); // EOF
	} else { // nread >= 0
		context->Resume(v8::BigInt::New(context->fiber->isolate(), nread)); // Return bytes read (can be 0)
	}
	delete context;
}
void OnPoll(uv_poll_t* handle, int status, int events) {
	PollContext* context = static_cast<PollContext*>(handle->data);
	uv_poll_stop(handle);
	// Close the poll handle itself
	uv_close((uv_handle_t*)handle, [](uv_handle_t* h){
		 // PollContext is deleted below after resuming
	});

	if (status < 0) context->ResumeError(status, "poll");
	else context->Resume(v8::Integer::New(context->fiber->isolate(), events));
	delete context; // Delete after resuming
}


// --- JS Primitives ---
void TCP_Listen(const v8::FunctionCallbackInfo<v8::Value>& args) { // Renamed
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value host_str(isolate, args[0]);
	int port = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	auto wrap = std::make_shared<NetHandle>();
	uv_tcp_init(Fiber::get_loop(), &wrap->handle);

	sockaddr_in addr;
	uv_ip4_addr(*host_str, port, &addr);
	int r = uv_tcp_bind(&wrap->handle, (const sockaddr*)&addr, 0);
	if (r) { ThrowUVException(isolate, r, "bind"); return; }

	r = uv_listen((uv_stream_t*)&wrap->handle, 128, OnTCPConnection);
	if (r) { ThrowUVException(isolate, r, "listen"); return; }

	uint64_t id = HandleStore::Add(wrap);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}
void TCP_Accept(const v8::FunctionCallbackInfo<v8::Value>& args) { // Renamed
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(args.GetIsolate(), "Invalid server handle"); return; }
	wrap->accept_queue.push(Fiber::get_current());
	Fiber::yield();
	args.GetReturnValue().Set(Fiber::get_current()->resume_value.Get(args.GetIsolate()));
}
void TCP_Connect(const v8::FunctionCallbackInfo<v8::Value>& args) { // Renamed
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value host_str(isolate, args[0]);
	int port = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	auto wrap = std::make_shared<NetHandle>();
	uv_tcp_init(Fiber::get_loop(), &wrap->handle);

	sockaddr_in addr;
	uv_ip4_addr(*host_str, port, &addr);

	ConnectContext* context = new ConnectContext(Fiber::get_current());
	int r = uv_tcp_connect(&context->req, &wrap->handle, (const sockaddr*)&addr, OnTCPConnect);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "connect", *host_str); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }

	uint64_t id = HandleStore::Add(wrap);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}
// Simplified Read: Operates on the full Uint8Array view
void TCP_Read(const v8::FunctionCallbackInfo<v8::Value>& args) { // Renamed
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();

	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid socket handle"); return; }
	if (wrap->pending_read) { Throw(isolate, "Concurrent read on socket not allowed"); return; }

	char* data_ptr;
	size_t length;
	if (!GetUint8ArrayData(buffer_view, &data_ptr, &length)) return; // Error handled

	ReadContext* context = new ReadContext(Fiber::get_current(), data_ptr, length);
	wrap->pending_read = context;

	uv_read_start((uv_stream_t*)&wrap->handle, OnTCPAlloc, OnTCPRead);
	Fiber::yield();

	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result); // bytesRead (BigInt) or null
}
// Simplified Write: Operates on the full Uint8Array view
void TCP_Write(const v8::FunctionCallbackInfo<v8::Value>& args) { // Renamed
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();

	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid socket handle"); return; }

	char* data_ptr;
	size_t length;
	if (!GetUint8ArrayData(buffer_view, &data_ptr, &length)) return;

	WriteContext* context = new WriteContext(Fiber::get_current(), data_ptr, length);
	int r = uv_write(&context->req, (uv_stream_t*)&wrap->handle, &context->buf, 1, OnTCPWrite);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "write"); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result); // bytesWritten
}
void TCP_Close(const v8::FunctionCallbackInfo<v8::Value>& args) { // Renamed
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto wrap = HandleStore::Get<NetHandle>(id);
	if (!wrap) { Throw(args.GetIsolate(), "Invalid socket handle"); return; }
	wrap->Close(); // Initiate async close
	// Removal from HandleStore happens via FinalizationRegistry calling HANDLES_Free
}
// Renamed NET_Poll to TCP_Poll for clarity if needed, or keep generic? Keep generic.
void NET_Poll(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	int events = args[1]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	auto base_wrap = HandleStore::Get<BaseHandle>(id); // Get base handle
	if (!base_wrap) { Throw(isolate, "Invalid handle for poll"); return; }

	uv_handle_t* uv_handle_ptr = nullptr;
	// Determine the underlying uv_handle_t type
	if (auto net_handle = std::dynamic_pointer_cast<NetHandle>(base_wrap)) {
		uv_handle_ptr = (uv_handle_t*)&net_handle->handle;
	} else if (auto udp_handle = std::dynamic_pointer_cast<UDPHandle>(base_wrap)) {
		uv_handle_ptr = (uv_handle_t*)&udp_handle->handle;
	} // Add other handle types if they need polling

	if (!uv_handle_ptr) { Throw(isolate, "Handle type cannot be polled"); return; }

	uv_os_fd_t fd;
	if (uv_fileno(uv_handle_ptr, &fd) != 0) { ThrowUVException(isolate, uv_last_error(Fiber::get_loop()).code, "poll (fileno)"); return; }

	PollContext* context = new PollContext(Fiber::get_current());
	uv_poll_init_socket(Fiber::get_loop(), &context->poll_handle, fd);
	uv_poll_start(&context->poll_handle, events, OnPoll);

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

// Renamed Initialization function
void InitializeTCP(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> tcp_obj = v8::Object::New(isolate); // Renamed object
	SET_METHOD(tcp_obj, "listen", TCP_Listen);
	SET_METHOD(tcp_obj, "accept", TCP_Accept);
	SET_METHOD(tcp_obj, "connect", TCP_Connect);
	SET_METHOD(tcp_obj, "read", TCP_Read);
	SET_METHOD(tcp_obj, "write", TCP_Write);
	SET_METHOD(tcp_obj, "close", TCP_Close);
	SET_METHOD(tcp_obj, "poll", NET_Poll); // Keep poll generic? Or TCP_Poll? Keep generic for now.
	// Export under 'tcp' key
	exports->Set(context, v8::String::NewFromUtf8(isolate, "tcp").ToLocalChecked(), tcp_obj).Check();
}
