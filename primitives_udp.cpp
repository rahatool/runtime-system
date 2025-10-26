#include "primitives.h"
#include "handles.h"
#include <string>
#include <queue>

// --- Handle Implementation ---
void UDPHandle::Close() {
	if (!uv_is_closing((uv_handle_t*)&handle)) {
		uv_close((uv_handle_t*)&handle, [](uv_handle_t* h){
			// Destructor via shared_ptr
		});
	}
}
UDPHandle::~UDPHandle() {
	delete (uv_udp_t*)&handle;
}

// --- Async Contexts ---
struct SendContext : public AsyncContext {
	uv_udp_send_t req;
	uv_buf_t buf;
	char* data;
	SendContext(Fiber* f, const char* d, size_t len) : AsyncContext(f) {
		req.data = this;
		data = new char[len];
		memcpy(data, d, len);
		buf = uv_buf_init(data, len);
	}
	~SendContext() { delete[] data; }
};
struct RecvContext : public AsyncContext {
	uv_buf_t buf;
	sockaddr_storage remote_addr;
	RecvContext(Fiber* f) : AsyncContext(f) {
		buf.base = new char[65536]; // Default 64k buffer
		buf.len = 65536;
	}
	~RecvContext() { delete[] buf.base; }
};

// --- Callbacks ---
void OnAllocUDP(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
	UDPHandle* wrap = static_cast<UDPHandle*>(handle->data);
	RecvContext* context = static_cast<RecvContext*>(wrap->pending_read);
	*buf = context->buf;
}
void OnRecvUDP(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags) {
	UDPHandle* wrap = static_cast<UDPHandle*>(handle->data);
	RecvContext* context = static_cast<RecvContext*>(wrap->pending_read);
	if (context == nullptr) return;
	
	uv_udp_recv_stop(handle);
	wrap->pending_read = nullptr;

	if (nread < 0) {
		context->ResumeError(nread, "read");
	} else if (nread >= 0) {
		if (addr) {
			memcpy(&context->remote_addr, addr, sizeof(sockaddr_storage));
		}
		context->Resume(v8::BigInt::New(context->fiber->isolate(), nread));
	}
	// Do NOT delete context here
}
void OnSendUDP(uv_udp_send_t* req, int status) {
	SendContext* context = static_cast<SendContext*>(req->data);
	if (status < 0) context->ResumeError(status, "write");
	else context->Resume(v8::Undefined(context->fiber->isolate()));
	delete context;
}

// --- JS Primitives ---
void UDP_Create(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	auto wrap = std::make_shared<UDPHandle>();
	int r = uv_udp_init(Fiber::get_loop(), &wrap->handle);
	if (r < 0) { ThrowUVException(isolate, r, "udp_init"); return; }
	uint64_t id = HandleStore::Add(wrap);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}
void UDP_Listen(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::String::Utf8Value host_str(isolate, args[1]);
	int port = args[2]->Int32Value(isolate->GetCurrentContext()).ToChecked();
	
	auto wrap = HandleStore::Get<UDPHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid UDP handle"); return; }
	
	sockaddr_in addr;
	uv_ip4_addr(*host_str, port, &addr);
	int r = uv_udp_bind(&wrap->handle, (const sockaddr*)&addr, 0);
	if (r) { ThrowUVException(isolate, r, "bind"); return; }
	
	args.GetReturnValue().Set(v8::Undefined(isolate));
}
void UDP_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();
	
	auto wrap = HandleStore::Get<UDPHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid UDP handle"); return; }
	
	RecvContext* context = new RecvContext(Fiber::get_current());
	wrap->pending_read = context;

	uv_udp_recv_start(&wrap->handle, OnAllocUDP, OnRecvUDP);
	Fiber::yield();
	
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) {
		delete context;
		isolate->ThrowException(result);
		return;
	}
	
	int64_t bytes_read = result.As<v8::BigInt>()->Int64Value();
	char* js_buf = GetUint8ArrayBufferData(buffer);
	size_t js_buf_len = GetUint8ArrayByteLength(buffer);
	
	size_t to_copy = std::min((size_t)bytes_read, js_buf_len);
	memcpy(js_buf, context->buf.base, to_copy);
	
	v8::Local<v8::Object> ret_obj = v8::Object::New(isolate);
	ret_obj->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, "bytes").ToLocalChecked(), v8::BigInt::New(isolate, (int64_t)to_copy)).Check();

	char host_str[17];
	int port;
	uv_ip4_name((struct sockaddr_in*)&context->remote_addr, host_str, 16);
	port = ntohs(((struct sockaddr_in*)&context->remote_addr)->sin_port);

	// Return host/port directly on the object
	ret_obj->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, "host").ToLocalChecked(), v8::String::NewFromUtf8(isolate, host_str).ToLocalChecked()).Check();
	ret_obj->Set(isolate->GetCurrentContext(), v8::String::NewFromUtf8(isolate, "port").ToLocalChecked(), v8::Integer::New(isolate, port)).Check();
	
	delete context;
	args.GetReturnValue().Set(ret_obj);
}
void UDP_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer = args[1].As<v8::Uint8Array>();
	v8::String::Utf8Value host_str(isolate, args[2]);
	int port = args[3]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	auto wrap = HandleStore::Get<UDPHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid UDP handle"); return; }
	
	char* data = GetUint8ArrayBufferData(buffer);
	size_t len = GetUint8ArrayByteLength(buffer);

	SendContext* context = new SendContext(Fiber::get_current(), data, len);
	
	sockaddr_in addr;
	uv_ip4_addr(*host_str, port, &addr);
	
	int r = uv_udp_send(&context->req, &wrap->handle, &context->buf, 1, (const sockaddr*)&addr, OnSendUDP);
	if (r < 0) { delete context; ThrowUVException(isolate, r, "write"); return; }
	
	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(v8::Undefined(isolate));
}
void UDP_Close(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	HandleStore::Remove(id);
}
void InitializeUDP(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> udp_obj = v8::Object::New(isolate);
	SET_METHOD(udp_obj, "create", UDP_Create);
	SET_METHOD(udp_obj, "listen", UDP_Listen); // Renamed bind
	SET_METHOD(udp_obj, "read", UDP_Read); // Renamed recv
	SET_METHOD(udp_obj, "write", UDP_Write); // Renamed send
	SET_METHOD(udp_obj, "close", UDP_Close);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "udp").ToLocalChecked(), udp_obj).Check(); // Renamed dgram
}

