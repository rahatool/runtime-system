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

v8::Local<v8::Value> ParseDNSRecord(v8::Isolate* isolate, const uv_dns_record_t* record) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> obj = v8::Object::New(isolate);
	obj->Set(context, v8::String::NewFromUtf8(isolate, "ttl").ToLocalChecked(), v8::Integer::New(isolate, record->ttl)).Check();
	
	switch (record->type) {
		case UV_DNS_A:
			char ip[17];
			uv_ip4_name(&record->addr4, ip, sizeof(ip));
			obj->Set(context, v8::String::NewFromUtf8(isolate, "address").ToLocalChecked(), v8::String::NewFromUtf8(isolate, ip).ToLocalChecked()).Check();
			break;
		case UV_DNS_AAAA:
			char ip6[40];
			uv_ip6_name(&record->addr6, ip6, sizeof(ip6));
			obj->Set(context, v8::String::NewFromUtf8(isolate, "address").ToLocalChecked(), v8::String::NewFromUtf8(isolate, ip6).ToLocalChecked()).Check();
			break;
		case UV_DNS_MX:
			obj->Set(context, v8::String::NewFromUtf8(isolate, "priority").ToLocalChecked(), v8::Integer::New(isolate, record->mx.priority)).Check();
			obj->Set(context, v8::String::NewFromUtf8(isolate, "exchange").ToLocalChecked(), v8::String::NewFromUtf8(isolate, record->mx.exchange).ToLocalChecked()).Check();
			break;
		case UV_DNS_TXT:
			obj->Set(context, v8::String::NewFromUtf8(isolate, "value").ToLocalChecked(), v8::String::NewFromUtf8(isolate, record->txt.str).ToLocalChecked()).Check();
			break;
		case UV_DNS_CNAME:
			obj->Set(context, v8::String::NewFromUtf8(isolate, "value").ToLocalChecked(), v8::String::NewFromUtf8(isolate, record->cname.host).ToLocalChecked()).Check();
			break;
		case UV_DNS_NS:
			obj->Set(context, v8::String::NewFromUtf8(isolate, "value").ToLocalChecked(), v8::String::NewFromUtf8(isolate, record->ns.host).ToLocalChecked()).Check();
			break;
		case UV_DNS_SRV:
			obj->Set(context, v8::String::NewFromUtf8(isolate, "priority").ToLocalChecked(), v8::Integer::New(isolate, record->srv.priority)).Check();
			obj->Set(context, v8::String::NewFromUtf8(isolate, "weight").ToLocalChecked(), v8::Integer::New(isolate, record->srv.weight)).Check();
			obj->Set(context, v8::String::NewFromUtf8(isolate, "port").ToLocalChecked(), v8::Integer::New(isolate, record->srv.port)).Check();
			obj->Set(context, v8::String::NewFromUtf8(isolate, "target").ToLocalChecked(), v8::String::NewFromUtf8(isolate, record->srv.target).ToLocalChecked()).Check();
			break;
		case UV_DNS_PTR:
			obj->Set(context, v8::String::NewFromUtf8(isolate, "value").ToLocalChecked(), v8::String::NewFromUtf8(isolate, record->ptr.host).ToLocalChecked()).Check();
			break;
		default:
			obj->Set(context, v8::String::NewFromUtf8(isolate, "value").ToLocalChecked(), v8::String::NewFromUtf8(isolate, "Unknown record type").ToLocalChecked()).Check();
	}
	return obj;
}


void OnDNSCallback(uv_dns_query_t* req, int status, uv_dns_query_result_t* res) {
	DNSContext* context = static_cast<DNSContext*>(req->data);
	context->result = res; // Store for parsing, dtor will free it
	
	if (status < 0) {
		context->ResumeError(status, "dns_query", context->hostname.c_str());
	} else {
		v8::Isolate* isolate = context->fiber->isolate();
		v8::HandleScope handle_scope(isolate);
		v8::Local<v8::Context> v8_context = isolate->GetCurrentContext();
		
		v8::Local<v8::Array> records = v8::Array::New(isolate, res->record_count);
		for (int i = 0; i < res->record_count; ++i) {
			records->Set(v8_context, i, ParseDNSRecord(isolate, &res->records[i])).Check();
		}
		context->Resume(records);
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

