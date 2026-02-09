#include "primitives.h"
#include "handles.h"
#include <string>
#include <vector>
#include <cstring>

struct DNSContext : public AsyncContext {
	uv_getaddrinfo_t req;
	std::string hostname;
	addrinfo* addr_result = nullptr;
	std::vector<std::string> addresses; // Store string results here

	DNSContext(Fiber* f, const char* host) : AsyncContext(f), hostname(host) {
		req.data = this;
	}
	~DNSContext() {
		if (addr_result) {
			uv_freeaddrinfo(addr_result);
		}
	}

	// This is called in the main loop to build the V8 array.
	v8::Local<v8::Value> CreateResultValue(v8::Isolate* isolate) override {
		if (result < 0) {
			error_syscall = "getaddrinfo";
			error_path = hostname;
			return AsyncContext::CreateResultValue(isolate);
		}
		
		v8::Local<v8::Context> context = isolate->GetCurrentContext();
		v8::Local<v8::Array> arr = v8::Array::New(isolate, static_cast<int>(addresses.size()));
		for (uint32_t i = 0; i < addresses.size(); ++i) {
			arr->Set(context, i, v8::String::NewFromUtf8(isolate, addresses[i].c_str()).ToLocalChecked()).Check();
		}
		return arr;
	}
};

static void OnDNSCallback(uv_getaddrinfo_t* req, int status, addrinfo* res) {
	DNSContext* context = static_cast<DNSContext*>(req->data);
	context->result = status;
	context->addr_result = res; // Store for freeing in destructor

	if (status >= 0) {
		// If successful, process the linked list into a vector of strings.
		// No V8 objects are created here.
		for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
			char buf[INET6_ADDRSTRLEN] = {0};
			if (ai->ai_family == AF_INET) {
				uv_ip4_name(reinterpret_cast<const sockaddr_in*>(ai->ai_addr), buf, sizeof(buf));
				context->addresses.emplace_back(buf);
			} else if (ai->ai_family == AF_INET6) {
				uv_ip6_name(reinterpret_cast<const sockaddr_in6*>(ai->ai_addr), buf, sizeof(buf));
				context->addresses.emplace_back(buf);
			}
		}
	}
	
	QueueFiberToResume(context);
}

void DNS_Resolve(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value hostname(isolate, args[0]);
	v8::String::Utf8Value rrtype_str(isolate, args[1]);

	std::string type(*rrtype_str);
	int family = AF_UNSPEC;
	if (type == "A") family = AF_INET;
	else if (type == "AAAA") family = AF_INET6;
	else { Throw(isolate, "DNS record type not supported by libuv resolver"); return; }

	addrinfo hints{};
	hints.ai_family = family;
	hints.ai_socktype = SOCK_STREAM;

	DNSContext* context = new DNSContext(Fiber::get_current(), *hostname);
	int r = uv_getaddrinfo(Fiber::get_loop(), &context->req, OnDNSCallback, *hostname, nullptr, &hints);
	if (r) { delete context; ThrowUVException(isolate, r, "getaddrinfo", *hostname); return; }

	Fiber::yield();
	v8::Local<v8::Value> result = Fiber::get_current()->resume_value.Get(isolate);
	if(result->IsNativeError()) { isolate->ThrowException(result); return; }
	args.GetReturnValue().Set(result);
}

void InitializeDNS(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> dns_obj = v8::Object::New(isolate);
	SET_METHOD(dns_obj, "resolve", DNS_Resolve);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "dns").ToLocalChecked(), dns_obj).Check();
}