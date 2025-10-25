#include "primitives.h"
#include "handles.h"
#include <string>
#include <vector>

struct DNSContext : public AsyncContext {
	uv_dns_query_t req;
	std::string hostname;
	uv_dns_query_result_t* result = nullptr;
	
	DNSContext(Fiber* f, const char* host) : AsyncContext(f), hostname(host) {
		req.data = this;
	}
	~DNSContext() {
		if (result) {
			uv_dns_free_result(result);
		}
	}
};

void OnDNSCallback(uv_dns_query_t* req, int status, uv_dns_query_result_t* res) {
	DNSContext* context = static_cast<DNSContext*>(req->data);
	context->result = res; // Store for parsing
	
	if (status < 0) {
		context->ResumeError(status, "dns_query", context->hostname.c_str());
	} else {
		v8::Isolate* isolate = context->fiber->isolate();
		v8::HandleScope handle_scope(isolate);
		v8::Local<v8::Context> v8_context = isolate->GetCurrentContext();
		
		v8::Local<v8::Object> result_obj = v8::Object::New(isolate);
		
		// A records
		v8::Local<v8::Array> a_records = v8::Array::New(isolate);
		char ip[17];
		int a_idx = 0;
		for (int i = 0; i < res->addr4_count; ++i) {
			uv_ip4_name(&res->addr4[i], ip, sizeof(ip));
			a_records->Set(v8_context, a_idx++, v8::String::NewFromUtf8(isolate, ip).ToLocalChecked()).Check();
		}
		result_obj->Set(v8_context, v8::String::NewFromUtf8(isolate, "A").ToLocalChecked(), a_records).Check();

		// AAAA records
		v8::Local<v8::Array> aaaa_records = v8::Array::New(isolate);
		char ip6[40];
		int aaaa_idx = 0;
		for (int i = 0; i < res->addr6_count; ++i) {
			uv_ip6_name(&res->addr6[i], ip6, sizeof(ip6));
			aaaa_records->Set(v8_context, aaaa_idx++, v8::String::NewFromUtf8(isolate, ip6).ToLocalChecked()).Check();
		}
		result_obj->Set(v8_context, v8::String::NewFromUtf8(isolate, "AAAA").ToLocalChecked(), aaaa_records).Check();

		// MX records
		v8::Local<v8::Array> mx_records = v8::Array::New(isolate);
		int mx_idx = 0;
		for (int i = 0; i < res->mx_count; ++i) {
			v8::Local<v8::Object> mx = v8::Object::New(isolate);
			mx->Set(v8_context, v8::String::NewFromUtf8(isolate, "priority").ToLocalChecked(), v8::Integer::New(isolate, res->mx[i].priority)).Check();
			mx->Set(v8_context, v8::String::NewFromUtf8(isolate, "exchange").ToLocalChecked(), v8::String::NewFromUtf8(isolate, res->mx[i].exchange).ToLocalChecked()).Check();
			mx_records->Set(v8_context, mx_idx++, mx).Check();
		}
		result_obj->Set(v8_context, v8::String::NewFromUtf8(isolate, "MX").ToLocalChecked(), mx_records).Check();

		// TXT records
		v8::Local<v8::Array> txt_records = v8::Array::New(isolate);
		int txt_idx = 0;
		for (int i = 0; i < res->txt_count; ++i) {
			txt_records->Set(v8_context, txt_idx++, v8::String::NewFromUtf8(isolate, res->txt[i].str).ToLocalChecked()).Check();
		}
		result_obj->Set(v8_context, v8::String::NewFromUtf8(isolate, "TXT").ToLocalChecked(), txt_records).Check();
		
		// SRV, CNAME, NS, PTR records...
		
		context->Resume(result_obj);
	}
	
	delete context;
}

void DNS_Resolve(const v8::FunctionCallbackInfo<v8::Value>& args) {
	v8::Isolate* isolate = args.GetIsolate();
	v8::String::Utf8Value hostname(isolate, args[0]);
	v8::String::Utf8Value rrtype_str(isolate, args[1]);

	uv_dns_query_type_t type;
	std::string s_type(*rrtype_str);
	if (s_type == "A") type = UV_DNS_A;
	else if (s_type == "AAAA") type = UV_DNS_AAAA;
	else if (s_type == "MX") type = UV_DNS_MX;
	else if (s_type == "TXT") type = UV_DNS_TXT;
	else if (s_type == "SRV") type = UV_DNS_SRV;
	else if (s_type == "CNAME") type = UV_DNS_CNAME;
	else if (s_type == "NS") type = UV_DNS_NS;
	else if (s_type == "PTR") type = UV_DNS_PTR;
	else { Throw(isolate, "Invalid DNS record type"); return; }

	DNSContext* context = new DNSContext(Fiber::get_current(), *hostname);
	int r = uv_dns_query(Fiber::get_loop(), &context->req, OnDNSCallback, *hostname, type);
	if (r) { delete context; ThrowUVException(isolate, r, "dns_query", *hostname); return; }

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

