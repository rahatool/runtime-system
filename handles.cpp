#include "handles.h"
#include "primitives.h"

std::map<uint64_t, std::shared_ptr<BaseHandle>> HandleStore::store;
uint64_t HandleStore::next_id;

void HandleStore::Init() {
	next_id = 1;
	store.clear();
}

void HandleStore::Dispose() {
	// Close all remaining handles
	for (auto const& [id, handle] : store) {
		handle->Close();
	}
	store.clear();
}

uint64_t HandleStore::Add(std::shared_ptr<BaseHandle> handle) {
	uint64_t id = next_id++;
	store[id] = handle;
	return id;
}

bool HandleStore::Remove(uint64_t id) {
	auto it = store.find(id);
	if (it != store.end()) {
		// The shared_ptr destructor will call the handle's destructor
		// which should free the underlying resource (SSL_CTX_free, EVP_PKEY_free, etc.)
		// For uv handles, Close() must be called first.
		it->second->Close(); 
		store.erase(it);
		return true;
	}
	return false;
}

// --- Primitive for FinalizationRegistry ---
void HANDLES_Free(const v8::FunctionCallbackInfo<v8::Value>& args) {
	uint64_t id = args[0].As<v8::BigInt>()->Uint64Value();
	HandleStore::Remove(id);
}

void InitializeHandles(v8::Isolate* isolate, v8::Local<v8::Object> exports) {
	v8::Local<v8::Context> context = isolate->GetCurrentContext();
	v8::Local<v8::Object> handles_obj = v8::Object::New(isolate);
	SET_METHOD(handles_obj, "free", HANDLES_Free);
	exports->Set(context, v8::String::NewFromUtf8(isolate, "handles").ToLocalChecked(), handles_obj).Check();
}

