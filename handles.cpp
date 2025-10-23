#include "handles.h"

std::map<uint64_t, std::shared_ptr<BaseHandle>> HandleStore::store;
uint64_t HandleStore::next_id;

void HandleStore::Init() {
	next_id = 1;
	store.clear();
}

void HandleStore::Dispose() {
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

void HandleStore::Remove(uint64_t id) {
	store.erase(id);
}