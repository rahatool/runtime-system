#include "primitives.h"
#include "handles.h"
#include <string>

// --- Handle Implementation ---
void UDPHandle::Close() {
	uv_close((uv_handle_t*)&handle, [](uv_handle_t* h){
		// UDPHandle shared_ptr manages deletion
	});
}

// --- Async Context ---
struct UDPReadContext : public AsyncContext { // Renamed from RecvContext
	uv_buf_t buf; // Points into the JS buffer
	sockaddr_storage remote_addr;
	UDPReadContext(Fiber* f, char* d, size_t len) : AsyncContext(f) {
		buf = uv_buf_init(d, len);
	}
};
struct UDPWriteContext : public AsyncContext { // Renamed from SendContext
	uv_udp_send_t req;
	uv_buf_t buf;
	char* data; // Copied data
	UDPWriteContext(Fiber* f, char* d, size_t len) : AsyncContext(f) {
		req.data = this;
		data = new char[len];
		memcpy(data, d, len);
		buf = uv_buf_init(data, len);
	}
	~UDPWriteContext() { delete[] data; }
};

// --- Callbacks ---
void OnUDPAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
	UDPHandle* wrap = static_cast<UDPHandle*>(handle->data);
	UDPReadContext* context = static_cast<UDPReadContext*>(wrap->pending_read);
	if (!context || !context->buf.base || context->buf.len == 0) {
		static char dummy_buf[1];
		*buf = uv_buf_init(dummy_buf, 0);
		return;
	}
	*buf = context->buf; // Use JS buffer
}
// Renamed callback
void OnUDPRead(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags) {
	UDPHandle* wrap = static_cast<UDPHandle*>(handle->data);
	UDPReadContext* context = static_cast<UDPReadContext*>(wrap->pending_read);
	if (context == nullptr) return; // Read stopped

	uv_udp_recv_stop(handle); // Stop receiving until next read call
	wrap->pending_read = nullptr;

	if (nread < 0) {
		context->ResumeError(nread, "read"); // Use "read" for syscall name
	} else if (nread == 0 && addr == NULL) {
		// No data, ignore (uv_udp_recv_cb documentation)
		 context->Resume(v8::Null(context->fiber->isolate())); // Or indicate no data? Null seems better.
	} else {
		memcpy(&context->remote_addr, addr, sizeof(sockaddr_storage));
		context->Resume(v8::BigInt::New(context->fiber->isolate(), nread));
	}
	delete context;
}
// Renamed callback
void OnUDPWrite(uv_udp_send_t* req, int status) {
	UDPWriteContext* context = static_cast<UDPWriteContext*>(req->data);
	if (status < 0) {
		context->ResumeError(status, "write"); // Use "write"
	} else {
		context->Resume(v8::Undefined(context->fiber->isolate()));
	}
	delete context;
}

// --- JS Primitives ---
void UDP_CreateSocket(const v8::FunctionCallbackInfo<v8::Value>& args) { // Renamed
	auto wrap = std::make_shared<UDPHandle>();
	uv_udp_init(Fiber::get_loop(), &wrap->handle);
	uint64_t id = HandleStore::Add(wrap);
	args.GetReturnValue().Set(v8::BigInt::New(args.GetIsolate(), id));
}
// Renamed primitive
void UDP_Listen(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::String::Utf8Value host_str(isolate, args[1]);
	int port = args[2]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	auto wrap = HandleStore::Get<UDPHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid UDP socket handle"); return; }

	sockaddr_in addr;
	uv_ip4_addr(*host_str, port, &addr);
	// Use UV_UDP_REUSEADDR? Maybe add as option.
	int r = uv_udp_bind(&wrap->handle, (const sockaddr*)&addr, 0);
	if (r) { ThrowUVException(isolate, r, "listen"); return; } // Use "listen"

	// Start receiving immediately after successful bind/listen
	r = uv_udp_recv_start(&wrap->handle, OnUDPAlloc, OnUDPRead);
	if (r) { ThrowUVException(isolate, r, "recv_start"); return; }
}
// Renamed primitive
void UDP_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();
	v8::String::Utf8Value host_str(isolate, args[2]);
	int port = args[3].As<v8::Int32>()->Value();

	auto wrap = HandleStore::Get<UDPHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid UDP socket handle"); return; }

	char* data_ptr;
	size_t length;
	if (!GetUint8ArrayData(buffer_view, &data_ptr, &length)) return;

	UDPWriteContext* context = new UDPWriteContext(Fiber::get_current(), data_ptr, length);

	sockaddr_in addr;
	uv_ip4_addr(*host_str, port, &addr);

	int r = uv_udp_send(&context->req, &wrap->handle, &context->buf, 1, (const sockaddr*)&addr, OnUDPWrite);
	if (r) { delete context; ThrowUVException(isolate, r, "write"); return; } // Use "write"

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
}
// Renamed primitive
void UDP_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();

	auto wrap = HandleStore::Get<UDPHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid UDP socket handle"); return; }
	if (wrap->pending_read) { Throw(isolate, "Concurrent read on UDP socket not allowed"); return; }

	char* data_ptr;
	size_t length;
	 if (!GetUint8ArrayData(buffer_view, &data_ptr, &length)) return;

	UDPReadContext* context = new UDPReadContext(Fiber::get_current(), data_ptr, length);
	wrap->pending_read = context;

	// Reading should have already been started by Listen/Bind
	// If not, maybe start it here? Let's assume Listen starts it.
	// int r = uv_udp_recv_start(&wrap->handle, OnUDPAlloc, OnUDPRead);
	// if (r) { delete context; ThrowUVException(isolate, r, "recv_start"); return; }

	Fiber::yield(); // Yield until OnUDPRead resumes

	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	if(result->IsNull()) { args.GetReturnValue().Set(v8::Null(isolate)); return; } // No data received

	// Got data, create result object { bytes, remote: { host, port } }
	v8::Local<v8::Object> res_obj = v8::Object::New(isolate);
	res_obj->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, "bytes").ToLocalChecked(), result).Check();

	v8::Local<v8::Object> remote_obj = v8::Object::New(isolate);
	char host[17];
	int port;
	sockaddr_in* addr4 = (sockaddr_in*)&context->remote_addr;
	uv_ip4_name(addr4, host, 16);
	port = ntohs(addr4->sin_port);

	remote_obj->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, "host").ToLocalChecked(), v8::String::NewFromUtf8(isolate, host).ToLocalChecked()).Check();
	remote_obj->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, "port").ToLocalChecked(), v8::Integer::New(isolate, port)).Check();
	res_obj->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, "remote").ToLocalChecked(), remote_obj).Check();

	args.GetReturnValue().Set(res_obj);
}
void UDP_Close(const v8::FunctionCallbackInfo<v8::Value>& args) { // Renamed
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto wrap = HandleStore::Get<UDPHandle>(id);
	if (!wrap) { Throw(args.GetIsolate(), "Invalid UDP socket handle"); return; }
	wrap->Close();
	// Removal happens via FinalizationRegistry
}

// Renamed Initialization function
void InitializeUDP(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> udp_obj = v8::Object::New(isolate); // Renamed
	SET_METHOD(udp_obj, "create", UDP_CreateSocket);
	SET_METHOD(udp_obj, "listen", UDP_Listen); // Renamed
	SET_METHOD(udp_obj, "write", UDP_Write);   // Renamed
	SET_METHOD(udp_obj, "read", UDP_Read);     // Renamed
	SET_METHOD(udp_obj, "close", UDP_Close);
	// Export under 'udp' key
	exports->Set(context, v8::String::NewFromUtf8(isolate, "udp").ToLocalChecked(), udp_obj).Check();
}
