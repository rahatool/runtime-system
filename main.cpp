#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <map>
#include <filesystem>
#include "libplatform/libplatform.h"
#include "v8.h"
#include "uv.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "fiber.h"
#include "primitives.h"
#include "handles.h"

// Forward declarations for primitives
void InitializeFS(v8::Isolate* isolate, v8::Local<v8::Object> exports);
void InitializeNet(v8::Isolate* isolate, v8::Local<v8::Object> exports);
void InitializeDNS(v8::Isolate* isolate, v8::Local<v8::Object> exports);
void InitializeTLS(v8::Isolate* isolate, v8::Local<v8::Object> exports);
void InitializeFibers(v8::Isolate* isolate, v8::Local<v8::Object> exports);

#include "module_loader.inc"

int main(int argc, char* argv[]) {
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " <entry_module.mjs>" << std::endl;
		return 1;
	}

	// Initialize OpenSSL
	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();

	v8::V8::InitializeICUDefaultLocation(argv[0]);
	v8::V8::InitializeExternalStartupData(argv[0]);
	std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
	v8::V8::InitializePlatform(platform.get());
	v8::V8::Initialize();

	v8::Isolate::CreateParams create_params;
	create_params.array_buffer_allocator = v8::ArrayBuffer::Allocator::NewDefaultAllocator();
	v8::Isolate* isolate = v8::Isolate::New(create_params);
	{
		RuntimeState state;
		state.isolate = isolate;
		isolate->SetData(0, &state);

		v8::Isolate::Scope isolate_scope(isolate);
		v8::HandleScope handle_scope(isolate);
		v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);

		global->Set(
			v8::String::NewFromUtf8(isolate, "print").ToLocalChecked(),
			v8::FunctionTemplate::New(isolate, [](const v8::FunctionCallbackInfo<v8::Value>& args) {
				if (args.Length() > 0) {
					v8::String::Utf8Value str(args.GetIsolate(), args[0]);
					std::cout << *str;
				}
			})
		);

		v8::Local<v8::Context> context = v8::Context::New(isolate, NULL, global);
		v8::Context::Scope context_scope(context);

		Fiber::init(uv_default_loop());
		HandleStore::Init();

		// Create the single __primordials object
		v8::Local<v8::Object> primordials = v8::Object::New(isolate);
		InitializeFS(isolate, primordials);
		InitializeNet(isolate, primordials);
		InitializeDNS(isolate, primordials);
		InitializeTLS(isolate, primordials);
		InitializeFibers(isolate, primordials);

		context->Global()->Set(context, v8::String::NewFromUtf8(isolate, "__primordials").ToLocalChecked(), primordials).Check();
		
		v8::TryCatch try_catch(isolate);
		LoadAndRunModules(context, "stdlib.js", argv[1], &try_catch);
		
		uv_run(Fiber::get_loop(), UV_RUN_DEFAULT);
		
		HandleStore::Dispose();
	}

	isolate->Dispose();
	v8::V8::Dispose();
	v8::V8::DisposePlatform();
	delete create_params.array_buffer_allocator;

	return 0;
}