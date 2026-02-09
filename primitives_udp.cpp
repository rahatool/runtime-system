#include "primitives.h"
#include "handles.h"
#include <string>
#include <queue>

// --- Handle Implementation ---
void UDPHandle::Close() {
	if (!uv_is_closing((uv_handle_t*)&handle)) {
		uv_close((uv_handle_t*)&handle, [](uv_handle_t* h){});
	}
}

// --- Contexts for Async Ops ---
struct RecvContext : public AsyncContext {
	uv_buf_t iov;
	v8::Persistent<v8::Uint8Array> user_buffer;
	std::string host;
	int port;

	RecvContext(Fiber* f, v8::Isolate* isolate, v8::Local<v8::Uint8Array> user_buf) : AsyncContext(f), port(0) {
		user_buffer.Reset(isolate, user_buf);
		size_t len;
		char* data = GetUint8ArrayBufferData(user_buf, &len);
		iov = uv_buf_init(data, len);
	}
	~RecvContext() { user_buffer.Reset(); }

	v8::Local<v8::Value> CreateResultValue(v8::Isolate* isolate) override {
		if (result < 0) {
			error_syscall = "read";
			return AsyncContext::CreateResultValue(isolate);
		}
		if (result == 0 && host.empty()) {
			return v8::Null(isolate); // No data received
		}

		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Object> ret_obj = v8::Object::New(isolate);
		ret_obj->Set(context, v8::String::NewFromUtf8(isolate, "bytes").ToLocalChecked(), v8::BigInt::New(isolate, result)).Check();
		ret_obj->Set(context, v8::String::NewFromUtf8(isolate, "host").ToLocalChecked(), v8::String::NewFromUtf8(isolate, host.c_str()).ToLocalChecked()).Check();
		ret_obj->Set(context, v8::String::NewFromUtf8(isolate, "port").ToLocalChecked(), v8::Integer::New(isolate, port)).Check();
		return ret_obj;
	}
};

struct SendContext : public AsyncContext {
	uv_udp_send_t req;
	uv_buf_t buf;
	v8::Persistent<v8::Uint8Array> user_buffer;

	SendContext(Fiber* f, v8::Isolate* isolate, v8::Local<v8::Uint8Array> user_buf) : AsyncContext(f) {
		req.data = this;
		user_buffer.Reset(isolate, user_buf);
		size_t len;
		char* data = GetUint8ArrayBufferData(user_buf, &len);
		buf = uv_buf_init(const_cast<char*>(data), len);
	}
	~SendContext() { user_buffer.Reset(); }
	v8::Local<v8::Value> CreateResultValue(v8::Isolate* isolate) override {
		if (result < 0) {
			error_syscall = "write";
			return AsyncContext::CreateResultValue(isolate);
		}
		return v8::Undefined(isolate);
	}
};

// --- Callbacks ---
void OnRecvAlloc(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
	UDPHandle* wrap = static_cast<UDPHandle*>(handle->data);
	RecvContext* context = static_cast<RecvContext*>(wrap->pending_read);
	*buf = context->iov;
}

void OnRecv(uv_udp_t* handle, ssize_t nread, const uv_buf_t* buf, const struct sockaddr* addr, unsigned flags) {
	UDPHandle* wrap = static_cast<UDPHandle*>(handle->data);
	RecvContext* context = static_cast<RecvContext*>(wrap->pending_read);
	if (context == nullptr) return;

	uv_udp_recv_stop(handle);
	wrap->pending_read = nullptr;
	
	context->result = nread;

	if (nread >= 0 && addr != nullptr) {
		if (addr->sa_family == AF_INET) {
			char host_str[17];
			uv_ip4_name((const sockaddr_in*)addr, host_str, 17);
			context->host = host_str;
			context->port = ntohs(((const sockaddr_in*)addr)->sin_port);
		} else if (addr->sa_family == AF_INET6) {
			char host_str[40];
			uv_ip6_name((const sockaddr_in6*)addr, host_str, 40);
			context->host = host_str;
			context->port = ntohs(((const sockaddr_in6*)addr)->sin6_port);
		}
	}
	
	QueueFiberToResume(context);
}

void OnSend(uv_udp_send_t* req, int status) {
	SendContext* context = static_cast<SendContext*>(req->data);
	context->result = status;
	QueueFiberToResume(context);
}

// --- JS Primitives ---
void UDP_Create(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	auto wrap = std::make_shared<UDPHandle>();
	uv_udp_init(Fiber::get_loop(), &wrap->handle);
	uint64_t id = HandleStore::Add(wrap);
	args.GetReturnValue().Set(v8::BigInt::New(isolate, id));
}

void UDP_Listen(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::String::Utf8Value host_str(isolate, args[1]);
	int port = args[2]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	auto wrap = HandleStore::Get<UDPHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid UDP socket handle"); return; }

	sockaddr_in addr;
	uv_ip4_addr(*host_str, port, &addr);
	if (uv_udp_bind(&wrap->handle, (const sockaddr*)&addr, 0) != 0) { Throw(isolate, "Failed to bind UDP socket"); }
}

void UDP_Read(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();

	auto wrap = HandleStore::Get<UDPHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid UDP socket handle"); return; }
	if (wrap->pending_read) { Throw(isolate, "Read already pending on socket"); return; }

	RecvContext* recv_context = new RecvContext(Fiber::get_current(), isolate, buffer_view);
	wrap->pending_read = recv_context;

	uv_udp_recv_start(&wrap->handle, OnRecvAlloc, OnRecv);
	Fiber::yield();

	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

void UDP_Write(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	v8::Local<v8::Uint8Array> buffer_view = args[1].As<v8::Uint8Array>();
	v8::String::Utf8Value host_str(isolate, args[2]);
	int port = args[3]->Int32Value(isolate->GetCurrentContext()).ToChecked();

	auto wrap = HandleStore::Get<UDPHandle>(id);
	if (!wrap) { Throw(isolate, "Invalid UDP socket handle"); return; }

	SendContext* context = new SendContext(Fiber::get_current(), isolate, buffer_view);

	sockaddr_in dest_addr;
	uv_ip4_addr(*host_str, port, &dest_addr);

	uv_udp_send(&context->req, &wrap->handle, &context->buf, 1, (const sockaddr*)&dest_addr, OnSend);

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(v8::Undefined(isolate));
}

void UDP_Close(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	auto wrap = HandleStore::Get<UDPHandle>(id);
	if (!wrap) { Throw(args.GetIsolate(), "Invalid UDP socket handle"); return; }
	wrap->Close();
	HandleStore::Remove(id);
}

void InitializeUDP(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> udp_obj = v8::Object::New(isolate);
	SET_METHOD(udp_obj, "create", UDP_Create);
	SET_METHOD(udp_obj, "listen", UDP_Listen);
	SET_METHOD(udp_obj, "read", UDP_Read);
	SET_METHOD(udp_obj, "write", UDP_Write);
	SET_METHOD(udp_obj, "close", UDP_Close);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "udp").ToLocalChecked(), udp_obj).Check();
}