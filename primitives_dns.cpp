#include "primitives.h"
#include "handles.h"
#include <string>
#include <vector>
#include <cstring>

struct DNSContext : public AsyncContext {
	uv_getaddrinfo_t req;
	std::string hostname;
	std::string rrtype;
	addrinfo* result = nullptr;
	
	DNSContext(Fiber* f, const char* host, const char* type) : AsyncContext(f), hostname(host), rrtype(type) {
		req.data = this;
	}
	~DNSContext() {
		if (result) {
			uv_freeaddrinfo(result);
		}
	}
};

static void OnDNSCallback(uv_getaddrinfo_t* req, int status, addrinfo* res) {
	DNSContext* context = static_cast<DNSContext*>(req->data);
	context->result = res; // store to free in dtor

	if (status < 0) {
		context->ResumeError(status, "getaddrinfo", context->hostname.c_str());
		delete context;
		return;
	}

	v8::Isolate* isolate = context->fiber->isolate();
	v8::HandleScope handle_scope(isolate);
	v8::Local<v8::Context> v8_context = isolate->GetCurrentContext();

	// Collect addresses matching requested family
	std::vector<std::string> addrs;
	for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
		char buf[INET6_ADDRSTRLEN] = {0};
		if (ai->ai_family == AF_INET) {
			uv_ip4_name(reinterpret_cast<const sockaddr_in*>(ai->ai_addr), buf, sizeof(buf));
			addrs.emplace_back(buf);
		} else if (ai->ai_family == AF_INET6) {
			uv_ip6_name(reinterpret_cast<const sockaddr_in6*>(ai->ai_addr), buf, sizeof(buf));
			addrs.emplace_back(buf);
		}
	}

	v8::Local<v8::Array> arr = v8::Array::New(isolate, static_cast<int>(addrs.size()));
	for (uint32_t i = 0; i < addrs.size(); ++i) {
		arr->Set(v8_context, i, v8::String::NewFromUtf8(isolate, addrs[i].c_str()).ToLocalChecked()).Check();
	}
	context->Resume(arr);
	delete context;
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
	hints.ai_socktype = SOCK_STREAM; // any usable

	DNSContext* context = new DNSContext(Fiber::get_current(), *hostname, type.c_str());
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
