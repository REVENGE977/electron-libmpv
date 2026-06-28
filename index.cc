#include <napi.h>
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>
#include <string>
#include <vector>
#include <mpv/client.h>

#ifdef _WIN32
#include <windows.h>
#endif

class MpvPlayer : public Napi::ObjectWrap<MpvPlayer> {
	public:
		static Napi::Object Init(Napi::Env env, Napi::Object exports);
		explicit MpvPlayer(const Napi::CallbackInfo& info);
		virtual ~MpvPlayer();
	
	private:
		Napi::Value Command(const Napi::CallbackInfo& info);
		Napi::Value SetProperty(const Napi::CallbackInfo& info);
		Napi::Value GetProperty(const Napi::CallbackInfo& info);
		Napi::Value Attach(const Napi::CallbackInfo& info);
		Napi::Value Resize(const Napi::CallbackInfo& info);
	
	static void OnMpvEvents(void* ctx);
	
	mpv_handle* mpv_;
	Napi::ThreadSafeFunction event_fn_;
	
	#ifdef _WIN32
	HWND sub_hwnd_ = nullptr;
	HWND parent_hwnd_ = nullptr;
	#endif
};

Napi::Object MpvPlayer::Init(Napi::Env env, Napi::Object exports) {
	Napi::Function func = DefineClass(env, "MpvPlayer", {
		InstanceMethod("command", &MpvPlayer::Command),
		InstanceMethod("setProperty", &MpvPlayer::SetProperty),
		InstanceMethod("getProperty", &MpvPlayer::GetProperty),
		InstanceMethod("attach", &MpvPlayer::Attach),
		InstanceMethod("resize", &MpvPlayer::Resize)
	});
	
	Napi::FunctionReference* constructor = new Napi::FunctionReference();
	*constructor = Napi::Persistent(func);
	env.SetInstanceData(constructor);
	
	exports.Set("MpvPlayer", func);
	return exports;
}

MpvPlayer::MpvPlayer(const Napi::CallbackInfo& info) : Napi::ObjectWrap<MpvPlayer>(info), mpv_(nullptr) {
	Napi::Env env = info.Env();
	Napi::Object options = info[0].ToObject();
	Napi::Function onEventCall = options.Get("onEvent").As<Napi::Function>();
	
	event_fn_ = Napi::ThreadSafeFunction::New(env, onEventCall, "MpvEvents", 0, 1);
	
	setlocale(LC_NUMERIC, "C");
	mpv_ = mpv_create();
}

MpvPlayer::~MpvPlayer() {
	#ifdef _WIN32
	if (sub_hwnd_) DestroyWindow(sub_hwnd_);
	#endif
	if (mpv_) mpv_terminate_destroy(mpv_);
	if (event_fn_) event_fn_.Release();
}

void MpvPlayer::OnMpvEvents(void* ctx) {
	MpvPlayer* self = static_cast<MpvPlayer*>(ctx);
	if (!self || !self->event_fn_) return;
	
	bool has_events = false;
	while (mpv_event* event = mpv_wait_event(self->mpv_, 0)) {
		if (event->event_id == MPV_EVENT_NONE) break;
		has_events = true;
	}
	
	if (has_events) {
		auto callback = [](Napi::Env env, Napi::Function jsCallback) {
			if (jsCallback && !jsCallback.IsEmpty() && jsCallback.IsFunction()) {
				jsCallback.Call({});
			}
		};
		self->event_fn_.NonBlockingCall(callback);
	}
}

Napi::Value MpvPlayer::Attach(const Napi::CallbackInfo& info) {
	Napi::Env env = info.Env();
	#ifdef _WIN32
	if (info.Length() < 1 || !info[0].IsBuffer()) return Napi::Boolean::New(env, false);
	
	Napi::Buffer<uint8_t> buf = info[0].As<Napi::Buffer<uint8_t>>();
	parent_hwnd_ = *reinterpret_cast<HWND*>(buf.Data());
	
	int x = info.Length() > 1 ? info[1].As<Napi::Number>().Int32Value() : 0;
	int y = info.Length() > 2 ? info[2].As<Napi::Number>().Int32Value() : 0;
	int w = info.Length() > 3 ? info[3].As<Napi::Number>().Int32Value() : 640;
	int h = info.Length() > 4 ? info[4].As<Napi::Number>().Int32Value() : 480;
	
	LONG style = GetWindowLongA(parent_hwnd_, GWL_STYLE);
	SetWindowLongA(parent_hwnd_, GWL_STYLE, style | WS_CLIPCHILDREN);
	
	sub_hwnd_ = CreateWindowExA(
		0, "Static", "",
		WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
		x, y, w, h,
		parent_hwnd_,
		nullptr, nullptr, nullptr
	);
	
	SetWindowPos(sub_hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
	
	int64_t wid = reinterpret_cast<int64_t>(sub_hwnd_);
	mpv_set_option(mpv_, "wid", MPV_FORMAT_INT64, &wid);
	
	if (mpv_initialize(mpv_) < 0) {
		return Napi::Boolean::New(env, false);
	}
	
	mpv_set_option_string(mpv_, "vo", "gpu-next"); 
	mpv_set_option_string(mpv_, "gpu-api", "d3d11");
	mpv_set_option_string(mpv_, "hwdec", "d3d11va"); 
	mpv_set_option_string(mpv_, "osc", "no");
	mpv_set_option_string(mpv_, "keep-open", "yes");
	
	return Napi::Boolean::New(env, true);
	#else
	return Napi::Boolean::New(env, false);
	#endif
}

Napi::Value MpvPlayer::Resize(const Napi::CallbackInfo& info) {
	Napi::Env env = info.Env();
	#ifdef _WIN32
	if (sub_hwnd_ && info.Length() >= 4) {
		int x = info[0].As<Napi::Number>().Int32Value();
		int y = info[1].As<Napi::Number>().Int32Value();
		int w = info[2].As<Napi::Number>().Int32Value();
		int h = info[3].As<Napi::Number>().Int32Value();
		
		SetWindowPos(sub_hwnd_, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
	}
	#endif
	return env.Null();
}

Napi::Value MpvPlayer::GetProperty(const Napi::CallbackInfo& info) {
	Napi::Env env = info.Env();
	if (!mpv_ || info.Length() < 1) return env.Null();
	std::string name = info[0].ToString().Utf8Value();
	char* result = nullptr;
	if (mpv_get_property(mpv_, name.c_str(), MPV_FORMAT_STRING, &result) < 0 || !result) return env.Null();
	Napi::String jsResult = Napi::String::New(env, result);
	mpv_free(result);
	return jsResult;
}

Napi::Value MpvPlayer::Command(const Napi::CallbackInfo& info) {
	Napi::Env env = info.Env();
	if (!mpv_) return Napi::Number::New(env, -1);
	
	std::vector<std::string> args_str;
	for (size_t i = 0; i < info.Length(); i++) args_str.push_back(info[i].ToString().Utf8Value());
	
	std::vector<const char*> args_ptr;
	for (const auto& s : args_str) args_ptr.push_back(s.c_str());
	args_ptr.push_back(nullptr);
	
	int res = mpv_command_async(mpv_, 0, args_ptr.data());
	return Napi::Number::New(env, res);
}

Napi::Value MpvPlayer::SetProperty(const Napi::CallbackInfo& info) {
	Napi::Env env = info.Env();
	if (!mpv_ || info.Length() < 2) return Napi::Number::New(env, -1);
	return Napi::Number::New(env, mpv_set_property_string(mpv_, info[0].ToString().Utf8Value().c_str(), info[1].ToString().Utf8Value().c_str()));
}

Napi::Object InitModule(Napi::Env env, Napi::Object exports) { return MpvPlayer::Init(env, exports); }
NODE_API_MODULE(electron-libmpv, InitModule)