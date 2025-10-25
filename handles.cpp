#include "handles.h"
#include "primitives.h" // For SET_METHOD

std::map<uint64_t, std::shared_ptr<BaseHandle>> HandleStore::store;
uint64_t HandleStore::next_id;

void HandleStore::Init() {
	next_id = 1;
	store.clear();
}

void HandleStore::Dispose() {
	std::vector<uint64_t> ids_to_remove;
	for (auto const& [id, handle] : store) {
		if (handle) {
			 try { handle->Close(); } catch (...) { /* Ignore */ }
		}
		ids_to_remove.push_back(id);
	}
	 for(uint64_t id : ids_to_remove) { store.erase(id); }
	store.clear();
}

uint64_t HandleStore::Add(std::shared_ptr<BaseHandle> handle) {
	uint64_t id = next_id++;
	store[id] = handle;
	// std::cout << "Added handle: " << id << ", type: " << typeid(*handle).name() << ", total: " << store.size() << std::endl; // Debugging
	return id;
}

void HandleStore::Remove(uint64_t id) {
	auto it = store.find(id);
	if (it != store.end()) {
		// std::cout << "Removing handle: " << id << ", type: " << typeid(*(it->second)).name() << ", remaining: " << store.size() - 1 << std::endl; // Debugging
		store.erase(it); // shared_ptr dtor calls C++ dtor (e.g., EVP_PKEY_free)
	} else {
		// std::cout << "Attempted to remove non-existent handle: " << id << std::endl; // Debugging
	}
}

uint64_t HandleStore::FindId(std::shared_ptr<BaseHandle> handle_to_find) {
	if (!handle_to_find) return 0;
	for (const auto& [id, handle] : store) {
		if (handle == handle_to_find) {
			return id;
		}
	}
	return 0; // Not found
}

// --- Primitives ---
// Generic function called by FinalizationRegistry
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

