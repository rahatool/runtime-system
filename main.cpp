#include <iostream>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <map>
#include <filesystem>
#include <queue>
#include <mutex>
#include "libplatform/libplatform.h"
#include "v8.h"
#include "uv.h"
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "fiber.h"
#include "primitives.h"
#include "handles.h"

// --- Global state for async fiber resumption ---
std::queue<AsyncContext*> resumable_fibers;
std::mutex queue_mutex;
uv_async_t resume_handle;
// ---

// Forward declarations for primitives
void InitializeFS(v8::Isolate* isolate, v8::Local<v8::Object> exports);
void InitializeTCP(v8::Isolate* isolate, v8::Local<v8::Object> exports);
void InitializeUDP(v8::Isolate* isolate, v8::Local<v8::Object> exports);
void InitializeDNS(v8::Isolate* isolate, v8::Local<v8::Object> exports);
void InitializeTLS(v8::Isolate* isolate, v8::Local<v8::Object> exports);
void InitializeEncoding(v8::Isolate* isolate, v8::Local<v8::Object> exports);
void InitializeFibers(v8::Isolate* isolate, v8::Local<v8::Object> exports);
void InitializeHandles(v8::Isolate* isolate, v8::Local<v8::Object> exports);

#include "module_loader.inc"

// This is the single, safe entry point for resuming fibers from async callbacks.
// It runs on the main event loop stack.
void OnFibersToResume(uv_async_t* handle) {
	v8::Isolate* isolate = static_cast<v8::Isolate*>(handle->data);

	// Establish a valid V8 context before touching any V8 APIs.
	v8::Isolate::Scope isolate_scope(isolate);
	v8::HandleScope handle_scope(isolate);
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Context::Scope context_scope(context);

	// Drain the concurrent queue into a local one to minimize lock time.
	std::queue<AsyncContext*> local_queue;
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		resumable_fibers.swap(local_queue);
	}

	// Process all completed operations.
	while(!local_queue.empty()) {
		AsyncContext* async_context = local_queue.front();
		local_queue.pop();

		Fiber* fiber = async_context->fiber;
		
		// Let the context object create the appropriate V8 result value.
		v8::Local<v8::Value> result_val = async_context->CreateResultValue(isolate);
		
		// Set the result and resume the waiting fiber.
		fiber->resume_value.Reset(isolate, result_val);
		Fiber::resume(fiber);

		delete async_context;
	}
}

// Any libuv callback will call this function instead of directly resuming a fiber.
void QueueFiberToResume(AsyncContext* context) {
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		resumable_fibers.push(context);
	}
	// This wakes up the uv_run loop, which will then execute OnFibersToResume.
	uv_async_send(&resume_handle);
}

int main(int argc, char* argv[]) {
	if (argc < 2) {
		std::cerr << "Usage: " << argv[0] << " <entry_module.mjs>" << std::endl;
		return 1;
	}

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

		// Initialize our async resumption mechanism before anything else.
		uv_async_init(uv_default_loop(), &resume_handle, OnFibersToResume);
		resume_handle.data = isolate; // Give the callback access to the isolate

		Fiber::init(isolate, uv_default_loop());
		Fiber::set_main_context(context);
		HandleStore::Init();

		v8::Local<v8::Object> primordials = v8::Object::New(isolate);
		InitializeFS(isolate, primordials);
		InitializeTCP(isolate, primordials);
		InitializeUDP(isolate, primordials);
		InitializeDNS(isolate, primordials);
		InitializeTLS(isolate, primordials);
		InitializeEncoding(isolate, primordials);
		InitializeFibers(isolate, primordials);
		InitializeHandles(isolate, primordials);

		context->Global()->Set(context, v8::String::NewFromUtf8(isolate, "__primordials").ToLocalChecked(), primordials).Check();
		
		v8::TryCatch try_catch(isolate);
		LoadAndRunModules(context, argv[1], &try_catch);
		
		// This loop now processes both I/O events and our fiber resumption events.
		uv_run(Fiber::get_loop(), UV_RUN_DEFAULT);
		
		uv_close((uv_handle_t*)&resume_handle, nullptr);
		// Run the loop one final time to allow the close event to be processed.
		uv_run(Fiber::get_loop(), UV_RUN_ONCE);
		
		HandleStore::Dispose();
	}

	isolate->Dispose();
	v8::V8::Dispose();
	v8::V8::DisposePlatform();
	delete create_params.array_buffer_allocator;

	return 0;
}