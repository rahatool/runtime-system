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
	handle->id = id; // Store the ID on the handle itself
	store[id] = handle;
	return id;
}

void HandleStore::Remove(uint64_t id) {
	// Erasing the shared_ptr from the map triggers its destructor,
	// which in turn calls the destructor for the C++ handle object
	// (e.g., ~KeyHandle, ~TLSHandle), freeing the C resource.
	store.erase(id);
}

// JS primitive: primordials.handles.free(id)
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

