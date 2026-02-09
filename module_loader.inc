#ifndef MODULE_LOADER_INC
#define MODULE_LOADER_INC

#include <string>
#include <map>
#include <filesystem>
#include "v8.h"
#include "primitives.h" // For ReportException

struct RuntimeState {
	v8::Isolate* isolate;
	std::map<std::string, v8::Persistent<v8::Module>> module_cache;
};

bool ReadFileSync(const std::string& path, std::string* content) {
	std::ifstream file(path, std::ios::in | std::ios::binary);
	if (!file) return false;
	std::stringstream buffer;
	buffer << file.rdbuf();
	*content = buffer.str();
	return true;
}

v8::MaybeLocal<v8::Module> ResolveModuleCallback(
	v8::Local<v8::Context> context,
	v8::Local<v8::String> specifier,
	v8::Local<v8::FixedArray> import_assertions,
	v8::Local<v8::Module> referrer) {

	v8::Isolate* isolate = v8::Isolate::GetCurrent();
	v8::TryCatch try_catch(isolate);
	RuntimeState* state = static_cast<RuntimeState*>(isolate->GetData(0));
	v8::String::Utf8Value specifier_str(isolate, specifier);
    // Find referrer path by reverse lookup in cache
    std::string ref_path;
    for (auto& it : state->module_cache) {
        if (it.second.Get(isolate) == referrer) { ref_path = it.first; break; }
    }
    v8::String::Utf8Value referrer_path_str(isolate, v8::String::NewFromUtf8(isolate, ref_path.c_str()).ToLocalChecked());
	
	std::filesystem::path referrer_path(*referrer_path_str);
	std::filesystem::path absolute_path;
	try {
		if (std::string(*specifier_str).rfind("./", 0) == 0) {
			 absolute_path = std::filesystem::canonical(referrer_path.parent_path() / *specifier_str);
		} else {
			 absolute_path = std::filesystem::canonical(*specifier_str);
		}
	} catch(const std::filesystem::filesystem_error& e) {
		std::string err_msg = "Module not found: " + std::string(*specifier_str);
		isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(isolate, err_msg.c_str()).ToLocalChecked()));
		return v8::MaybeLocal<v8::Module>();
	}
	std::string path_key = absolute_path.string();

	if (state->module_cache.count(path_key)) {
		return state->module_cache[path_key].Get(isolate);
	}

	std::string source_code;
	if (!ReadFileSync(path_key, &source_code)) {
		std::string err_msg = "Module not found: " + path_key;
		isolate->ThrowException(v8::Exception::Error(v8::String::NewFromUtf8(isolate, err_msg.c_str()).ToLocalChecked()));
		return v8::MaybeLocal<v8::Module>();
	}

    v8::Local<v8::String> source_v8 = v8::String::NewFromUtf8(isolate, source_code.c_str(), v8::NewStringType::kNormal, source_code.length()).ToLocalChecked();
    v8::Local<v8::String> res_name = v8::String::NewFromUtf8(isolate, path_key.c_str()).ToLocalChecked();
    v8::ScriptOrigin origin(res_name, 0, 0, false, -1, v8::Local<v8::Value>(), false, false, true, v8::Local<v8::Data>());
	v8::ScriptCompiler::Source source(source_v8, origin);
	
	v8::MaybeLocal<v8::Module> module = v8::ScriptCompiler::CompileModule(isolate, &source);
	if (module.IsEmpty()) {
		if(try_catch.HasCaught()) ReportException(isolate, &try_catch);
		return v8::MaybeLocal<v8::Module>();
	}

	v8::Local<v8::Module> local_module = module.ToLocalChecked();
	state->module_cache[path_key].Reset(isolate, local_module);
	return local_module;
}

void LoadAndRunModules(v8::Local<v8::Context> context, const char* entry_path_str, v8::TryCatch* try_catch) {
	v8::Isolate* isolate = v8::Isolate::GetCurrent();
	RuntimeState* state = static_cast<RuntimeState*>(isolate->GetData(0));

	auto load_module = [&](const std::string& path) -> v8::Local<v8::Module> {
		std::string abs_path;
		try {
			abs_path = std::filesystem::canonical(path).string();
		} catch(const std::filesystem::filesystem_error& e) {
			std::cerr << "Fatal: Failed to find canonical path for " << path << "\n";
			exit(1);
		}
		
		std::string code;
		if (!ReadFileSync(abs_path, &code)) {
			std::cerr << "Fatal: Failed to read " << path << "\n";
			exit(1);
		}
        v8::Local<v8::String> res_name = v8::String::NewFromUtf8(isolate, abs_path.c_str()).ToLocalChecked();
        v8::ScriptOrigin origin(res_name, 0, 0, false, -1, v8::Local<v8::Value>(), false, false, true, v8::Local<v8::Data>());
		v8::ScriptCompiler::Source source(v8::String::NewFromUtf8(isolate, code.c_str(), v8::NewStringType::kNormal, code.length()).ToLocalChecked(), origin);
		v8::MaybeLocal<v8::Module> module = v8::ScriptCompiler::CompileModule(isolate, &source);
		if (module.IsEmpty()) {
			ReportException(isolate, try_catch);
			exit(1);
		}
		v8::Local<v8::Module> local_module = module.ToLocalChecked();
		state->module_cache[abs_path].Reset(isolate, local_module);
		return local_module;
	};
	
    v8::Local<v8::Module> entry_module = load_module(entry_path_str);

    if (entry_module->InstantiateModule(context, ResolveModuleCallback).IsNothing()) {
		ReportException(isolate, try_catch);
		exit(1);
	}

	if (entry_module->Evaluate(context).IsEmpty()) {
		ReportException(isolate, try_catch);
		exit(1);
	}
}

#endif // MODULE_LOADER_INC

